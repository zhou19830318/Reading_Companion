#include "AiImageActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <CloudConfig.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <VoiceRelayImageClient.h>
#include <WiFi.h>
#include <WifiIdleManager.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/voice/VoiceActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/SleepCover.h"

namespace {

constexpr const char* AI_DIR = "/ai-images";

StrId reasonFor(VoiceRelay::ImageClient::Result result) {
  switch (result) {
    case VoiceRelay::ImageClient::Result::Rejected:
      return StrId::STR_AI_IMAGE_REJECTED;
    case VoiceRelay::ImageClient::Result::NotConfigured:
      return StrId::STR_AI_IMAGE_NOT_CONFIGURED;
    case VoiceRelay::ImageClient::Result::Timeout:
      return StrId::STR_AI_IMAGE_TIMEOUT;
    case VoiceRelay::ImageClient::Result::NoWifi:
      return StrId::STR_STT_NO_NETWORK;
    case VoiceRelay::ImageClient::Result::File:
      return StrId::STR_AI_IMAGE_SAVE_FAILED;
    default:
      return StrId::STR_AI_IMAGE_FAILED;
  }
}

// Options rows: index -> provider value + label. Index 0 is 默认 and sends
// nothing (the server keeps its own default).
constexpr const char* STYLE_VALUES[] = {
    "", "photo", "portrait", "anime", "oil", "watercolor", "sketch", "chinese_ink", "flat illustration", "3d cartoon",
};
constexpr StrId STYLE_LABELS[] = {
    StrId::STR_OPT_DEFAULT, StrId::STR_STYLE_PHOTO,      StrId::STR_STYLE_PORTRAIT, StrId::STR_STYLE_ANIME,
    StrId::STR_STYLE_OIL,   StrId::STR_STYLE_WATERCOLOR, StrId::STR_STYLE_SKETCH,   StrId::STR_STYLE_INK,
    StrId::STR_STYLE_FLAT,  StrId::STR_STYLE_3D,
};
static_assert(sizeof(STYLE_VALUES) / sizeof(STYLE_VALUES[0]) == 10, "style table drift");
static_assert(sizeof(STYLE_LABELS) / sizeof(STYLE_LABELS[0]) == 10, "style table drift");

constexpr const char* SIZE_VALUES[] = {
    "",
    "1280*768",
    "1024*1024",
};
constexpr StrId SIZE_LABELS[] = {
    StrId::STR_OPT_DEFAULT,
    StrId::STR_SIZE_LANDSCAPE,
    StrId::STR_SIZE_SQUARE,
};
static_assert(sizeof(SIZE_VALUES) / sizeof(SIZE_VALUES[0]) == 3, "size table drift");
static_assert(sizeof(SIZE_LABELS) / sizeof(SIZE_LABELS[0]) == 3, "size table drift");

bool isSeparatorCodepoint(uint32_t cp) {
  if (cp < 0x21) return true;  // control + space
  if (cp < 0x7F) {
    return strchr("!,.:;?@[](){}<>\"'`/\\|&*-=~^%$#+", static_cast<int>(cp)) != nullptr;
  }
  // CJK / fullwidth punctuation and dashes/quotes/ellipsis.
  return (cp >= 0x2013 && cp <= 0x201F) || cp == 0x2026 || (cp >= 0x3000 && cp <= 0x303F) ||
         (cp >= 0xFE10 && cp <= 0xFE1F) || (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) ||
         (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65);
}

// FAT32 LFN-illegal bytes never appear in a keyword (they are separators).
uint32_t decodeUtf8(const char* s, size_t len, size_t& advance) {
  const uint8_t c0 = static_cast<uint8_t>(s[0]);
  advance = 1;
  if (c0 < 0x80) return c0;
  if ((c0 & 0xE0) == 0xC0 && len >= 2) {
    advance = 2;
    return ((c0 & 0x1FU) << 6) | (static_cast<uint8_t>(s[1]) & 0x3FU);
  }
  if ((c0 & 0xF0) == 0xE0 && len >= 3) {
    advance = 3;
    return ((c0 & 0x0FU) << 12) | ((static_cast<uint8_t>(s[1]) & 0x3FU) << 6) | (static_cast<uint8_t>(s[2]) & 0x3FU);
  }
  if ((c0 & 0xF8) == 0xF0 && len >= 4) {
    advance = 4;
    return ((c0 & 0x07U) << 18) | ((static_cast<uint8_t>(s[1]) & 0x3FU) << 12) |
           ((static_cast<uint8_t>(s[2]) & 0x3FU) << 6) | (static_cast<uint8_t>(s[3]) & 0x3FU);
  }
  return 0xFFFD;
}

// <keyword>_ai_%04d.bmp naming: the head of the prompt up to the first
// separator, at most 8 code points / 24 bytes (CJK-safe — never cuts a
// multi-byte sequence). Empty after cutting -> "ai".
std::string imageKeyword(const char* prompt) {
  std::string out;
  if (prompt == nullptr) return "ai";
  size_t i = 0;
  const size_t len = strlen(prompt);
  int codepoints = 0;
  while (i < len && codepoints < 8 && out.size() < 24) {
    size_t advance = 0;
    const uint32_t cp = decodeUtf8(prompt + i, len - i, advance);
    if (cp == 0xFFFD || advance == 0) break;
    if (isSeparatorCodepoint(cp)) break;
    out.append(prompt + i, advance);
    i += advance;
    ++codepoints;
  }
  return out.empty() ? std::string("ai") : out;
}

// Largest "<...>_ai_<n>.bmp" / legacy "ai_<n>.bmp" suffix number in `name`.
unsigned indexFromName(const char* name) {
  unsigned index = 0;
  if (const char* p = strstr(name, "_ai_")) {
    if (sscanf(p + 4, "%u.", &index) == 1) return index;
  }
  if (sscanf(name, "ai_%u.bmp", &index) == 1) return index;
  return 0;
}

}  // namespace

