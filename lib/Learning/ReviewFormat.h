#pragma once

// On-device review store formats (百词斩 M2): the pending review-event queue,
// the cached "today's review" count, and the per-word card cache path.
//
//   events.tsv: RVW1 <rating> <clientEventId> <word>\n
//   state.tsv:  RVS1 <due> <checkedAt>\n
//   cards/<word>.txt: the raw card reply from /api/v1/chat mode=word
//
// "RVW1 "/"RVS1 " are format tags (the NTF1 rule: a stray text file in the
// directory cannot match them). Every field is space-free: rating is 1..4,
// clientEventId is the 32 lowercase hex digits buildReviewRequest() expects,
// and the word passes WordBookFormat::validWord(). Header-only and pure C++,
// so test/learning_review compiles the same helpers the firmware uses.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "WordBookFormat.h"

namespace Learning {
namespace ReviewFormat {

inline constexpr const char* DIR = WordBookFormat::DIR;
inline constexpr const char* EVENTS_PATH = "/.crosspoint/learning/reviews.tsv";
inline constexpr const char* EVENTS_TMP = "/.crosspoint/learning/reviews.tsv.tmp";
inline constexpr const char* STATE_PATH = "/.crosspoint/learning/review_state.txt";
inline constexpr const char* CARD_DIR = "/.crosspoint/learning/cards";

inline constexpr const char* EVENT_PREFIX = "RVW1 ";
inline constexpr const char* STATE_PREFIX = "RVS1 ";
inline constexpr const char* QUEUE_PREFIX = "RVQ1 ";

// Last fetched "today's review" page; this is what an offline session reads.
inline constexpr const char* QUEUE_PATH = "/.crosspoint/learning/review_queue.tsv";

// 32 lowercase hex digits + NUL, i.e. the relay's clientEventId shape.
inline constexpr size_t EVENT_ID_CAP = 33;

struct Event {
  uint8_t rating = 0;  // 1 忘记 / 2 模糊 / 3 认识 / 4 (server-side only)
  char clientEventId[EVENT_ID_CAP] = {};
  char word[WordBookFormat::WORD_CAP] = {};
};

inline bool validEventId(const char* id) {
  if (id == nullptr) return false;
  const size_t len = strnlen(id, EVENT_ID_CAP);
  if (len == 0 || len >= EVENT_ID_CAP) return false;
  for (size_t i = 0; i < len; ++i) {
    const char c = id[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// Returns the line length including '\n', or 0 when the event is malformed or
// would not fit `cap`.
inline size_t buildEventLine(char* out, size_t cap, const Event& event) {
  if (out == nullptr || cap == 0 || event.rating < 1 || event.rating > 4) return 0;
  if (!validEventId(event.clientEventId) || !WordBookFormat::validWord(event.word)) return 0;
  const int n = snprintf(out, cap, "%s%u %s %s\n", EVENT_PREFIX, static_cast<unsigned>(event.rating),
                         event.clientEventId, event.word);
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

// Parses one complete line (must end in '\n'). A torn append only loses its
// own line.
inline bool parseEventLine(const char* line, size_t len, Event& out) {
  const size_t prefixLen = strlen(EVENT_PREFIX);
  if (line == nullptr || len <= prefixLen + 4 || line[len - 1] != '\n') return false;
  if (memcmp(line, EVENT_PREFIX, prefixLen) != 0) return false;

  size_t p = prefixLen;
  if (p + 1 >= len || line[p] < '1' || line[p] > '4') return false;
  const uint8_t rating = static_cast<uint8_t>(line[p] - '0');
  ++p;
  if (p >= len || line[p] != ' ') return false;
  ++p;

  const size_t idStart = p;
  while (p < len && line[p] != ' ') ++p;
  if (p >= len || p == idStart) return false;
  const size_t idLen = p - idStart;
  if (idLen >= EVENT_ID_CAP) return false;
  char id[EVENT_ID_CAP];
  memcpy(id, line + idStart, idLen);
  id[idLen] = '\0';
  if (!validEventId(id)) return false;
  ++p;  // skip the space

  const size_t wordLen = len - 1 - p;  // exclude '\n'
  if (wordLen == 0 || wordLen >= WordBookFormat::WORD_CAP) return false;
  char word[WordBookFormat::WORD_CAP];
  memcpy(word, line + p, wordLen);
  word[wordLen] = '\0';
  if (!WordBookFormat::validWord(word)) return false;

  out.rating = rating;
  memcpy(out.clientEventId, id, idLen + 1);
  memcpy(out.word, word, wordLen + 1);
  return true;
}

// The cached queue size the home menu shows before the next sync; checkedAt is
// epoch seconds (0 when the clock was not set).
inline size_t buildStateLine(char* out, size_t cap, int32_t due, uint32_t checkedAt) {
  if (out == nullptr || cap == 0 || due < 0) return 0;
  const int n =
      snprintf(out, cap, "%s%ld %lu\n", STATE_PREFIX, static_cast<long>(due), static_cast<unsigned long>(checkedAt));
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

inline bool parseStateLine(const char* line, size_t len, int32_t& due, uint32_t& checkedAt) {
  const size_t prefixLen = strlen(STATE_PREFIX);
  if (line == nullptr || len <= prefixLen + 3 || line[len - 1] != '\n') return false;
  if (memcmp(line, STATE_PREFIX, prefixLen) != 0) return false;

  size_t p = prefixLen;
  size_t digits = 0;
  uint32_t value = 0;
  while (p < len && line[p] >= '0' && line[p] <= '9') {
    const uint32_t digit = static_cast<uint32_t>(line[p] - '0');
    if (value > (UINT32_MAX - digit) / 10) return false;
    value = value * 10 + digit;
    ++p;
    ++digits;
  }
  if (digits == 0 || value > INT32_MAX || p >= len || line[p] != ' ') return false;
  ++p;

  uint32_t at = 0;
  size_t atDigits = 0;
  while (p < len && line[p] >= '0' && line[p] <= '9') {
    const uint32_t digit = static_cast<uint32_t>(line[p] - '0');
    if (at > (UINT32_MAX - digit) / 10) return false;
    at = at * 10 + digit;
    ++p;
    ++atDigits;
  }
  if (atDigits == 0 || p != len - 1 || line[p] != '\n') return false;

  due = static_cast<int32_t>(value);
  checkedAt = at;
  return true;
}

// One cached queue entry: RVQ1 <lastRating> <word>\n, where lastRating is -1
// for a word that was never reviewed (the relay's null).
struct QueueEntry {
  int32_t lastRating = -1;
  char word[WordBookFormat::WORD_CAP] = {};
};

inline size_t buildQueueLine(char* out, size_t cap, const QueueEntry& entry) {
  if (out == nullptr || cap == 0 || entry.lastRating < -1 || entry.lastRating > 4) return 0;
  if (!WordBookFormat::validWord(entry.word)) return 0;
  const int n = snprintf(out, cap, "%s%ld %s\n", QUEUE_PREFIX, static_cast<long>(entry.lastRating), entry.word);
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

inline bool parseQueueLine(const char* line, size_t len, QueueEntry& out) {
  const size_t prefixLen = strlen(QUEUE_PREFIX);
  if (line == nullptr || len <= prefixLen + 3 || line[len - 1] != '\n') return false;
  if (memcmp(line, QUEUE_PREFIX, prefixLen) != 0) return false;

  size_t p = prefixLen;
  bool negative = false;
  if (p < len && line[p] == '-') {
    negative = true;
    ++p;
  }
  size_t digits = 0;
  uint32_t rating = 0;
  while (p < len && line[p] >= '0' && line[p] <= '9') {
    rating = rating * 10 + static_cast<uint32_t>(line[p] - '0');
    if (rating > 4) return false;
    ++p;
    ++digits;
  }
  if (digits == 0 || p >= len || line[p] != ' ') return false;
  if (negative && rating != 1) return false;  // only -1 is meaningful
  ++p;

  const size_t wordLen = len - 1 - p;
  if (wordLen == 0 || wordLen >= WordBookFormat::WORD_CAP) return false;
  char word[WordBookFormat::WORD_CAP];
  memcpy(word, line + p, wordLen);
  word[wordLen] = '\0';
  if (!WordBookFormat::validWord(word)) return false;

  out.lastRating = negative ? -1 : static_cast<int32_t>(rating);
  memcpy(out.word, word, wordLen + 1);
  return true;
}

// Writes "/.crosspoint/learning/cards/<word>.txt" into `out`. False when the
// word is not a valid token or the path does not fit.
inline bool cardPath(char* out, size_t cap, const char* word) {
  if (out == nullptr || cap == 0 || !WordBookFormat::validWord(word)) return false;
  const int n = snprintf(out, cap, "%s/%s.txt", CARD_DIR, word);
  return n > 0 && static_cast<size_t>(n) < cap;
}

}  // namespace ReviewFormat
}  // namespace Learning
