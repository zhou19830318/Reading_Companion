#include "ReviewStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "LineFile.h"
#include "WordBookFormat.h"

namespace Learning {

namespace {

constexpr size_t LINE_CAP = 128;  // ReviewFormat lines stay well under this
constexpr char TAG[] = "LRN";

bool ensureStoreDir() {
  if (Storage.ensureDirectoryExists(ReviewFormat::DIR)) return true;
  LOG_ERR(TAG, "mkdir failed: %s", ReviewFormat::DIR);
  return false;
}

bool readFirstLine(const char* path, char* line, size_t cap, size_t& len) {
  len = 0;
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  LineReader reader(f);
  return reader.next(line, cap, len) == 1;
}

}  // namespace

bool ReviewStore::enqueue(const char* word, const uint8_t rating, const char* clientEventId) {
  recoverRewrite(ReviewFormat::EVENTS_PATH, ReviewFormat::EVENTS_TMP);
  ReviewFormat::Event event;
  event.rating = rating;
  if (!ReviewFormat::validEventId(clientEventId) || !WordBookFormat::validWord(word)) return false;
  snprintf(event.clientEventId, sizeof(event.clientEventId), "%s", clientEventId);
  snprintf(event.word, sizeof(event.word), "%s", word);

  char out[LINE_CAP];
  const size_t n = ReviewFormat::buildEventLine(out, sizeof(out), event);
  if (n == 0) return false;
  if (!ensureStoreDir()) return false;

  // A repeated Confirm for the same word must not queue twice. The relay's
  // idempotency already protects it, but a duplicate line would keep the
  // pending count wrong.
  if (Storage.exists(ReviewFormat::EVENTS_PATH)) {
    HalFile src;
    if (Storage.openFileForRead(TAG, ReviewFormat::EVENTS_PATH, src)) {
      char line[LINE_CAP];
      size_t len = 0;
      LineReader reader(src);
      for (int r; (r = reader.next(line, sizeof(line), len)) == 1;) {
        ReviewFormat::Event existing;
        if (!ReviewFormat::parseEventLine(line, len, existing)) continue;
        if (strcmp(existing.clientEventId, clientEventId) == 0) return true;
      }
    }
  }

  HalFile f = Storage.open(ReviewFormat::EVENTS_PATH, O_WRITE | O_APPEND | O_CREAT);
  if (!f.isOpen()) {
    LOG_ERR(TAG, "review queue open failed: %s", ReviewFormat::EVENTS_PATH);
    return false;
  }
  if (f.write(out, n) != n) {
    LOG_ERR(TAG, "review queue write failed");
    return false;
  }
  return true;
}

size_t ReviewStore::listPending(ReviewFormat::Event* out, const size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  recoverRewrite(ReviewFormat::EVENTS_PATH, ReviewFormat::EVENTS_TMP);
  if (!Storage.exists(ReviewFormat::EVENTS_PATH)) return 0;

  HalFile f;
  if (!Storage.openFileForRead(TAG, ReviewFormat::EVENTS_PATH, f)) return 0;
  char line[LINE_CAP];
  size_t len = 0;
  size_t count = 0;
  LineReader reader(f);
  for (int r; count < cap && (r = reader.next(line, sizeof(line), len)) == 1;) {
    if (ReviewFormat::parseEventLine(line, len, out[count])) ++count;
  }
  return count;
}

size_t ReviewStore::pendingCount() {
  recoverRewrite(ReviewFormat::EVENTS_PATH, ReviewFormat::EVENTS_TMP);
  if (!Storage.exists(ReviewFormat::EVENTS_PATH)) return 0;
  HalFile f;
  if (!Storage.openFileForRead(TAG, ReviewFormat::EVENTS_PATH, f)) return 0;
  char line[LINE_CAP];
  size_t len = 0;
  size_t count = 0;
  LineReader reader(f);
  for (int r; (r = reader.next(line, sizeof(line), len)) == 1;) {
    ReviewFormat::Event event;
    if (ReviewFormat::parseEventLine(line, len, event)) ++count;
  }
  return count;
}

bool ReviewStore::remove(const char* clientEventId) {
  if (!ReviewFormat::validEventId(clientEventId)) return true;
  bool removed = false;
  const bool ok = rewriteLines(ReviewFormat::EVENTS_PATH, ReviewFormat::EVENTS_TMP, TAG,
                               [&](char* line, size_t len) -> size_t {
                                 ReviewFormat::Event event;
                                 if (!ReviewFormat::parseEventLine(line, len, event)) return 0;
                                 if (strcmp(event.clientEventId, clientEventId) == 0) {
                                   removed = true;
                                   return 0;
                                 }
                                 return len;
                               });
  if (ok && !removed) return true;  // already absent
  return ok;
}

bool ReviewStore::removeWord(const char* word) {
  if (!WordBookFormat::validWord(word)) return true;
  return rewriteLines(ReviewFormat::EVENTS_PATH, ReviewFormat::EVENTS_TMP, TAG,
                      [&](char* line, size_t len) -> size_t {
                        ReviewFormat::Event event;
                        if (!ReviewFormat::parseEventLine(line, len, event)) return 0;
                        return strcmp(event.word, word) == 0 ? 0 : len;
                      });
}

bool ReviewStore::saveCard(const char* word, const char* text) {
  if (text == nullptr || text[0] == '\0') return false;
  char path[96];
  if (!ReviewFormat::cardPath(path, sizeof(path), word)) return false;
  if (!Storage.ensureDirectoryExists(ReviewFormat::CARD_DIR)) {
    LOG_ERR(TAG, "mkdir failed: %s", ReviewFormat::CARD_DIR);
    return false;
  }
  const size_t len = strlen(text);
  HalFile f = Storage.open(path, O_WRITE | O_CREAT | O_TRUNC);
  if (!f.isOpen()) {
    LOG_ERR(TAG, "card cache open failed: %s", path);
    return false;
  }
  if (f.write(text, len) != len) {
    LOG_ERR(TAG, "card cache write failed: %s", path);
    return false;
  }
  return true;
}

size_t ReviewStore::loadCard(const char* word, char* out, const size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  out[0] = '\0';
  char path[96];
  if (!ReviewFormat::cardPath(path, sizeof(path), word)) return 0;
  if (!Storage.exists(path)) return 0;
  return Storage.readFileToBuffer(path, out, cap);
}

bool ReviewStore::saveDue(const int32_t due, const uint32_t checkedAt) {
  char out[LINE_CAP];
  const size_t n = ReviewFormat::buildStateLine(out, sizeof(out), due, checkedAt);
  if (n == 0) return false;
  if (!ensureStoreDir()) return false;
  HalFile f = Storage.open(ReviewFormat::STATE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (!f.isOpen()) return false;
  return f.write(out, n) == n;
}

bool ReviewStore::loadDue(int32_t& due, uint32_t& checkedAt) {
  if (!Storage.exists(ReviewFormat::STATE_PATH)) return false;
  char line[LINE_CAP];
  size_t len = 0;
  if (!readFirstLine(ReviewFormat::STATE_PATH, line, sizeof(line), len)) return false;
  return ReviewFormat::parseStateLine(line, len, due, checkedAt);
}

bool ReviewStore::saveQueue(const ReviewFormat::QueueEntry* entries, const size_t count) {
  if (entries == nullptr && count > 0) return false;
  if (!ensureStoreDir()) return false;
  HalFile f = Storage.open(ReviewFormat::QUEUE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (!f.isOpen()) {
    LOG_ERR(TAG, "queue cache open failed: %s", ReviewFormat::QUEUE_PATH);
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    char line[LINE_CAP];
    const size_t n = ReviewFormat::buildQueueLine(line, sizeof(line), entries[i]);
    if (n == 0) continue;  // a hostile word must not abort the whole snapshot
    if (f.write(line, n) != n) {
      LOG_ERR(TAG, "queue cache write failed");
      return false;
    }
  }
  return true;
}

size_t ReviewStore::loadQueue(ReviewFormat::QueueEntry* out, const size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  if (!Storage.exists(ReviewFormat::QUEUE_PATH)) return 0;
  HalFile f;
  if (!Storage.openFileForRead(TAG, ReviewFormat::QUEUE_PATH, f)) return 0;
  char line[LINE_CAP];
  size_t len = 0;
  size_t count = 0;
  LineReader reader(f);
  for (int r; count < cap && (r = reader.next(line, sizeof(line), len)) == 1;) {
    if (ReviewFormat::parseQueueLine(line, len, out[count])) ++count;
  }
  return count;
}

}  // namespace Learning
