#pragma once

#include <cstddef>
#include <cstdint>

#include "ReviewFormat.h"

// SD-backed review queue + card cache (百词斩 M2). Device-only: HalStorage.
//
// The queue is append-only until an event is acknowledged, then rewritten
// through the shared temp-file swap (ReviewFormat.h holds the line format).
// Cards are one file per word under /.crosspoint/learning/cards, which is what
// keeps 今日复习 usable offline: a word whose card was fetched once can be
// revealed without the relay.
namespace Learning {

class ReviewStore {
 public:
  // Appends one pending event. A duplicate clientEventId is a no-op. False
  // only on SD failure or a malformed event.
  static bool enqueue(const char* word, uint8_t rating, const char* clientEventId);

  // Copies pending events, oldest first, into `out`. Returns the count
  // written (<= cap).
  static size_t listPending(ReviewFormat::Event* out, size_t cap);

  static size_t pendingCount();

  // Drops the event whose id matches (already acknowledged by the relay).
  // True when the event is absent afterwards.
  static bool remove(const char* clientEventId);

  // Drops every pending event for `word` (used when the word itself leaves the
  // word book; otherwise a later sync would re-create it server-side).
  static bool removeWord(const char* word);

  // Card cache. saveCard overwrites; loadCard returns the byte count copied
  // (NUL-terminated) or 0 when the cache has no card for the word.
  static bool saveCard(const char* word, const char* text);
  static size_t loadCard(const char* word, char* out, size_t cap);

  // Cached due count for the home menu. saveDue rewrites the single state
  // line; loadDue returns false when nothing was cached yet.
  static bool saveDue(int32_t due, uint32_t checkedAt);
  static bool loadDue(int32_t& due, uint32_t& checkedAt);

  // Snapshot of the last fetched review queue. Offline sessions read this;
  // saveQueue replaces the whole snapshot (one file write per entry).
  static bool saveQueue(const ReviewFormat::QueueEntry* entries, size_t count);
  static size_t loadQueue(ReviewFormat::QueueEntry* out, size_t cap);
};

}  // namespace Learning