const char* AiImageActivity::styleValue() const {
  const char* v = STYLE_VALUES[styleIndex_];
  return v[0] != '\0' ? v : nullptr;
}

const char* AiImageActivity::sizeValue() const {
  const char* v = SIZE_VALUES[sizeIndex_];
  return v[0] != '\0' ? v : nullptr;
}

void AiImageActivity::onEnter() {
  Activity::onEnter();
  // Same network rule as the other relay front-ends: arm the idle shutdown
  // clock, so the radio does not sit up for as long as this screen is open
  // (AskAiActivity's pattern). The blocking round extends it by its nature —
  // the manager's poll cannot run while the loop is inside the call.
  WIFI_IDLE.arm();
  if (!Storage.ensureDirectoryExists(AI_DIR)) {
    LOG_ERR("AIIM", "cannot create %s", AI_DIR);
  }
  state_ = State::Menu;
  menuIndex = 0;
  requestUpdate();
}

void AiImageActivity::loop() {
  switch (state_) {
    case State::Working:
      // The blocking generate/download call owns the loop until it returns.
      break;

    case State::Menu: {
      buttonNavigator.onNext([this] {
        menuIndex = ButtonNavigator::nextIndex(menuIndex, MENU_ROWS);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        menuIndex = ButtonNavigator::previousIndex(menuIndex, MENU_ROWS);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (menuIndex == 0) {
          optionIndex = 0;
          state_ = State::Options;
          requestUpdate();
        } else {
          loadHistory();
          historyIndex = 0;
          state_ = State::History;
          requestUpdate();
        }
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
        return;
      }
      break;
    }

    case State::Options: {
      buttonNavigator.onNext([this] {
        optionIndex = ButtonNavigator::nextIndex(optionIndex, OPTIONS_ROWS);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        optionIndex = ButtonNavigator::previousIndex(optionIndex, OPTIONS_ROWS);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (optionIndex == 0) {
          styleIndex_ = (styleIndex_ + 1) % STYLE_COUNT;
          requestUpdate();
        } else if (optionIndex == 1) {
          sizeIndex_ = (sizeIndex_ + 1) % SIZE_COUNT;
          requestUpdate();
        } else {
          openEditor();
        }
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state_ = State::Menu;
        requestUpdate();
        return;
      }
      break;
    }

    case State::Viewing: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        applyCover();
        return;
      }
      // The hint bar's prev slot is 删除. It must bind to NavPrevious, not the
      // bare side Up: mapLabels paints the label over the front Left key, and
      // NavPrevious is the logical pair MappedInputManager resolves to side +
      // front (with the orientation swap) — the same pair Menu/History/
      // Options navigate with. Up alone left the printed button dead.
      if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
        state_ = State::ConfirmDelete;
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state_ = viewingFromHistory ? State::History : State::Menu;
        requestUpdate();
        return;
      }
      break;
    }

    case State::ConfirmDelete: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (!Storage.remove(currentPath.c_str())) {
          LOG_ERR("AIIM", "remove failed: %s", currentPath.c_str());
        } else {
          LOG_INF("AIIM", "deleted %s", currentPath.c_str());
        }
        if (viewingFromHistory) {
          loadHistory();
          if (historyNames.empty()) {
            historyIndex = 0;
          } else if (historyIndex >= static_cast<int>(historyNames.size())) {
            historyIndex = static_cast<int>(historyNames.size()) - 1;
          }
          state_ = State::History;
        } else {
          state_ = State::Menu;
        }
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state_ = State::Viewing;
        requestUpdate();
        return;
      }
      break;
    }

    case State::History: {
      if (historyNames.empty()) {
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          state_ = State::Menu;
          requestUpdate();
        }
        return;
      }
      buttonNavigator.onNext([this] {
        historyIndex = ButtonNavigator::nextIndex(historyIndex, static_cast<int>(historyNames.size()));
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        historyIndex = ButtonNavigator::previousIndex(historyIndex, static_cast<int>(historyNames.size()));
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        // Rows are displayed newest first: row 0 is the last name in the
        // ascending listing.
        currentPath = std::string(AI_DIR) + "/" + historyNames[historyNames.size() - 1 - historyIndex];
        viewingFromHistory = true;
        state_ = State::Viewing;
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state_ = State::Menu;
        requestUpdate();
        return;
      }
      break;
    }

    case State::Failed: {
      const bool canRetry = downloadPending || lastPrompt_[0] != '\0';
      if (canRetry && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        retry();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state_ = State::Menu;
        requestUpdate();
        return;
      }
      break;
    }
  }
}

