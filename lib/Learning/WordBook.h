#pragma once

#include <cstddef>
#include <cstdint>

#include "WordBookFormat.h"

// SD-backed personal word book (百词斩 M1-5): one append-only line per word
// under /.crosspoint/learning/words.tsv. Format: WordBookFormat.h
// (host-tested). Device-only: HalStorage.
//
// Memory discipline (the NoteStore rule): the file is never loaded whole.
// Membership checks and the pending queues stream it through a fixed 128-byte
// line buffer, so RAM cost is constant regardless of word count. add() is one
// append per word; remove()/markSynced()/markRemoved() rewrite through a temp
// file then remove + rename (the ProgressFile crash-safety rule).
//
// Sync contract (the relay is the other end of both queues):
//   add()    queues state Pending  -> syncPending pushes adds    (markSynced drops them)
//   remove() queues state Removing -> syncPending pushes removals (markRemoved drops them)
// remove() always leaves a tombstone, even for a never-pushed add: the relay's
// remove is idempotent (absent word = removed:0) and pushing it heals the one
// drift case where an add POST landed but markSynced's SD rewrite failed.
namespace Learning {

class WordBook {
 public:
  // Membership (case-exact: the picker stores lowercase tokens). A queued
  // removal (state Removing) counts as absent — the word left the book the
  // moment the user removed it. `synced` reports whether the relay has
  // acknowledged the word.
  static bool contains(const char* word, bool* synced = nullptr);

  // Appends the word when absent (duplicates are a no-op, not an error).
  // A tombstoned word is re-queued as Pending instead — the relay's add
  // reactivates its removed row, so re-adding after a remove self-heals
  // either order of in-flight ops. `added` reports whether a line was
  // appended or re-queued. False only on SD failure.
  static bool add(const char* word, uint32_t addedAt, bool& added);

  // Queues the word's removal (state Removing, the line survives until the
  // relay acks). True when the word is absent afterwards (already-removed and
  // never-present words are absent, hence true). False only on SD failure.
  static bool remove(const char* word);

  // Copies the pending (not yet relay-accepted) words, file order, into
  // `out`. Returns the count written (<= cap).
  static size_t listPending(WordBookFormat::Entry* out, size_t cap);

  // Same, for queued removals waiting on /api/v1/learning/words/remove.
  static size_t listRemoving(WordBookFormat::Entry* out, size_t cap);

  // Flips the given Pending words to Synced in one rewrite. Matches that are
  // no longer Pending (a removal queued in between) are left alone. False on
  // SD failure.
  static bool markSynced(const char* const* words, size_t count);

  // Drops the given Removing lines the relay has acknowledged, in one
  // rewrite. False on SD failure.
  static bool markRemoved(const char* const* words, size_t count);

 private:
  // Streams the file to the first line whose word matches; fills `out`.
  // Returns false when the word is absent (any state). Device-only helper.
  static bool find(const char* word, WordBookFormat::Entry& out);
};

}  // namespace Learning
