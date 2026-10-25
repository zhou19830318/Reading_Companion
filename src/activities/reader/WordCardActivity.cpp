#include "WordCardActivity.h"

#include <CloudConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <VoiceRelayChatClient.h>
#include <WiFi.h>
#include <WifiIdleManager.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int LABEL_GAP = 8;                                           // label → content spacing when inline
constexpr int RULE_GAP = 10;                                           // hero rule spacing
constexpr char LABEL_START[] = "\xE3\x80\x90";                         // 【
constexpr char LABEL_END[] = "\xE3\x80\x91";                           // 】
constexpr char WORD_LABEL[] = "\xE3\x80\x90\xE8\xAF\x8D\xE3\x80\x91";  // 【词】
constexpr char EXAMPLE_LABEL[] = "\xE3\x80\x90\xE4\xBE\x8B\xE5\x8F\xA5\xE3\x80\x91";      // 【例句】
constexpr char TRANSLATION_LABEL[] = "\xE3\x80\x90\xE8\xAF\x91\xE6\x96\x87\xE3\x80\x91";  // 【译文】
}  // namespace

void WordCardActivity::onEnter() {
  Activity::onEnter();
  // Same radio policy as the dictionary screen: the relay round needs Wi-Fi,
  // and the idle manager powers it down once this screen stops working.
  WIFI_IDLE.arm();
  hero_ = word_;
  reply_ = makeUniqueNoThrow<char[]>(REPLY_CAP + 1);
  if (!reply_) {
    LOG_ERR("WORD", "OOM: reply buffer %u bytes", static_cast<unsigned>(REPLY_CAP + 1));
    failText_ = tr(STR_MIC_OOM);
    state_ = State::Failed;
  }
  requestUpdate();
}

void WordCardActivity::onExit() {
  reply_.reset();
  rows_.clear();
  Activity::onExit();
}

void WordCardActivity::beginLookup() {
  if (!reply_) {
    failText_ = tr(STR_MIC_OOM);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    failText_ = tr(STR_STT_NO_NETWORK);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  CLOUD_CFG.load();  // NVS, idempotent; the same source the voice screen reads
  const bool relayConfigured = SETTINGS.voiceRelayEnabled != 0 && CLOUD_CFG.relayUrl[0] != '\0' &&
                               CLOUD_CFG.relayToken[0] != '\0' && CLOUD_CFG.relayDeviceId[0] != '\0';
  if (!relayConfigured) {
    // Unlike the Hanzi card there is no local payload to fall back to.
    LOG_ERR("WORD", "relay not configured; word cards need the relay");
    failText_ = tr(STR_WORD_NO_RELAY);
    state_ = State::Failed;
    requestUpdate();
    return;
  }

  bool truncated = false;
  VoiceRelay::ChatClient::Stats stats;
  const VoiceRelay::ChatClient::Result result =
      VoiceRelay::ChatClient::ask(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, word_.c_str(),
                                  reply_.get(), REPLY_CAP + 1, truncated, nullptr, 0, "word", &stats);
  if (result == VoiceRelay::ChatClient::Result::Ok && reply_[0] != '\0') {
    if (truncated) {
      LOG_ERR("WORD", "reply truncated at %u bytes", static_cast<unsigned>(REPLY_CAP));
    }
    LOG_INF("WORD", "card for %s: %u bytes (http %d)", word_.c_str(), static_cast<unsigned>(stats.responseBytes),
            stats.httpStatus);
    rowsBuilt_ = false;
    scrollLine_ = 0;
    detailOpen_ = false;
    state_ = State::Card;
  } else {
    LOG_ERR("WORD", "lookup failed: %s (http %d)", VoiceRelay::ChatClient::resultName(result), stats.httpStatus);
    failText_ = tr(STR_WORD_FAILED);
    state_ = State::Failed;
  }
  requestUpdate();
}

void WordCardActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  switch (state_) {
    case State::Loading:
      if (!started_) {
        started_ = true;
        requestUpdateAndWait();  // paint the waiting frame before the blocking round trip
        beginLookup();
      }
      break;
    case State::Failed:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        started_ = false;
        state_ = State::Loading;
        failText_ = nullptr;
        requestUpdate();
      }
      break;
    case State::Card:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        toggleDetail();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        if (scrollLine_ > 0) {
          --scrollLine_;
          requestUpdate();
        }
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        if (scrollLine_ < maxScroll_) {
          ++scrollLine_;
          requestUpdate();
        }
      }
      break;
  }
}

void WordCardActivity::toggleDetail() {
  detailOpen_ = !detailOpen_;
  scrollLine_ = 0;
  requestUpdate();
}