void AiImageActivity::openEditor() {
  // VoiceActivity's question mode is the prompt front-end: speak (STT),
  // re-speak, or open the keyboard with the previous prompt prefilled
  // (再来一张 iterates on the previous prompt instead of retyping it).
  pendingAction_ = PendingAction::Generate;
  auto voice = std::make_unique<VoiceActivity>(renderer, mappedInput);
  voice->setQuestionInput(StrId::STR_AI_IMAGE, StrId::STR_AI_IMAGE_PROMPT,
                          lastPrompt_[0] != '\0' ? lastPrompt_ : nullptr);
  startActivityForResult(std::move(voice), [this](const ActivityResult& result) {
    // Plain Back delivers the default result (isCancelled=false, monostate):
    // std::get threw bad_variant_access there and aborted. get_if, not get —
    // the same contract VoiceActivity's other callers rely on.
    const auto* keyboard = std::get_if<KeyboardResult>(&result.data);
    if (keyboard == nullptr || keyboard->text.empty()) return;  // stays where it was
    snprintf(lastPrompt_, sizeof(lastPrompt_), "%s", keyboard->text.c_str());
    startRound(/*downloadOnly=*/false);
  });
}

bool AiImageActivity::relayConfigured() {
  if (SETTINGS.voiceRelayEnabled == 0) return false;
  CLOUD_CFG.load();  // NVS, idempotent; the same source the voice screen reads
  return CLOUD_CFG.relayUrl[0] != '\0' && CLOUD_CFG.relayToken[0] != '\0' && CLOUD_CFG.relayDeviceId[0] != '\0';
}

