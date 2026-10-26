#pragma once

#include <VoiceRelayCodec.h>

#include <memory>
#include <string>
#include <vector>

#include "ReviewFormat.h"
#include "activities/Activity.h"

// 今日复习 (百词斩 §6, M2): the due-word queue from the relay, one card at a
// time, with the doc's layered key model.
//
//   Front  -- Confirm reveals the answer, side keys browse other cards.
//   Back   -- cached card text, side keys scroll, Confirm opens rating.
//   Rating -- front/side up-down only move the selection; Confirm submits.
//   Back   -- steps out one layer (rating -> back -> front -> exit), and never
//             submits a rating on its own (§14.3).
//
// Offline path: the queue and the card text are SD snapshots (ReviewStore), so
// a session started without Wi-Fi still reveals cached cards and queues the
// ratings locally. Ratings are idempotent events (clientEventId); the next
// online session pushes them before fetching a fresh queue.
class ReviewActivity final : public Activity {
 public:
  ReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Review", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override { return state_ == State::LoadingQueue || state_ == State::FetchingCard; }

 private:
  enum class State : uint8_t {
    LoadingQueue,  // fetching (or falling back to the cached queue)
    Empty,         // 今日已完成
    Front,
    Back,
    FetchingCard,  // one blocking relay round for a card that is not cached
    Rating,
    Done,  // session finished
    Failed,
  };

  enum class Status : uint8_t {
    None,
    Offline,     // cached queue, no relay
    Syncing,     // pushing queued ratings
    Synced,      // last rating batch acknowledged
    SyncFailed,  // ratings stay queued on SD
    SaveFailed,  // could not persist a rating
  };

  static constexpr size_t REPLY_CAP = 1536;  // CHAT_TEXT_CAP, same as WordCard
  static constexpr int MAX_WRAP_LINES = 16;
  static constexpr size_t QUEUE_CAP = VoiceRelay::LEARNING_REVIEW_LIMIT;

  State state_ = State::LoadingQueue;
  Status status_ = Status::None;
  bool started_ = false;
  bool relayConfigured_ = false;
  std::vector<Learning::ReviewFormat::QueueEntry> queue_;
  size_t index_ = 0;
  int32_t due_ = -1;  // last known total, mirrored to the home menu cache
  const char* failText_ = nullptr;

  std::unique_ptr<char[]> card_;
  bool cardAttempted_ = false;
  bool cardReady_ = false;
  int scrollLine_ = 0;
  int maxScroll_ = 0;
  std::vector<std::string> lines_;
  bool linesBuilt_ = false;
  int builtFontId_ = -1;
  int builtWidth_ = 0;
  int ratingChoice_ = 0;  // 0 忘记 / 1 模糊 / 2 认识

  const char* currentWord() const;
  void loadQueue();
  void loadCachedQueue();
  size_t dropRemovedWords();  // drops queue entries whose word left the word book
  void showFront();
  void reveal();
  void fetchCard();
  void openRating(int choice);
  void submitRating();
  void moveCard(int delta);
  void moveRating(int delta);
  void updateDueCache();
  void buildLines(int fontId, int textWidth);
};
