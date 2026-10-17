#include "DictionaryActivity.h"

#include <CloudConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <VoiceRelayChatClient.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int LABEL_GAP = 8;  // label → inline content spacing
constexpr int RULE_GAP = 10;  // hero rule spacing
constexpr int MAX_WRAP_LINES = 40;
constexpr char LABEL_END[] = "\xE3\x80\x91";                           // 】 — closes the 【...】 section marker
constexpr char HERO_LABEL[] = "\xE3\x80\x90\xE5\xAD\x97\xE3\x80\x91";  // 【字】
}  // namespace

void DictionaryActivity::onEnter() {
  Activity::onEnter();
  std::string ch;
  utf8AppendCodepoint(codepoint_, ch);
  strncpy(charBuf_, ch.c_str(), sizeof(charBuf_) - 1);
  heroChar_ = charBuf_;
  reply_ = makeUniqueNoThrow<char[]>(REPLY_CAP + 1);
  if (!reply_) {
    LOG_ERR("DICT", "OOM: reply buffer %u bytes", static_cast<unsigned>(REPLY_CAP + 1));
    failText_ = tr(STR_MIC_OOM);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  // Network first, then the request, then the reply: the entry point prompts
  // for WiFi before any waiting frame, so "查询中" is only ever painted once
  // the radio is up (or already was).
  if (WiFi.status() != WL_CONNECTED) {
    promptForWifi();
    return;
  }
  requestUpdate();
}

void DictionaryActivity::onExit() {
  // The session is process-wide: drop the notifier — it points at this object —
  // and never close the socket (the Ask-AI / VoiceActivity rule).
  session_.setNotifier(nullptr, nullptr);
  Activity::onExit();
}

void DictionaryActivity::loop() {
  // Pumps the shared socket in every state; on an unopened session poll() is
  // a no-op.
  session_.poll();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Finish on the release edge: the picker below treats a Back release as
    // "leave the picker", so handing it an orphaned edge would close the mode.
    finish();
    return;
  }

  if (state_ == State::Loading) {
    if (!started_) {
      started_ = true;
      // Paint the waiting frame before the blocking relay round (one question
      // at a time from the loop, the Ask-AI pattern).
      requestUpdateAndWait();
      beginLookup();
    }
    return;
  }

  if (state_ == State::Connecting) {
    if (session_.state() == OpenClaw::Session::State::Connected && !session_.reconnecting()) {
      sendDictPrompt();
      return;
    }
    if (session_.state() == OpenClaw::Session::State::Failed) {
      failText_ = session_.reason() != nullptr ? session_.reason() : tr(STR_OPENCLAW_DISCONNECTED);
      state_ = State::Failed;
      requestUpdate();
      return;
    }
    if (millis() - lastWaitTickMs_ >= CONNECT_TICK_MS) {
      lastWaitTickMs_ = millis();
      requestUpdate();
    }
    return;
  }

  if (state_ == State::Asking) {
    if (session_.awaitingReply()) {
      // Repaint exactly when the number the screen would print changes.
      const uint32_t shownSec = session_.awaitingMs() / 1000;
      if (shownSec != lastWaitShownSec_) {
        lastWaitShownSec_ = shownSec;
        requestUpdate();
      }
      return;
    }
    if (session_.replySize() > 0 && session_.outcome() == OpenClaw::Session::SendOutcome::Sent) {
      // Settle the card off the session's borrowed buffer: copy so the next
      // send or close cannot change what this card renders.
      const char* text = session_.replyText();
      snprintf(reply_.get(), REPLY_CAP + 1, "%s", text != nullptr ? text : "");
      if (session_.replyTruncated() || (text != nullptr && strlen(text) > REPLY_CAP)) {
        LOG_ERR("DICT", "reply truncated at %u bytes", static_cast<unsigned>(REPLY_CAP));
      }
      rowsBuilt_ = false;
      scrollLine_ = 0;
      state_ = State::Card;
      requestUpdate();
      return;
    }
    if (session_.state() == OpenClaw::Session::State::Connected) {
      // A dropped socket leaves state_ at Connected and only arms the
      // reconnect, so "the link is gone" has to be read from reconnecting()
      // and not from the state (the Ask-AI rule).
      failText_ = session_.reconnecting()
                      ? tr(STR_OPENCLAW_DISCONNECTED)
                      : (session_.lastError() != nullptr ? tr(STR_OPENCLAW_AGENT_FAILED) : tr(STR_OPENCLAW_NO_REPLY));
      state_ = State::Failed;
      requestUpdate();
      return;
    }
    if (session_.state() == OpenClaw::Session::State::Failed) {
      failText_ = session_.reason() != nullptr ? session_.reason() : tr(STR_OPENCLAW_DISCONNECTED);
      state_ = State::Failed;
      requestUpdate();
    }
    return;
  }

  if (state_ == State::Failed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state_ = State::Loading;
      started_ = false;
      requestUpdate();
    }
    return;
  }

  // Card: side Up/Down (and the mirrored front pair) scroll the entry.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (scrollLine_ > 0) {
      scrollLine_--;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (scrollLine_ < maxScroll_) {
      scrollLine_++;
      requestUpdate();
    }
    return;
  }
}