void AiImageActivity::startRound(bool downloadOnly) {
  if (!downloadOnly && !relayConfigured()) {
    LOG_ERR("AIIM", "relay not configured (voiceRelayEnabled=%d)", SETTINGS.voiceRelayEnabled);
    fail(StrId::STR_AI_IMAGE_NOT_CONFIGURED);
    return;
  }
  // downloadOnly overrides the action; otherwise openEditor()'s Generate
  // choice stands (retry() relies on that to re-run a failed round).
  if (downloadOnly) pendingAction_ = PendingAction::Download;
  if (WiFi.status() != WL_CONNECTED) {
    // The radio may have idled out while this screen sat open (WifiIdleManager
    // powers it down after 60 s of silence), or never came up. Bring it up the
    // way the voice capture does, then resume the round from the handler.
    LOG_INF("AIIM", "no network, opening Wi-Fi selection");
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                               fail(StrId::STR_STT_NO_NETWORK);
                               return;
                             }
                             resumeRound();
                           });
    return;
  }
  resumeRound();
}

void AiImageActivity::resumeRound() {
  state_ = State::Working;
  failReason_ = StrId::STR_AI_IMAGE_FAILED;
  // The blocking call below owns the loop for its whole duration: paint the
  // waiting frame first and wait for it (AskAiActivity's relay round).
  requestUpdateAndWait();
  switch (pendingAction_) {
    case PendingAction::Download:
      doDownload();
      break;
    case PendingAction::Generate:
      doGenerate();
      break;
  }
}

void AiImageActivity::doGenerate() {
  VoiceRelay::ImageClient::Stats stats;
  const auto result = VoiceRelay::ImageClient::generate(
      CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, lastPrompt_, lastMeta_,
      /*phrase=*/nullptr, /*phraseCap=*/0, &stats, styleValue(), sizeValue());
  if (result != VoiceRelay::ImageClient::Result::Ok) {
    LOG_ERR("AIIM", "generate failed: %s (http %d)", VoiceRelay::ImageClient::resultName(result), stats.httpStatus);
    downloadPending = false;
    fail(reasonFor(result));
    return;
  }
  LOG_INF("AIIM", "generated %s %dx%d %d bytes", lastMeta_.id, static_cast<int>(lastMeta_.width),
          static_cast<int>(lastMeta_.height), static_cast<int>(lastMeta_.bytes));
  downloadPending = true;
  doDownload();
}

void AiImageActivity::doDownload() {
  pendingPath = nextImagePath();
  const auto result = VoiceRelay::ImageClient::download(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken,
                                                        CLOUD_CFG.relayDeviceId, lastMeta_, pendingPath.c_str());
  if (result != VoiceRelay::ImageClient::Result::Ok) {
    LOG_ERR("AIIM", "download failed: %s (%s)", VoiceRelay::ImageClient::resultName(result), pendingPath.c_str());
    fail(result == VoiceRelay::ImageClient::Result::File ? StrId::STR_AI_IMAGE_SAVE_FAILED
                                                         : StrId::STR_AI_IMAGE_FAILED);
    return;
  }
  currentPath = pendingPath;
  viewingFromHistory = false;
  state_ = State::Viewing;
  requestUpdate();
}

void AiImageActivity::retry() {
  if (downloadPending) {
    // The render is already paid for and stored server-side: fetch it again.
    startRound(/*downloadOnly=*/true);
    return;
  }
  if (lastPrompt_[0] != '\0') startRound(/*downloadOnly=*/false);
}

void AiImageActivity::loadHistory() {
  historyNames.clear();
  auto dir = Storage.open(AI_DIR);
  if (dir && dir.isDirectory()) {
    char name[128];
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (!file.isDirectory()) {
        file.getName(name, sizeof(name));
        if (name[0] != '.') {
          const std::string entry(name);
          if (entry.size() > 4 && entry.compare(entry.size() - 4, 4, ".bmp") == 0) {
            historyNames.push_back(entry);
          }
        }
      }
    }
    dir.close();
  }
  FsHelpers::sortFileList(historyNames);  // ai_NNNN zero-pads, so name order is age order
  LOG_INF("AIIM", "history: %u images", static_cast<unsigned>(historyNames.size()));
}

std::string AiImageActivity::nextImagePath() {
  unsigned maxIndex = 0;
  auto dir = Storage.open(AI_DIR);
  if (dir && dir.isDirectory()) {
    char name[128];
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) continue;
      file.getName(name, sizeof(name));
      const unsigned index = indexFromName(name);
      if (index > maxIndex) maxIndex = index;
    }
    dir.close();
  }
  const std::string keyword = imageKeyword(lastPrompt_);
  char path[96];
  snprintf(path, sizeof(path), "%s/%s_ai_%04u.bmp", AI_DIR, keyword.c_str(), maxIndex + 1);
  return std::string(path);
}

