#pragma once

#include <PigeonClient.h>
#include <PigeonService.h>

#include <cstddef>
#include <cstdint>

#include "activities/Activity.h"

// One peer's conversation. The relay history fills the screen on entry and the
// inbox is polled while the screen is frontmost. Messages are store-and-forward:
// a peer does not have to be online for the send to succeed, and the pending
// message is fetched the next time that peer opens the conversation.
//
//   Confirm -> VoiceActivity in question-input mode (speak)
//   Left    -> keyboard text entry
//   Back    -> device list
//   Up/Down -> scroll the message tail
class PigeonConversationActivity final : public Activity {
 public:
  PigeonConversationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* peerId,
                             const char* peerName, bool peerOnline = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { Loading, Ready, Failed };

  static constexpr size_t POLL_BATCH = 4;
  static constexpr uint32_t POLL_INTERVAL_MS = 1000;
  static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 30000;
  static constexpr uint32_t STATUS_TIMEOUT_MS = 3000;

  void loadHistory();
  void pollInbox();
  void sendText(const char* text);
  void startVoiceInput();
  void startKeyboardInput();
  void confirmClearMessages();
  void clearMessages();
  void addMessage(const Pigeon::Message& message);
  void ackUnread(const Pigeon::Message* messages, size_t count);
  bool updatePeerOnline();

  char peerId_[Pigeon::DEVICE_ID_CAP] = {};
  char peerName_[Pigeon::DEVICE_NAME_CAP] = {};
  State state_ = State::Loading;
  bool started_ = false;
  bool peerOnline_ = false;
  const char* failText_ = nullptr;
  const char* statusText_ = nullptr;
  uint32_t statusUntilMs_ = 0;
  Pigeon::Message messages_[Pigeon::MAX_MESSAGES] = {};
  size_t messageCount_ = 0;
  Pigeon::Message pollBuf_[POLL_BATCH] = {};
  uint32_t latestId_ = 0;
  uint32_t lastPollMs_ = 0;
  uint32_t lastHeartbeatMs_ = 0;
  int scroll_ = 0;
};
