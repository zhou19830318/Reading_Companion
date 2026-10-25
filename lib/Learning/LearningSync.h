#pragma once

#include <VoiceRelayCodec.h>  // LEARNING_CHUNK_WORDS

#include <cstddef>

// Pushes locally queued word-book entries to the relay (百词斩 M1-5). The card
// adds a word to SD instantly and then tries one pass here so the server copy
// catches up; a failed pass leaves the words pending and the next add retries
// them too.
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

  // Syncs every pending word in LEARNING_CHUNK_WORDS batches. `url`/`token`/
  // `deviceId` come from CloudConfig (relay URL is the ws:// form ChatClient
  // accepts). Stops at the first failed batch.
  static Outcome syncPending(const char* url, const char* token, const char* deviceId);
};

}  // namespace Learning
