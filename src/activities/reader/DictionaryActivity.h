#pragma once

#include <OpenClawSession.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

// Character lookup card: opened from the reader's dictionary picker with the
// single chosen codepoint. Preferred backend is the relay's /api/v1/chat with
// mode="dict" (the relay applies its dictionary system prompt server-side:
// pinyin, radical and stroke count, meanings, word formations, an example —
// 【】-delimited lines). When the relay is not configured or its round fails,
// the same request falls back to the legacy MiMo + OpenClaw gateway, which
// gets the format instruction inline (the relay's dict prompt, mirrored in
// buildDictPrompt()). Either way the reply is laid out as a Xinhua-style card
// with the character set as a hero glyph at the top. Back returns to the
// picker with the cursor intact; Confirm retries after a failure.
class DictionaryActivity final : public Activity {
 public:
  DictionaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint32_t codepoint)
      : Activity("Dictionary", renderer, mappedInput), codepoint_(codepoint) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override {
    return state_ == State::Loading || state_ == State::Connecting || state_ == State::Asking;
  }

 private:
  enum class State : uint8_t {
    Loading,     // relay round in flight (painted before the blocking call)
    Connecting,  // OpenClaw fallback: gateway link coming up, sends once Connected
    Asking,      // chat.send outstanding, reply streaming
    Card,        // settled entry, scrollable
    Failed,      // both backends died; Confirm retries from the relay
  };

  // One laid-out card row: a label (【拼音】 …) and its content. Short
  // content shares the label row; longer content wraps onto rows underneath so
  // meanings and examples stay readable instead of overflowing.
  struct CardRow {
    std::string label;
    std::vector<std::string> content;
    bool inlineContent = false;  // content[0] belongs on the label row
  };

  State state_ = State::Loading;
  bool started_ = false;  // one relay round per Loading entry
  uint32_t codepoint_ = 0;
  char charBuf_[8] = {};            // the selected character, UTF-8
  const char* failText_ = nullptr;  // tr() table pointer; valid for our lifetime
  std::unique_ptr<char[]> reply_;
  static constexpr size_t REPLY_CAP = 2048;
  std::string heroChar_;  // 【字】's content when present, else charBuf_
  std::vector<CardRow> rows_;
  bool rowsBuilt_ = false;  // rows_ matches the current reply + text width
  int scrollLine_ = 0;
  int maxScroll_ = 0;  // recomputed each render from the wrapped rows

  // OpenClaw fallback (Connecting/Asking). The prompt is allocated on first
  // use — with the link already up, so it never sits on the internal heap
  // across the TLS dial — and the session is the shared, process-wide gateway
  // link: borrowed, never closed here (the workbench status cards read it).
  std::unique_ptr<char[]> prompt_;
  static constexpr size_t PROMPT_CAP = 1024;
  OpenClaw::Session& session_ = OpenClaw::Session::instance();
  // Safety repaint while State::Connecting. phaseText() is static between the
  // session notifier's own repaints, so this only guards a notification that
  // never arrives; there is no counter on that screen to keep honest, hence
  // the coarse step.
  static constexpr uint32_t CONNECT_TICK_MS = 15000;
  uint32_t lastWaitTickMs_ = 0;
  // Elapsed-seconds value the Asking read-out last showed; compared against
  // awaitingMs()/1000 so the wait line repaints exactly once per real second.
  uint32_t lastWaitShownSec_ = 0;

  void beginLookup();
  void beginOpenClawLookup();
  bool buildDictPrompt();
  void sendDictPrompt();
  const char* phaseText() const;
  void promptForWifi();
  void buildCard(int fontId, int textWidth);
  static void onSessionNotifyTrampoline(void* ctx);
  void onSessionNotify();
};
