#pragma once

// Personal word-book line format (百词斩 M1-5): one append-only line per word
// under /.crosspoint/learning/words.tsv.
//
//   WBT1 <addedAt> <p|s|r> <word>\n
//
// "WBT1 " is the format tag (a stray text file in the directory cannot match
// it, the NTF1 rule). Fields are decimal / single characters / the token,
// space separated. The state char is the sync contract with the relay:
//   p = the add still waits for /api/v1/learning/words to accept it,
//   s = that endpoint has acknowledged the add,
//   r = removed locally, waiting for /api/v1/learning/words/remove (a
//       tombstone — the line survives until the relay acks the removal).
// Words are the picker's lowercase ASCII tokens; only [a-z]/[A-Z], apostrophe
// and hyphen are accepted so a line can never contain a space or control
// byte.
//
// Header-only, pure C++ — no ESP-IDF, no Arduino — so test/learning_wordbook
// compiles the same helpers the firmware uses.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Learning {
namespace WordBookFormat {

// Root of the word-book store, under the shared SD cache root (.crosspoint/).
inline constexpr const char* DIR = "/.crosspoint/learning";
inline constexpr const char* FILE_PATH = "/.crosspoint/learning/words.tsv";

// "WBT1 " — see file comment.
inline constexpr const char* LINE_PREFIX = "WBT1 ";

// Token capacity (NUL included): the longest single English dictionary word is
// well under this; 48 keeps a whole line inside a 128-byte scratch buffer.
inline constexpr size_t WORD_CAP = 48;

enum class State : uint8_t { Pending, Synced, Removing };

struct Entry {
  uint32_t addedAt = 0;  // epoch seconds; 0 when the clock was not set
  State state = State::Pending;
  char word[WORD_CAP] = {};
};

// State char on the wire; lives next to the enum so build/parse cannot drift.
inline char stateChar(State state) {
  switch (state) {
    case State::Synced:
      return 's';
    case State::Removing:
      return 'r';
    case State::Pending:
      break;
  }
  return 'p';
}

// True when `word` is a non-empty token the picker can produce.
inline bool validWord(const char* word) {
  if (word == nullptr || word[0] == '\0') return false;
  const size_t len = strnlen(word, WORD_CAP);
  if (len == 0 || len >= WORD_CAP) return false;
  for (size_t i = 0; i < len; ++i) {
    const char c = word[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// Serialises one line. Returns its byte length (including '\n'), or 0 when the
// entry is malformed or would not fit `cap`.
inline size_t buildLine(char* out, size_t cap, const Entry& entry) {
  if (out == nullptr || cap == 0 || !validWord(entry.word)) return 0;
  const int n = snprintf(out, cap, "%s%lu %c %s\n", LINE_PREFIX, static_cast<unsigned long>(entry.addedAt),
                         stateChar(entry.state), entry.word);
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

// Parses one complete line (must end in '\n'). Rejects malformed records so a
// torn append only loses its own line.
inline bool parseLine(const char* line, size_t len, Entry& out) {
  if (line == nullptr || len <= strlen(LINE_PREFIX) + 4 || line[len - 1] != '\n') return false;
  const size_t prefixLen = strlen(LINE_PREFIX);
  if (memcmp(line, LINE_PREFIX, prefixLen) != 0) return false;

  size_t p = prefixLen;
  size_t digits = 0;
  uint32_t addedAt = 0;
  while (p < len && line[p] >= '0' && line[p] <= '9') {
    const uint32_t digit = static_cast<uint32_t>(line[p] - '0');
    if (addedAt > (UINT32_MAX - digit) / 10) return false;  // overflow
    addedAt = addedAt * 10 + digit;
    ++p;
    ++digits;
  }
  if (digits == 0 || p + 2 >= len || line[p] != ' ') return false;
  ++p;

  State state;
  switch (line[p]) {
    case 'p':
      state = State::Pending;
      break;
    case 's':
      state = State::Synced;
      break;
    case 'r':
      state = State::Removing;
      break;
    default:
      return false;
  }
  ++p;
  if (p >= len || line[p] != ' ') return false;
  ++p;

  const size_t wordLen = len - 1 - p;  // exclude '\n'
  if (wordLen == 0 || wordLen >= WORD_CAP) return false;
  char word[WORD_CAP];
  memcpy(word, line + p, wordLen);
  word[wordLen] = '\0';
  if (!validWord(word)) return false;

  out.addedAt = addedAt;
  out.state = state;
  memcpy(out.word, word, wordLen + 1);
  return true;
}

}  // namespace WordBookFormat
}  // namespace Learning