void WordCardActivity::buildCard(const int fontId, const int textWidth) {
  rows_.clear();
  rowsBuilt_ = true;
  builtFontId_ = fontId;
  builtWidth_ = textWidth;
  hero_ = word_;
  if (!reply_) return;

  // Non-【】 lines continue the previous section's content.
  const auto appendWrapped = [&](const char* text) {
    if (text == nullptr || text[0] == '\0') return;
    auto wrapped = renderer.wrappedText(fontId, text, textWidth, MAX_WRAP_LINES);
    if (rows_.empty()) {
      Row row;
      row.content = std::move(wrapped);
      rows_.push_back(std::move(row));
    } else {
      auto& content = rows_.back().content;
      content.insert(content.end(), std::make_move_iterator(wrapped.begin()), std::make_move_iterator(wrapped.end()));
    }
  };

  const char* p = reply_.get();
  while (*p != '\0') {
    const char* nl = strchr(p, '\n');
    const size_t len = nl != nullptr ? static_cast<size_t>(nl - p) : strlen(p);
    size_t b = 0;
    while (b < len && (p[b] == ' ' || p[b] == '\t' || p[b] == '\r')) b++;
    size_t e = len;
    while (e > b && (p[e - 1] == ' ' || p[e - 1] == '\t' || p[e - 1] == '\r')) e--;

    if (e > b) {
      std::string line(p + b, e - b);
      if (line.rfind(LABEL_START, 0) == 0) {
        const size_t end = line.find(LABEL_END);
        if (end != std::string::npos) {
          const std::string label = line.substr(0, end + strlen(LABEL_END));
          std::string rest = line.substr(end + strlen(LABEL_END));
          const size_t rb = rest.find_first_not_of(" \t");
          rest = rb == std::string::npos ? std::string() : rest.substr(rb);
          if (label == WORD_LABEL) {
            // The card title wins over the requested form (e.g. abandon for
            // abandoning).
            if (!rest.empty()) hero_ = rest;
          } else {
            Row row;
            row.label = label;
            row.detail = label == EXAMPLE_LABEL || label == TRANSLATION_LABEL;
            if (!rest.empty()) {
              row.content = renderer.wrappedText(fontId, rest.c_str(), textWidth, MAX_WRAP_LINES);
            }
            rows_.push_back(std::move(row));
          }
          if (nl == nullptr) break;
          p = nl + 1;
          continue;
        }
      }
      appendWrapped(line.c_str());
    }
    if (nl == nullptr) break;
    p = nl + 1;
  }
}

void WordCardActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_WORD_CARD));

  // Reader font on purpose: the user's SD face is the only one guaranteed to
  // carry the IPA glyphs the card needs.
  const int bodyFont = SETTINGS.getReaderFontId();
  const int left = safe.x + metrics.contentSidePadding;
  const int right = safe.x + safe.width - metrics.contentSidePadding;
  const int textWidth = right - left;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;
  const int lineH = renderer.getLineHeight(bodyFont);

  const auto drawHero = [&]() {
    const int heroW = renderer.getTextWidth(bodyFont, hero_.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(bodyFont, left + (textWidth - heroW) / 2, contentTop, hero_.c_str(), true, EpdFontFamily::BOLD);
  };

  if (state_ == State::Loading) {
    drawHero();
    const int msgW = renderer.getTextWidth(bodyFont, tr(STR_WORD_LOOKING));
    renderer.drawText(bodyFont, left + (textWidth - msgW) / 2, contentTop + lineH + RULE_GAP * 2, tr(STR_WORD_LOOKING),
                      true);
  } else if (state_ == State::Failed) {
    drawHero();
    const int bodyTop = contentTop + lineH + RULE_GAP + 6;
    const int failW = renderer.getTextWidth(bodyFont, tr(STR_WORD_FAILED));
    renderer.drawText(bodyFont, left + (textWidth - failW) / 2, bodyTop, tr(STR_WORD_FAILED), true);
    if (failText_ != nullptr) {
      const int detailW = renderer.getTextWidth(bodyFont, failText_);
      renderer.drawText(bodyFont, left + (textWidth - detailW) / 2, bodyTop + lineH + 6, failText_, true);
    }
  } else {
    // Build before the hero draw so a returned 【词】 (canonical form) shows on
    // the first Card frame.
    if (!rowsBuilt_ || builtFontId_ != bodyFont || builtWidth_ != textWidth) {
      buildCard(bodyFont, textWidth);
    }
    drawHero();
    const int ruleY = contentTop + lineH + RULE_GAP / 2;
    renderer.drawLine(left, ruleY, right - 1, ruleY, true);
    renderer.drawLine(left, ruleY + 2, left + (right - left) / 3, ruleY + 2, true);
    const int bodyTop = contentTop + lineH + RULE_GAP + 6;
    const int bodyLineH = lineH + 2;

    const auto rowLines = [&](const Row& row) {
      if (row.detail && !detailOpen_) return 0;
      // One line for a lone label, label + content otherwise.
      return row.content.empty() ? 1 : 1 + static_cast<int>(row.content.size());
    };
    int totalLines = 0;
    for (const auto& row : rows_) totalLines += rowLines(row);

    const int visibleLines = bodyLineH > 0 ? (contentBottom - bodyTop) / bodyLineH : 0;
    maxScroll_ = std::max(0, totalLines - visibleLines);
    if (scrollLine_ > maxScroll_) scrollLine_ = maxScroll_;

    int y = bodyTop;
    int lineIdx = 0;
    for (const auto& row : rows_) {
      const int lines = rowLines(row);
      if (lines == 0) continue;
      if (lineIdx + lines <= scrollLine_) {
        lineIdx += lines;
        continue;
      }
      if (y > contentBottom) break;
      if (!row.label.empty()) {
        renderer.drawText(bodyFont, left, y, row.label.c_str(), true, EpdFontFamily::BOLD);
        y += bodyLineH;
      }
      for (const auto& contentLine : row.content) {
        if (y > contentBottom) break;
        renderer.drawText(bodyFont, left, y, contentLine.c_str(), true);
        y += bodyLineH;
      }
      lineIdx += lines;
    }
  }

  if (state_ == State::Card && maxScroll_ > 0) {
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
  const char* confirmLabel = "";
  if (state_ == State::Failed) {
    confirmLabel = tr(STR_RETRY);
  } else if (state_ == State::Card) {
    confirmLabel = detailOpen_ ? tr(STR_WORD_COLLAPSE) : tr(STR_WORD_DETAIL);
  }
  const char* scrollUp = state_ == State::Card && maxScroll_ > 0 ? tr(STR_DIR_UP) : "";
  const char* scrollDown = state_ == State::Card && maxScroll_ > 0 ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, scrollUp, scrollDown);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