void DictionaryActivity::promptForWifi() {
  // Ask-at-send, matching Ask-AI: cancelling surfaces as a failed lookup whose
  // Confirm retry re-prompts — by then the radio usually reconnects from the
  // saved profile. `started_` stays set while the prompt is up so the Loading
  // branch does not fire a second prompt before this activity is pushed off.
  LOG_INF("DICT", "no WiFi; prompting for network");
  started_ = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             failText_ = tr(STR_STT_NO_NETWORK);
                             state_ = State::Failed;
                           } else {
                             // Re-enter the Loading path; the waiting frame is
                             // painted first, then the request goes out with
                             // the radio now up.
                             started_ = false;
                             state_ = State::Loading;
                           }
                           requestUpdate();
                         });
}

void DictionaryActivity::beginLookup() {
  if (!reply_) {
    // onEnter's OOM path left us without the reply buffer; retrying cannot
    // fix it and both backends write into it.
    failText_ = tr(STR_MIC_OOM);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    // Safety net: the radio dropped between the entry check and the send.
    // Covers both backends.
    promptForWifi();
    return;
  }

  CLOUD_CFG.load();  // NVS, idempotent; the same source the voice screen reads
  const bool relayConfigured = SETTINGS.voiceRelayEnabled != 0 && CLOUD_CFG.relayUrl[0] != '\0' &&
                               CLOUD_CFG.relayToken[0] != '\0' && CLOUD_CFG.relayDeviceId[0] != '\0';
  if (relayConfigured) {
    bool truncated = false;
    VoiceRelay::ChatClient::Stats stats;
    const VoiceRelay::ChatClient::Result result =
        VoiceRelay::ChatClient::ask(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, charBuf_,
                                    reply_.get(), REPLY_CAP + 1, truncated, "dict", &stats);
    if (result == VoiceRelay::ChatClient::Result::Ok) {
      if (truncated) {
        LOG_ERR("DICT", "reply truncated at %u bytes", static_cast<unsigned>(REPLY_CAP));
      }
      LOG_INF("DICT", "reply %u bytes (http %d)", static_cast<unsigned>(stats.responseBytes), stats.httpStatus);
      rowsBuilt_ = false;
      scrollLine_ = 0;
      state_ = State::Card;
      requestUpdate();
      return;
    }
    // Relay is the preferred service, not a gatekeeper: any failed round
    // (config class included) falls back to the OpenClaw gateway below.
    LOG_ERR("DICT", "relay lookup failed: %s (http %d), falling back to OpenClaw",
            VoiceRelay::ChatClient::resultName(result), stats.httpStatus);
  } else {
    LOG_INF("DICT", "relay not configured, using the OpenClaw gateway");
  }
  beginOpenClawLookup();
}