void AiImageActivity::applyCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  if (SleepCover::apply(currentPath.c_str())) {
    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }
  delay(1000);
  requestUpdateAndWait();  // repaint the image over the popup
}

void AiImageActivity::fail(StrId reason) {
  state_ = State::Failed;
  failReason_ = reason;
  requestUpdate();
}

bool AiImageActivity::drawImage(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("AIIM", path, file)) return false;
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  int x = 0;
  int y = 0;
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
    if (ratio > screenRatio) {
      y = static_cast<int>(std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2));
    } else {
      x = static_cast<int>(std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2));
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
  // The viewer's prev slot becomes 删除: the side button acts on the picture
  // on screen instead of navigating.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), tr(STR_DELETE), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  return true;
}

void AiImageActivity::render(RenderLock&&) {
  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  switch (state_) {
    case State::Viewing:
      if (drawImage(currentPath.c_str())) return;
      // A deleted/corrupt file: paint the failure instead of a dead screen.
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_AI_IMAGE_OPEN_FAILED));
      {
        const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      }
      renderer.displayBuffer();
      return;

    case State::ConfirmDelete: {
      renderer.clearScreen();
      GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                     tr(STR_AI_IMAGE));
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_AI_IMAGE_DELETE_CONFIRM));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer();
      return;
    }

    case State::Working:
      renderer.clearScreen();
      GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                     tr(STR_AI_IMAGE));
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 3, tr(STR_AI_IMAGE_GENERATING));
      if (lastPrompt_[0] != '\0') {
        // Show what is being drawn, so a 30 s wait has context.
        int y = contentTop + contentHeight / 3 + metrics.verticalSpacing * 4;
        for (const auto& line :
             renderer.wrappedText(UI_10_FONT_ID, lastPrompt_, screen.width - 2 * metrics.contentSidePadding, 3)) {
          renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, y, line.c_str());
          y += renderer.getLineHeight(UI_10_FONT_ID);
        }
      }
      renderer.displayBuffer();
      return;

    case State::Failed: {
      renderer.clearScreen();
      GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                     tr(STR_AI_IMAGE));
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 3, I18N.get(failReason_));
      const bool canRetry = downloadPending || lastPrompt_[0] != '\0';
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), canRetry ? tr(STR_RETRY) : "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer();
      return;
    }

    case State::Options: {
      renderer.clearScreen();
      GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                     tr(STR_AI_IMAGE_NEW));
      GUI.drawList(
          renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, OPTIONS_ROWS, optionIndex,
          [this](int index) -> std::string {
            if (index == 0) {
              return std::string(tr(STR_AI_IMAGE_STYLE)) + ": " + I18N.get(STYLE_LABELS[styleIndex_]);
            }
            if (index == 1) {
              return std::string(tr(STR_AI_IMAGE_SIZE)) + ": " + I18N.get(SIZE_LABELS[sizeIndex_]);
            }
            return tr(STR_START);
          },
          nullptr, nullptr);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer();
      return;
    }

    case State::Menu:
    case State::History: {
      renderer.clearScreen();
      GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                     tr(STR_AI_IMAGE));

      if (state_ == State::Menu) {
        static constexpr StrId menuLabels[MENU_ROWS] = {StrId::STR_AI_IMAGE_NEW, StrId::STR_VOICE_HISTORY};
        GUI.drawList(
            renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, MENU_ROWS, menuIndex,
            [](int index) { return I18N.get(menuLabels[index]); }, nullptr,
            [](int index) { return index == 0 ? Image : Recent; });
        const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      } else if (historyNames.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_VOICE_HISTORY_EMPTY));
        const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      } else {
        const int count = static_cast<int>(historyNames.size());
        GUI.drawList(
            renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, count, historyIndex,
            [this, count](int index) { return historyNames[static_cast<size_t>(count - 1 - index)]; }, nullptr,
            [](int) { return Image; }, nullptr, false, nullptr);
        const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      }
      renderer.displayBuffer();
      return;
    }
  }
}
