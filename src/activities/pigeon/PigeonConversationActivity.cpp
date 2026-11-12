#include "PigeonConversationActivity.h"

#include <ChatHistoryFormat.h>
#include <ChatTime.h>
#include <CrossPointSettings.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <WifiIdleManager.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/voice/VoiceActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr size_t MESSAGE_STAMP_CAP = 24;

// Relay timestamps are UTC ISO-8601 (e.g. "2026-11-02T03:39:07+00:00").
// Follow the local chat convention: today shows only the clock, yesterday is
// labelled, and older messages carry the full date.
bool messageStamp(const Pigeon::Message& message, const int64_t nowMs, char* out, const size_t cap) {
  int64_t seconds = 0;
  if (!ChatLog::ChatHistoryFormat::parseIsoUtcSeconds(message.createdAt, strlen(message.createdAt), seconds)) {
    return false;
  }
  return ChatLog::ChatHistoryFormat::formatChatMessageTime(out, cap, seconds * 1000, nowMs, SETTINGS.clockUtcOffsetQ,
                                                           tr(STR_YESTERDAY));
}

}  // namespace

PigeonConversationActivity::PigeonConversationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       const char* peerId, const char* peerName, bool peerOnline)
    : Activity("PigeonConversation", renderer, mappedInput), peerOnline_(peerOnline) {
  snprintf(peerId_, sizeof(peerId_), "%s", peerId != nullptr ? peerId : "");
  snprintf(peerName_, sizeof(peerName_), "%s", peerName != nullptr && peerName[0] != '\0' ? peerName : peerId_);
}

void PigeonConversationActivity::onEnter() {
  Activity::onEnter();
  WIFI_IDLE.arm();
  WIFI_IDLE.hold();
  state_ = State::Loading;
  started_ = false;
  failText_ = nullptr;
  statusText_ = nullptr;
  messageCount_ = 0;
  latestId_ = 0;
  lastPollMs_ = 0;
  lastHeartbeatMs_ = 0;
  statusUntilMs_ = 0;
  scroll_ = 0;

  if (!PIGEON_SERVICE.begin()) {
    state_ = State::Failed;
    failText_ = tr(STR_PIGEON_CONFIG);
    requestUpdate();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("PIGEON", "conversation offline, opening Wi-Fi selection");
    started_ = true;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                               state_ = State::Failed;
                               failText_ = tr(STR_PIGEON_FAILED);
                             }
                             started_ = false;
                             requestUpdate();
                           });
    requestUpdate();
    return;
  }
  requestUpdate();
}

void PigeonConversationActivity::onExit() {
  WIFI_IDLE.release();
  Activity::onExit();
}

void PigeonConversationActivity::loadHistory() {
  if (PIGEON_SERVICE.refresh() == PigeonClient::Result::Ok) updatePeerOnline();

  size_t count = 0;
  const PigeonClient::Result result = PIGEON_SERVICE.fetchHistory(peerId_, messages_, Pigeon::MAX_MESSAGES, count);
  if (result != PigeonClient::Result::Ok) {
    state_ = State::Failed;
    failText_ = tr(STR_PIGEON_FAILED);
    LOG_ERR("PIGEON", "history load failed: %s", PigeonClient::resultName(result));
    requestUpdate();
    return;
  }
  messageCount_ = count;
  latestId_ = 0;
  for (size_t i = 0; i < messageCount_; ++i) latestId_ = std::max(latestId_, messages_[i].id);
  state_ = State::Ready;
  lastPollMs_ = millis();
  lastHeartbeatMs_ = millis();
  ackUnread(messages_, messageCount_);
  requestUpdate();
}

