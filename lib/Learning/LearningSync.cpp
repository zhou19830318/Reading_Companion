#include "LearningSync.h"

#include <Logging.h>
#include <Memory.h>

#include "LearningClient.h"
#include "WordBook.h"

namespace Learning {

namespace {
// A hostile/looping store must not pin the device here; 16-word batches mean
// 32 rounds cover 512 words, far past any realistic queue.
constexpr int MAX_BATCHES = 32;

// 16 entries x ~56 B would blow the <256-byte stack rule; the batch buffer is
// heap-allocated once per sync (~900 B internal DRAM).
constexpr size_t BATCH_CAP = VoiceRelay::LEARNING_CHUNK_WORDS;
}  // namespace

LearningSync::Outcome LearningSync::syncPending(const char* url, const char* token, const char* deviceId) {
  Outcome outcome;
  auto batch = makeUniqueNoThrow<WordBookFormat::Entry[]>(BATCH_CAP);
  if (!batch) {
    LOG_ERR("LSYNC", "OOM: %u entries", static_cast<unsigned>(BATCH_CAP));
    outcome.error = "oom";
    return outcome;
  }

  for (int round = 0; round < MAX_BATCHES; ++round) {
    const size_t count = WordBook::listPending(batch.get(), BATCH_CAP);
    if (count == 0) return outcome;  // done (also the clean first pass)
    outcome.attempted = true;

    const char* words[BATCH_CAP];
    for (size_t i = 0; i < count; ++i) words[i] = batch[i].word;

    int32_t added = 0;
    int32_t accepted = 0;
    const auto result =
        VoiceRelay::LearningClient::addWords(url, token, deviceId, words, count, "lookup", &added, &accepted);
    if (result != VoiceRelay::LearningClient::Result::Ok) {
      LOG_ERR("LSYNC", "batch of %u failed: %s", static_cast<unsigned>(count),
              VoiceRelay::LearningClient::resultName(result));
      outcome.error = VoiceRelay::LearningClient::resultName(result);
      return outcome;
    }
    LOG_INF("LSYNC", "batch of %u: accepted %d, added %d", static_cast<unsigned>(count), static_cast<int>(accepted),
            static_cast<int>(added));
    if (!WordBook::markSynced(words, count)) {
      outcome.error = "wordbook";
      return outcome;
    }
    outcome.synced += count;
  }
  LOG_ERR("LSYNC", "stopped after %d batches", MAX_BATCHES);
  outcome.error = "loop";
  return outcome;
}

}  // namespace Learning
