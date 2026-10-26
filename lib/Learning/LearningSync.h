#pragma once

#include <VoiceRelayCodec.h>  // LEARNING_CHUNK_WORDS

#include <cstddef>

// Pushes locally queued word-book entries and review events to the relay
// (百词斩 M1-5 / M2). The card adds a word to SD instantly and then tries one
// pass here so the server copy catches up; a failed pass leaves the words
// pending and the next add retries them too. Review events use the same
// pending-until-acknowledged rule.
//
// Blocking: the caller paints a waiting frame first (the ChatClient rule).
namespace Learning {

class LearningSync {
 public:
  struct Outcome {
    size_t synced = 0;            // words the relay accepted (and marked)
    bool attempted = false;       // false when nothing was pending
    const char* error = nullptr;  // static reason on failure
  };

  // Syncs the word book in two phases, LEARNING_CHUNK_WORDS batches each:
  // pending adds first (POST /words, WordBook::markSynced), then removal
  // tombstones (POST /words/remove, WordBook::markRemoved). `url`/`token`/
  // `deviceId` come from CloudConfig (relay URL is the ws:// form ChatClient
  // accepts). Stops at the first failed batch; whatever was not acked stays
  // queued and the next call retries it. `synced` counts words the relay
  // acknowledged across both phases.
  static Outcome syncPending(const char* url, const char* token, const char* deviceId);

  // Submits every queued review event one POST at a time (the relay stores one
  // event per request and de-duplicates by clientEventId). Stops at the first
  // failure; `synced` counts events the relay acknowledged.
  static Outcome syncReviews(const char* url, const char* token, const char* deviceId);
};

}  // namespace Learning
