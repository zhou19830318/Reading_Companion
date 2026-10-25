#include "WordBook.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace Learning {

namespace {

constexpr size_t LINE_CAP = 128;  // see WordBookFormat::WORD_CAP
constexpr char TMP_PATH[] = "/.crosspoint/learning/words.tsv.tmp";

// Pulls '\n'-terminated lines out of a HalFile in fixed-size chunks. Overlong
// lines are reported as one (parse-rejected) record and then skipped whole, so
// a corrupt byte run cannot shift every later word onto the wrong entry.
class LineReader {
 public:
  explicit LineReader(HalFile& file) : file_(file) {}

  // 1 = a line (NUL-terminated, `len` excludes the '\n'), 0 = EOF, -1 = error.
  int next(char* line, size_t cap, size_t& len) {
    len = 0;
    bool overlong = false;
    for (;;) {
      if (pos_ < fill_) {
        const char c = buf_[pos_++];
        if (c == '\n') {
          if (overlong) {
            len = cap;  // garbage; parseLine rejects it
            return 1;
          }
          line[len] = '\0';
          return 1;
        }
        if (len + 1 < cap) {
          line[len++] = c;
        } else {
          overlong = true;
        }
        continue;
      }
      if (eof_) {
        if (len > 0 && !overlong) {
          line[len] = '\0';  // torn tail without '\n': parseLine rejects
          return 1;
        }
        return 0;
      }
      const int n = file_.read(buf_, sizeof(buf_));
      if (n < 0) return -1;
      if (n == 0) {
        eof_ = true;
        continue;
      }
      pos_ = 0;
      fill_ = static_cast<size_t>(n);
    }
  }

 private:
  HalFile& file_;
  char buf_[128];
  size_t fill_ = 0;
  size_t pos_ = 0;
  bool eof_ = false;
};

// Streams the file through `fn(Entry&) -> keep` into the temp file and swaps it
// in. Lines that fail parsing are dropped. True when the file is absent (a
// missing word book is an empty one) or the rewrite landed.
template <typename Fn>
bool rewriteFile(Fn&& fn) {
  if (!Storage.exists(WordBookFormat::FILE_PATH)) return true;
  {
    HalFile src;
    if (!Storage.openFileForRead("LRN", WordBookFormat::FILE_PATH, src)) {
      LOG_ERR("LRN", "rewrite: open failed: %s", WordBookFormat::FILE_PATH);
      return false;
    }
    HalFile dst = Storage.open(TMP_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (!dst.isOpen()) {
      LOG_ERR("LRN", "rewrite: temp open failed: %s", TMP_PATH);
      return false;
    }

    char line[LINE_CAP];
    char out[LINE_CAP];
    size_t len = 0;
    LineReader reader(src);
    for (int r; (r = reader.next(line, sizeof(line), len)) == 1;) {
      WordBookFormat::Entry entry;
      if (!WordBookFormat::parseLine(line, len, entry)) {
        LOG_DBG("LRN", "rewrite: dropping malformed line");
        continue;
      }
      if (!fn(entry)) continue;
      const size_t n = WordBookFormat::buildLine(out, sizeof(out), entry);
      if (n == 0) continue;
      if (dst.write(out, n) != n) {
        LOG_ERR("LRN", "rewrite: write failed");
        Storage.remove(TMP_PATH);
        return false;
      }
    }
    // dst's destructor closes it before the directory operations below.
  }

  if (!Storage.remove(WordBookFormat::FILE_PATH)) {
    LOG_ERR("LRN", "rewrite: remove failed: %s", WordBookFormat::FILE_PATH);
    Storage.remove(TMP_PATH);
    return false;
  }
  if (!Storage.rename(TMP_PATH, WordBookFormat::FILE_PATH)) {
    LOG_ERR("LRN", "rewrite: rename failed: %s", TMP_PATH);
    Storage.remove(TMP_PATH);
    return false;
  }
  return true;
}

}  // namespace

bool WordBook::contains(const char* word, bool* synced) {
  if (!WordBookFormat::validWord(word)) return false;
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