void PigeonConversationActivity::loop() {
  if (state_ == State::Loading && !started_) {
    started_ = true;
    requestUpdateAndWait();
    loadHistory();
    return;
  }

  if (state_ == State::Failed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state_ = State::Loading;
      started_ = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  if (state_ != State::Ready) return;

  const uint32_t now = millis();
  if (statusText_ != nullptr && static_cast<int32_t>(now - statusUntilMs_) >= 0) {
    statusText_ = nullptr;
    requestUpdate();
  }
  if (now - lastHeartbeatMs_ >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs_ = now;
    if (PIGEON_SERVICE.refresh() == PigeonClient::Result::Ok && updatePeerOnline()) requestUpdate();
  }
  if (now - lastPollMs_ >= POLL_INTERVAL_MS) {
    lastPollMs_ = now;
    pollInbox();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) && messageCount_ > 0 &&
      scroll_ < static_cast<int>(messageCount_) - 1) {
    ++scroll_;
    requestUpdate();
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) && scroll_ > 0) {
    --scroll_;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startVoiceInput();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    startKeyboardInput();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    confirmClearMessages();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

bool PigeonConversationActivity::updatePeerOnline() {
  bool online = false;
  for (size_t i = 0; i < PIGEON_SERVICE.deviceCount(); ++i) {
    const Pigeon::Device& device = PIGEON_SERVICE.devices()[i];
    if (strcmp(device.id, peerId_) == 0) {
      online = device.online;
      break;
    }
  }
  const bool changed = online != peerOnline_;
  peerOnline_ = online;
  return changed;
}

void PigeonConversationActivity::addMessage(const Pigeon::Message& message) {
  for (size_t i = 0; i < messageCount_; ++i) {
    if (messages_[i].id == message.id && message.id != 0) return;
  }
  if (messageCount_ >= Pigeon::MAX_MESSAGES) {
    for (size_t i = 1; i < messageCount_; ++i) messages_[i - 1] = messages_[i];
    --messageCount_;
  }
  messages_[messageCount_++] = message;
  scroll_ = 0;
}

void PigeonConversationActivity::pollInbox() {
  size_t count = 0;
  uint32_t latest = latestId_;
  const PigeonClient::Result result = PIGEON_SERVICE.fetchInbox(latestId_, pollBuf_, POLL_BATCH, count, latest);
  if (result != PigeonClient::Result::Ok) {
    // Polling is best-effort; the next tick retries. Do not replace a readable
    // conversation with an error for a transient radio hiccup.
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    addMessage(pollBuf_[i]);
    latestId_ = std::max(latestId_, pollBuf_[i].id);
  }
  latestId_ = std::max(latestId_, latest);
  if (count > 0) {
    ackUnread(pollBuf_, count);
    requestUpdate();
  }
}

void PigeonConversationActivity::ackUnread(const Pigeon::Message* messages, size_t count) {
  uint32_t ids[Pigeon::MAX_MESSAGES] = {};
  size_t idCount = 0;
  for (size_t i = 0; i < count && idCount < Pigeon::MAX_MESSAGES; ++i) {
    if (!messages[i].outgoing && !messages[i].read && messages[i].id != 0) ids[idCount++] = messages[i].id;
  }
  if (idCount > 0) PIGEON_SERVICE.ack(ids, idCount);
}

void PigeonConversationActivity::sendText(const char* text) {
  if (text == nullptr || text[0] == '\0') return;
  char clipped[Pigeon::TEXT_CAP + 1];
  size_t len = strlen(text);
  if (len > Pigeon::TEXT_CAP) {
    len = Pigeon::TEXT_CAP;
    while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80) --len;
  }
  snprintf(clipped, sizeof(clipped), "%.*s", static_cast<int>(len), text);

  statusText_ = tr(STR_PIGEON_SENDING);
  statusUntilMs_ = millis() + STATUS_TIMEOUT_MS;
  requestUpdateAndWait();
  Pigeon::Message sent;
  const PigeonClient::Result result = PIGEON_SERVICE.sendText(peerId_, clipped, &sent);
  if (result == PigeonClient::Result::Ok) {
    addMessage(sent);
    latestId_ = std::max(latestId_, sent.id);
    statusText_ = peerOnline_ ? tr(STR_PIGEON_SENT) : tr(STR_PIGEON_SENT_OFFLINE);
  } else {
    statusText_ = tr(STR_PIGEON_SEND_FAILED);
    LOG_ERR("PIGEON", "send failed: %s", PigeonClient::resultName(result));
  }
  statusUntilMs_ = millis() + STATUS_TIMEOUT_MS;
  requestUpdate();
}

