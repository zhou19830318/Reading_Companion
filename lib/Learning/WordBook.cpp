#include "WordBook.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "LineFile.h"

namespace Learning {

namespace {

constexpr size_t LINE_CAP = 128;  // see WordBookFormat::WORD_CAP
constexpr char TMP_PATH[] = "/.crosspoint/learning/words.tsv.tmp";

// Re-serialises surviving lines through the shared temp-file swap. A line that
// no longer parses is dropped, so a torn append cannot survive a rewrite.
template <typename Fn>
bool rewriteFile(Fn&& fn) {
  return rewriteLines(WordBookFormat::FILE_PATH, TMP_PATH, "LRN", [&](char* line, size_t len) -> size_t {
    WordBookFormat::Entry entry;
    if (!WordBookFormat::parseLine(line, len, entry)) {
      LOG_DBG("LRN", "rewrite: dropping malformed line");
      return 0;
    }
    if (!fn(entry)) return 0;
    return WordBookFormat::buildLine(line, LINE_CAP, entry);
  });
}

}  // namespace

bool WordBook::contains(const char* word, bool* synced) {
  if (!WordBookFormat::validWord(word)) return false;
  recoverRewrite(WordBookFormat::FILE_PATH, TMP_PATH);
  if (!Storage.exists(WordBookFormat::FILE_PATH)) return false;

  HalFile f;
  if (!Storage.openFileForRead("LRN", WordBookFormat::FILE_PATH, f)) return false;
  char line[LINE_CAP];
  size_t len = 0;
  LineReader reader(f);
  for (int r; (r = reader.next(line, sizeof(line), len)) == 1;) {
    WordBookFormat::Entry entry;
    if (!WordBookFormat::parseLine(line, len, entry)) continue;
    if (strcmp(entry.word, word) == 0) {
      if (synced != nullptr) *synced = entry.synced;
      return true;
    }
  }
  return false;
}

bool WordBook::add(const char* word, const uint32_t addedAt, bool& added) {
  added = false;
  WordBookFormat::Entry entry;
  entry.addedAt = addedAt;
  if (!WordBookFormat::validWord(word)) {
    LOG_ERR("LRN", "add: bad word");
    return false;
  }
  snprintf(entry.word, sizeof(entry.word), "%s", word);
  if (contains(word)) return true;  // duplicate

  if (!Storage.ensureDirectoryExists(WordBookFormat::DIR)) {
    LOG_ERR("LRN", "mkdir failed: %s", WordBookFormat::DIR);
    return false;
  }
  char out[LINE_CAP];
  const size_t n = WordBookFormat::buildLine(out, sizeof(out), entry);
  if (n == 0) {
    LOG_ERR("LRN", "add: line did not fit");
    return false;
  }
  HalFile f = Storage.open(WordBookFormat::FILE_PATH, O_WRITE | O_APPEND | O_CREAT);
  if (!f.isOpen()) {
    LOG_ERR("LRN", "add: open failed: %s", WordBookFormat::FILE_PATH);
    return false;
  }
  if (f.write(out, n) != n) {
    LOG_ERR("LRN", "add: write failed");
    return false;
  }
  added = true;
  return true;
}

bool WordBook::remove(const char* word) {
  if (!WordBookFormat::validWord(word)) return true;  // can never be a member
  bool removed = false;
  const bool ok = rewriteFile([&](WordBookFormat::Entry& entry) {
    if (strcmp(entry.word, word) == 0) {
      removed = true;
      return false;
    }
    return true;
  });
  if (ok && !removed) return true;  // already absent
  return ok;
}

size_t WordBook::listPending(WordBookFormat::Entry* out, const size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  recoverRewrite(WordBookFormat::FILE_PATH, TMP_PATH);
  if (!Storage.exists(WordBookFormat::FILE_PATH)) return 0;

  HalFile f;
  if (!Storage.openFileForRead("LRN", WordBookFormat::FILE_PATH, f)) return 0;
  char line[LINE_CAP];
  size_t len = 0;
  size_t count = 0;
  LineReader reader(f);
  for (int r; count < cap && (r = reader.next(line, sizeof(line), len)) == 1;) {
    WordBookFormat::Entry entry;
    if (!WordBookFormat::parseLine(line, len, entry)) continue;
    if (entry.synced) continue;
    out[count++] = entry;
  }
  return count;
}

bool WordBook::markSynced(const char* const* words, const size_t count) {
  if (words == nullptr || count == 0) return true;
  return rewriteFile([&](WordBookFormat::Entry& entry) {
    for (size_t i = 0; i < count; ++i) {
      if (words[i] != nullptr && strcmp(entry.word, words[i]) == 0) {
        entry.synced = true;
        break;
      }
    }
    return true;
  });
}

}  // namespace Learning
