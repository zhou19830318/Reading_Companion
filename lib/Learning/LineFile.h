#pragma once

// Shared line-file plumbing for the SD-backed learning stores (WordBook,
// ReviewStore). Device-only: HalStorage. Keeping one LineReader and one
// rewrite path means the crash-safety rule (temp file + remove + rename) and
// the overlong-line rule cannot drift between the two stores.

#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>

namespace Learning {

// Pulls '\n'-terminated lines out of a HalFile in fixed-size chunks. Overlong
// lines are reported as one (parse-rejected) record and then skipped whole, so
// a corrupt byte run cannot shift every later line onto the wrong entry.
class LineReader {
 public:
  explicit LineReader(HalFile& file) : file_(file) {}

  // 1 = a line (NUL-terminated, `len` INCLUDES the '\n' — parseLine's
  // contract), 0 = EOF, -1 = error.
  int next(char* line, size_t cap, size_t& len) {
    len = 0;
    bool overlong = false;
    for (;;) {
      if (pos_ < fill_) {
        const char c = buf_[pos_++];
        if (c == '\n') {
          if (overlong || len + 1 >= cap) {
            len = cap;  // garbage; parseLine rejects it
            return 1;
          }
          line[len++] = '\n';
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

inline void recoverRewrite(const char* path, const char* tmpPath) {
  if (!Storage.exists(path) && Storage.exists(tmpPath) && Storage.rename(tmpPath, path)) {
    LOG_INF("LRN", "rewrite: recovered completed temp file: %s", tmpPath);
  }
}

// Streams `path` through `fn(line, len) -> newLen` into `tmpPath` and swaps it
// in. fn may rewrite the line in place and returns the new length; 0 drops the
// line. True when the file is absent (a missing store is an empty one) or the
// rewrite landed. If power was lost between the rename steps, the completed
// temp file is recovered before treating the store as empty. The caller parses
// `line` with its own format helper, so malformed records are dropped by it.
template <typename Fn>
bool rewriteLines(const char* path, const char* tmpPath, const char* tag, Fn&& fn) {
  recoverRewrite(path, tmpPath);
  if (!Storage.exists(path)) return true;
  {
    HalFile src;
    if (!Storage.openFileForRead(tag, path, src)) {
      LOG_ERR("LRN", "rewrite: open failed: %s", path);
      return false;
    }
    HalFile dst = Storage.open(tmpPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!dst.isOpen()) {
      LOG_ERR("LRN", "rewrite: temp open failed: %s", tmpPath);
      return false;
    }

    char line[128];
    size_t len = 0;
    LineReader reader(src);
    for (;;) {
      const int r = reader.next(line, sizeof(line), len);
      if (r == 0) break;
      if (r < 0) {
        LOG_ERR("LRN", "rewrite: read failed: %s", path);
        return false;
      }
      const size_t keep = fn(line, len);
      if (keep == 0) continue;
      if (dst.write(line, keep) != keep) {
        LOG_ERR("LRN", "rewrite: write failed");
        return false;
      }
    }
    // dst's destructor closes it before the directory operations below.
  }

  if (!Storage.remove(path)) {
    LOG_ERR("LRN", "rewrite: remove failed: %s", path);
    Storage.remove(tmpPath);
    return false;
  }
  if (!Storage.rename(tmpPath, path)) {
    LOG_ERR("LRN", "rewrite: rename failed, retained temp file: %s", tmpPath);
    return false;
  }
  return true;
}

}  // namespace Learning