void PigeonConversationActivity::startVoiceInput() {
  auto voice = std::make_unique<VoiceActivity>(renderer, mappedInput);
  voice->setQuestionInput(StrId::STR_PIGEON, StrId::STR_PIGEON_TYPE);
  // Pigeon: the recognized utterance is the message. Return the transcript
  // directly and send it; editing stays available through the Left "Type" key.
  voice->setAutoSendTranscript(true);
  startActivityForResult(std::move(voice), [this](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) {
      requestUpdate();
      return;
    }
    const std::string& text = std::get<KeyboardResult>(result.data).text;
    if (text.empty()) {
      requestUpdate();
      return;
    }
    sendText(text.c_str());
  });
}

void PigeonConversationActivity::confirmClearMessages() {
  if (messageCount_ == 0) return;
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_PIGEON_CLEAR), tr(STR_PIGEON_CLEAR_CONFIRM)),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        clearMessages();
      });
}

void PigeonConversationActivity::clearMessages() {
  statusText_ = tr(STR_PIGEON_CLEARING);
  statusUntilMs_ = millis() + STATUS_TIMEOUT_MS;
  requestUpdateAndWait();

  const PigeonClient::Result result = PIGEON_SERVICE.clearHistory(peerId_);
  if (result == PigeonClient::Result::Ok) {
    messageCount_ = 0;
    latestId_ = 0;
    scroll_ = 0;
    statusText_ = tr(STR_PIGEON_CLEARED);
  } else {
    statusText_ = tr(STR_PIGEON_CLEAR_FAILED);
    LOG_ERR("PIGEON", "clear history failed: %s", PigeonClient::resultName(result));
  }
  statusUntilMs_ = millis() + STATUS_TIMEOUT_MS;
  requestUpdate();
}

void PigeonConversationActivity::startKeyboardInput() {
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(StrId::STR_PIGEON_TYPE), "",
                                                          Pigeon::TEXT_CAP, InputType::Text);
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) {
      requestUpdate();
      return;
    }
    const std::string& text = std::get<KeyboardResult>(result.data).text;
    if (text.empty()) {
      requestUpdate();
      return;
    }
    sendText(text.c_str());
  });
}

void PigeonConversationActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  char title[Pigeon::DEVICE_NAME_CAP + 24];
  snprintf(title, sizeof(title), "%s · %s", peerName_,
           I18N.get(peerOnline_ ? StrId::STR_PIGEON_ONLINE : StrId::STR_PIGEON_OFFLINE));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;
  const int pagePadding = std::max(8, metrics.contentSidePadding);

  if (state_ == State::Loading) {
    GUI.drawPopup(renderer, tr(STR_PIGEON_LOADING));
  } else if (state_ == State::Failed) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2,
                              failText_ != nullptr ? failText_ : tr(STR_PIGEON_FAILED));
  } else {
    // WeChat-style bottom input bar. Incoming bubbles are outlined white,
    // outgoing bubbles are solid black with reversed text.
    const int inputBarHeight = 34;
    const int inputGap = 6;
    const int inputBarTop = contentBottom - inputBarHeight;
    const int messagesTop = contentTop + 4;
    const int messagesBottom = inputBarTop - inputGap;

    if (messageCount_ == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, (messagesTop + messagesBottom) / 2, tr(STR_PIGEON_NO_MESSAGES));
    } else {
      struct Bubble {
        int x;
        int y;
        int width;
        int height;
        bool outgoing;
        bool hasTime;
        char time[MESSAGE_STAMP_CAP];
        std::vector<std::string> lines;
      };

      const int bubblePadX = 10;
      const int bubblePadY = 6;
      const int bubbleGap = 8;
      const int bubbleRadius = 8;
      const int timeGap = 2;
      const int maxBubbleWidth = std::max(120, pageWidth * 3 / 4);
      const int textMaxWidth = maxBubbleWidth - 2 * bubblePadX;
      const int lineHeight = renderer.getTextHeight(UI_10_FONT_ID) + 2;
      const int timeLineHeight = renderer.getTextHeight(UI_10_FONT_ID);
      const int availableHeight = messagesBottom - messagesTop - 2 * bubblePadY;
      const int maxLines = std::max(1, std::min(8, availableHeight / lineHeight));
      const int64_t nowMs = ChatLog::nowEpochMs();

      std::vector<Bubble> bubbles;
      bubbles.reserve(8);
      int end = static_cast<int>(messageCount_) - scroll_;
      if (end < 0) end = 0;
      int y = messagesBottom;
      for (int i = end - 1; i >= 0; --i) {
        std::vector<std::string> lines = renderer.wrappedText(UI_10_FONT_ID, messages_[i].text, textMaxWidth, maxLines);
        if (lines.empty()) lines.emplace_back("");
        int textWidth = 0;
        for (const auto& line : lines)
          textWidth = std::max(textWidth, renderer.getTextWidth(UI_10_FONT_ID, line.c_str()));
        char time[MESSAGE_STAMP_CAP] = {};
        const bool hasTime = messageStamp(messages_[i], nowMs, time, sizeof(time));
        const int timeWidth = hasTime ? renderer.getTextWidth(UI_10_FONT_ID, time) : 0;
        const int bubbleWidth = std::min(maxBubbleWidth, std::max(40, std::max(textWidth, timeWidth) + 2 * bubblePadX));
        const int bubbleHeight =
            static_cast<int>(lines.size()) * lineHeight + (hasTime ? timeLineHeight + timeGap : 0) + 2 * bubblePadY;
        if (y - bubbleHeight < messagesTop) break;
        y -= bubbleHeight;
        const int x = messages_[i].outgoing ? pageWidth - pagePadding - bubbleWidth : pagePadding;
        Bubble bubble{x, y, bubbleWidth, bubbleHeight, messages_[i].outgoing, hasTime, {}, std::move(lines)};
        if (hasTime) memcpy(bubble.time, time, sizeof(bubble.time));
        bubbles.push_back(std::move(bubble));
        y -= bubbleGap;
      }

      for (auto it = bubbles.rbegin(); it != bubbles.rend(); ++it) {
        if (it->outgoing) {
          renderer.fillRoundedRect(it->x, it->y, it->width, it->height, bubbleRadius, Color::Black);
        } else {
          renderer.fillRoundedRect(it->x, it->y, it->width, it->height, bubbleRadius, Color::White);
          renderer.drawRoundedRect(it->x, it->y, it->width, it->height, 1, bubbleRadius, true);
        }
        int textY = it->y + bubblePadY;
        for (const auto& line : it->lines) {
          renderer.drawText(UI_10_FONT_ID, it->x + bubblePadX, textY, line.c_str(), !it->outgoing);
          textY += lineHeight;
        }
        if (it->hasTime) {
          textY += timeGap;
          const int timeX = it->x + it->width - bubblePadX - renderer.getTextWidth(UI_10_FONT_ID, it->time);
          renderer.drawText(UI_10_FONT_ID, timeX, textY, it->time, !it->outgoing);
        }
      }
    }

    const int inputWidth = pageWidth - 2 * pagePadding;
    renderer.fillRoundedRect(pagePadding, inputBarTop, inputWidth, inputBarHeight, 8, Color::White);
    renderer.drawRoundedRect(pagePadding, inputBarTop, inputWidth, inputBarHeight, 1, 8, true);
    const char* barText = statusText_;
    if (barText == nullptr) barText = peerOnline_ ? tr(STR_PIGEON_HINT) : tr(STR_PIGEON_OFFLINE_SEND);
    renderer.drawCenteredText(UI_10_FONT_ID, inputBarTop + (inputBarHeight - renderer.getTextHeight(UI_10_FONT_ID)) / 2,
                              barText);
  }

  if (state_ == State::Failed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == State::Ready) {
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), tr(STR_PIGEON_TALK), tr(STR_PIGEON_TYPE), tr(STR_PIGEON_CLEAR));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