void DictionaryActivity::beginOpenClawLookup() {
  // Join a live link rather than re-dialling over it (startConnect() has no
  // connected guard), the Ask-AI rule.
  const auto sessionState = session_.state();
  if (sessionState == OpenClaw::Session::State::Connected || sessionState == OpenClaw::Session::State::Connecting ||
      sessionState == OpenClaw::Session::State::Handshake || sessionState == OpenClaw::Session::State::Challenge) {
    session_.setNotifier(this, &DictionaryActivity::onSessionNotifyTrampoline);
    state_ = State::Connecting;
    lastWaitTickMs_ = millis();
    requestUpdate();
    return;
  }
  if (!session_.loadConfig()) {
    failText_ = session_.reason() != nullptr ? session_.reason() : tr(STR_OPENCLAW_NO_CONFIG);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  session_.setNotifier(this, &DictionaryActivity::onSessionNotifyTrampoline);
  if (!session_.startConnect()) {
    failText_ = session_.reason() != nullptr ? session_.reason() : tr(STR_OPENCLAW_DISCONNECTED);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  state_ = State::Connecting;
  lastWaitTickMs_ = millis();
  requestUpdate();
}

bool DictionaryActivity::buildDictPrompt() {
  if (!prompt_) {
    prompt_ = makeUniqueNoThrow<char[]>(PROMPT_CAP);
    if (!prompt_) {
      LOG_ERR("DICT", "OOM: prompt buffer %u bytes", static_cast<unsigned>(PROMPT_CAP));
      return false;
    }
  }
  // Model-facing instruction, not UI text: the relay's dict system prompt
  // (relay/relay/config.py, llm_dict_system_prompt), restated inline because
  // the OpenClaw agent has its own system prompt. The 【】 line shape must
  // stay in lockstep with buildCard()'s parser and the relay's server-side
  // template.
  static constexpr char INSTRUCTION[] =
      "你是一本简明新华字典。用户发来一个汉字，你只输出这个字的条目，严格用下面格式"
      "（每项一行，不要 Markdown、不要解释、不要反问）：\n"
      "【字】<字>\n"
      "【拼音】<带声调的拼音；多音字按常用度列出，每个读音后跟一个词>\n"
      "【部首笔画】<部首>·<笔画数>画\n"
      "【释义】1. <释义> 2. <释义>\n"
      "【组词】<词1>、<词2>、<词3>\n"
      "【例句】<一句用到该字的话>\n"
      "内容务必准确精炼：释义最多 3 条，组词 3~5 个，全文不超过 300 字；"
      "若输入不是单个汉字，输出【字】无法识别。\n";
  snprintf(prompt_.get(), PROMPT_CAP, "%s%s", INSTRUCTION, charBuf_);
  return true;
}

void DictionaryActivity::sendDictPrompt() {
  if (!buildDictPrompt()) {
    failText_ = tr(STR_MIC_OOM);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  if (session_.sendMessage(prompt_.get()) == OpenClaw::Session::SendOutcome::Failed) {
    failText_ = tr(STR_OPENCLAW_SEND_FAILED);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  lastWaitShownSec_ = session_.awaitingMs() / 1000;
  state_ = State::Asking;
  requestUpdate();
}

const char* DictionaryActivity::phaseText() const {
  using SessionState = OpenClaw::Session::State;
  if (session_.reconnecting()) return tr(STR_OPENCLAW_DISCONNECTED);
  switch (session_.state()) {
    case SessionState::Connecting:
      return tr(STR_OPENCLAW_CONNECTING);
    case SessionState::Handshake:
      return tr(STR_OPENCLAW_HANDSHAKE);
    case SessionState::Challenge:
      return tr(STR_OPENCLAW_CHALLENGE);
    case SessionState::Connected:
      return tr(STR_OPENCLAW_CONNECTED);
    case SessionState::Failed:
      return session_.reason() != nullptr ? session_.reason() : tr(STR_OPENCLAW_DISCONNECTED);
    case SessionState::Idle:
    default:
      return tr(STR_OPENCLAW_CONNECTING);
  }
}

void DictionaryActivity::onSessionNotifyTrampoline(void* ctx) {
  static_cast<DictionaryActivity*>(ctx)->onSessionNotify();
}

void DictionaryActivity::onSessionNotify() {
  // Session callbacks fire from inside the WebSocket dispatch: repaint only.
  if (state_ == State::Connecting || state_ == State::Asking) requestUpdate();
}

void DictionaryActivity::buildCard(const int fontId, const int textWidth) {
  rows_.clear();
  if (!reply_) return;

  const auto pushRow = [&](std::string label, const char* rest) {
    CardRow row;
    row.label = std::move(label);
    if (rest != nullptr && rest[0] != '\0') {
      if (row.label.empty()) {
        row.content = renderer.wrappedText(fontId, rest, textWidth, MAX_WRAP_LINES);
      } else {
        const int labelW = renderer.getTextWidth(fontId, row.label.c_str());
        const int restW = renderer.getTextWidth(fontId, rest);
        if (labelW + LABEL_GAP + restW <= textWidth) {
          row.inlineContent = true;
          row.content.emplace_back(rest);
        } else {
          row.content = renderer.wrappedText(fontId, rest, textWidth, MAX_WRAP_LINES);
        }
      }
    }
    if (!row.label.empty() || !row.content.empty()) rows_.push_back(std::move(row));
  };

  const char* p = reply_.get();
  while (*p != '\0') {
    const char* nl = strchr(p, '\n');
    size_t len = nl != nullptr ? static_cast<size_t>(nl - p) : strlen(p);
    size_t b = 0;
    while (b < len && (p[b] == ' ' || p[b] == '\t' || p[b] == '\r')) b++;
    while (len > b && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r')) len--;

    if (len > b) {
      std::string line(p + b, len - b);
      if (line.rfind("\xE3\x80\x90", 0) == 0) {  // starts with 【
        const size_t end = line.find(LABEL_END);
        if (end != std::string::npos) {
          const std::string label = line.substr(0, end + strlen(LABEL_END));
          const std::string rest = line.substr(end + strlen(LABEL_END));
          if (label == HERO_LABEL) {
            // The hero glyph is drawn separately above the card body.
            std::string hero = rest;
            const size_t hb = hero.find_first_not_of(" \t");
            if (hb != std::string::npos) heroChar_ = hero.substr(hb);
          } else {
            pushRow(label, rest.c_str());
          }
          if (nl == nullptr) break;
          p = nl + 1;
          continue;
        }
      }
      pushRow(std::string(), line.c_str());
    }
    if (nl == nullptr) break;
    p = nl + 1;
  }
}

void DictionaryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_DICTIONARY));

  // Same face as the Ask-AI and voice screens: the reader font, so CJK glyphs
  // come from the user's own SD font rather than a fallback-less builtin.
  const int bodyFont = SETTINGS.getReaderFontId();

  const int left = safe.x + metrics.contentSidePadding;
  const int right = safe.x + safe.width - metrics.contentSidePadding;
  const int textWidth = right - left;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;

  const int heroLineH = renderer.getLineHeight(bodyFont);
  const int bodyLineH = heroLineH;

  const auto drawHero = [&]() {
    const int heroW = renderer.getTextWidth(bodyFont, heroChar_.c_str());
    renderer.drawText(bodyFont, left + (textWidth - heroW) / 2, contentTop, heroChar_.c_str(), true);
  };

  if (state_ == State::Loading || state_ == State::Connecting || state_ == State::Asking) {
    drawHero();
    // Loading keeps the old bare line ("查询中…"), the OpenClaw phases name
    // themselves; Asking counts the 120 s chat timeout down like Ask-AI.
    char wait[48];
    const char* line = tr(STR_DICT_LOOKING);
    if (state_ == State::Connecting) {
      line = phaseText();
    } else if (state_ == State::Asking) {
      snprintf(wait, sizeof(wait), "%s %u/%us", tr(STR_OPENCLAW_WAITING),
               static_cast<unsigned>(session_.awaitingMs() / 1000),
               static_cast<unsigned>(OpenClaw::Session::CHAT_TIMEOUT_MS / 1000));
      line = wait;
    }
    const int msgW = renderer.getTextWidth(bodyFont, line);
    renderer.drawText(bodyFont, left + (textWidth - msgW) / 2, contentTop + heroLineH + RULE_GAP * 2, line, true);
  } else {
    drawHero();
    const int ruleY = contentTop + heroLineH + RULE_GAP / 2;
    renderer.drawLine(left, ruleY, right - 1, ruleY, true);
    renderer.drawLine(left, ruleY + 2, left + (right - left) / 3, ruleY + 2, true);
    const int bodyTop = contentTop + heroLineH + RULE_GAP + 6;

    if (state_ == State::Failed) {
      const int failW = renderer.getTextWidth(bodyFont, tr(STR_DICT_FAILED));
      renderer.drawText(bodyFont, left + (textWidth - failW) / 2, bodyTop, tr(STR_DICT_FAILED), true);
      if (failText_ != nullptr) {
        const int detailW = renderer.getTextWidth(bodyFont, failText_);
        renderer.drawText(bodyFont, left + (textWidth - detailW) / 2, bodyTop + bodyLineH + 6, failText_, true);
      }
    } else {
      if (!rowsBuilt_) {
        buildCard(bodyFont, textWidth);
        rowsBuilt_ = true;
      }
      const auto rowLines = [](const CardRow& row) {
        if (row.content.empty()) return 1;
        return row.inlineContent ? 1 : 1 + static_cast<int>(row.content.size());
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
        if (lineIdx + lines <= scrollLine_) {
          lineIdx += lines;
          continue;
        }
        if (y > contentBottom) break;
        const int labelW = row.label.empty() ? 0 : renderer.getTextWidth(bodyFont, row.label.c_str());
        if (!row.label.empty()) {
          renderer.drawText(bodyFont, left, y, row.label.c_str(), true);
        }
        if (row.inlineContent && !row.content.empty()) {
          renderer.drawText(bodyFont, left + labelW + LABEL_GAP, y, row.content[0].c_str(), true);
          y += bodyLineH;
        } else {
          if (!row.label.empty()) y += bodyLineH;
          for (const auto& contentLine : row.content) {
            if (y > contentBottom) break;
            renderer.drawText(bodyFont, left, y, contentLine.c_str(), true);
            y += bodyLineH;
          }
        }
        lineIdx += lines;
      }
    }
  }

  if (state_ == State::Card) {
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
  const char* confirmLabel = state_ == State::Failed ? tr(STR_RETRY) : "";
  const char* scrollUp = state_ == State::Card ? tr(STR_DIR_UP) : "";
  const char* scrollDown = state_ == State::Card ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, scrollUp, scrollDown);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
