#pragma once

#include <cstddef>
#include <cstdint>

#include "WordBookFormat.h"

// SD-backed personal word book (百词斩 M1-5): one append-only line per lookup
// add under /.crosspoint/learning/words.tsv. Format: WordBookFormat.h
// (host-tested). Device-only: HalStorage.
//
// Memory discipline (the NoteStore rule): the file is never loaded whole.
// Membership checks and the pending queue stream it through a fixed 128-byte
// line buffer, so RAM cost is constant regardless of word count. add() is one
// append per word; remove()/markSynced() rewrite through a temp file then
// remove + rename (the ProgressFile crash-safety rule).
namespace Learning {

class WordBook {
 public:
  // Membership (case-exact: the picker stores lowercase tokens). `synced`
  // reports whether the relay has acknowledged the word.
  static bool contains(const char* word, bool* synced = nullptr);

  // Appends the word when absent (duplicates are a no-op, not an error).
  // `added` reports whether a line was appended. False only on SD failure.
  static bool add(const char* word, uint32_t addedAt, bool& added);

  // Removes the word's line. True when the word is absent afterwards.
  static bool remove(const char* word);

  // Copies the pending (not yet relay-accepted) words, oldest first, into
  // `out`. Returns the count written (<= cap).
  static size_t listPending(WordBookFormat::Entry* out, size_t cap);

  // Flips the given words to synced in one rewrite. False on SD failure.
  static bool markSynced(const char* const* words, size_t count);
};

}  // namespace Learning
