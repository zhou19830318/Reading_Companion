#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

// English word card (百词斩 §5.1/§5.2), opened from the reader's dictionary
// picker when the cursor sits on an ASCII word (§4.3: the word card opens
// directly, no candidate list on the happy path).
//
// Data comes from the relay's /api/v1/chat with mode="word" — the offline
// dictionary payload only covers Hanzi, so a missing relay is a hard failure,
// not a fallback. The request is one blocking round trip from loop() after the
// waiting frame is painted (the DictionaryActivity/Ask-AI pattern).
//
// M1 scope: word, phonetic, part of speech, meaning, and a Confirm-expanded
// example block, plus 加入/移出生词 on SD with one opportunistic relay sync.
// The voice layer arrives in M3; it must not regress the layered interaction:
// front Confirm adds/removes while the detail block is closed, second Confirm
// expands 例句/译文, Back steps out one layer at a time.
class WordCardActivity final : public Activity {
 public:
  WordCardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* word)
      : Activity("WordCard", renderer, mappedInput), word_(word != nullptr ? word : "") {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override { return state_ == State::Loading; }

 private:
  enum class State : uint8_t { Loading, Card, Failed };

  // One-line feedback under the card (加入/移出 + sync state).
  enum class Status : uint8_t {
    None,
    AddedPending,  // saved on SD, sync not attempted yet
    Syncing,
    AddedSynced,
    AddedSyncFailed,
    Removed,
    SaveFailed,
  };

  struct Row {
    std::string label;                 // includes the 【】 markers, drawn as-is
    std::vector<std::string> content;  // wrapped at build time
    bool detail = false;               // 例句/译文: hidden until the first Confirm
  };

  static constexpr size_t REPLY_CAP = 1536;  // CHAT_TEXT_CAP; a card is ~300 bytes
  static constexpr int MAX_WRAP_LINES = 12;

  State state_ = State::Loading;
  bool started_ = false;
  std::string word_;
  std::string hero_;
  std::unique_ptr<char[]> reply_;
  std::vector<Row> rows_;
  bool rowsBuilt_ = false;
  int builtFontId_ = -1;
  int builtWidth_ = 0;
  int maxScroll_ = 0;
  bool detailOpen_ = false;
  int scrollLine_ = 0;
  const char* failText_ = nullptr;
  bool relayConfigured_ = false;
  bool inWordbook_ = false;
  bool wordSynced_ = false;
  Status status_ = Status::None;

  void beginLookup();
  void buildCard(int fontId, int textWidth);
  void toggleDetail();
  void toggleAdd();
};
