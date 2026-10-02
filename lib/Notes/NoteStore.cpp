#include "NoteStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

// See NoteStore.h. Format: NoteFormat.h (NTF1, host-tested).
namespace Notes {

namespace {

// "/.crosspoint/notes" (18) + '/' + a full NAME_PATH_SIZE base (95) + ".ntf"
// + NUL = 119 -> 128. buildPath still guards, but the buffer must be large
// enough for every name listBooks can return, not just short ones.
constexpr size_t PATH_CAP = 128;

// <DIR>/<base>.ntf into a caller-owned path buffer.
bool buildPath(char* out, size_t cap, const char* bookBaseName) {
  if (out == nullptr || bookBaseName == nullptr || bookBaseName[0] == '\0') return false;
  const int n = snprintf(out, cap, "%s/%s%s", NoteFormat::DIR, bookBaseName, NoteFormat::EXT);
  return n > 0 && static_cast<size_t>(n) < cap;
}

}  // namespace

bool NoteStore::append(const char* bookBaseName, const uint64_t tsMs, const uint16_t spineIndex,
                       const uint16_t pageNumber, const uint16_t pageCount, const char* text, char* lineBuf,
                       const size_t lineCap) {
  if (bookBaseName == nullptr || bookBaseName[0] == '\0' || lineBuf == nullptr || lineCap < 32) {
    LOG_ERR("NOTE", "append: bad argument");
    return false;
  }
  if (text == nullptr) return false;

  const size_t len =
      NoteFormat::buildLine(lineBuf, lineCap, tsMs, spineIndex, pageNumber, pageCount, std::string_view(text));
  if (len == 0) {
    LOG_ERR("NOTE", "note line did not fit %u bytes", static_cast<unsigned>(lineCap));
    return false;
  }

  if (!Storage.ensureDirectoryExists(NoteFormat::DIR)) {
    LOG_ERR("NOTE", "mkdir failed: %s", NoteFormat::DIR);
    return false;
  }

  char path[PATH_CAP];
  if (!buildPath(path, sizeof(path), bookBaseName)) return false;

  // Append mode: one line per saved note. The HalFile destructor closes (and
  // releases the storage mutex) at scope exit; no explicit close for a local
  // handle (DESTRUCTOR_CLOSES_FILE=1).
  HalFile f = Storage.open(path, O_WRITE | O_APPEND | O_CREAT);
  if (!f.isOpen()) {
    LOG_ERR("NOTE", "open for append failed: %s", path);
    return false;
  }
  const size_t written = f.write(lineBuf, len);
  if (written != len) {
    LOG_ERR("NOTE", "short write %u/%u: %s", static_cast<unsigned>(written), static_cast<unsigned>(len), path);
    return false;
  }
  LOG_INF("NOTE", "note saved (%u bytes) -> %s", static_cast<unsigned>(len), bookBaseName);
  return true;
}

size_t NoteStore::listBooks(char names[][NoteFormat::NAME_PATH_SIZE], const size_t cap) {
  if (cap == 0) return 0;
  // File names only — no content is read here. Names sort lexicographically;
  // unlike the chat history there is no date in the name, so "newest first"
  // is not derivable from the name alone. mtime would need an extra stat per
  // file; for v1 the browser sorts by name and the newest note is found by
  // opening a book, which is how the reader navigates anyway.
  const std::vector<String> files = Storage.listFiles(NoteFormat::DIR, 64);
  size_t count = 0;
  for (const String& name : files) {
    if (!name.endsWith(NoteFormat::EXT)) continue;
    const size_t baseLen = name.length() - strlen(NoteFormat::EXT);
    if (baseLen == 0 || baseLen >= NoteFormat::NAME_PATH_SIZE) continue;
    // Sorted insert, case-insensitive enough for file names (plain byte order,
    // consistent within one browser list).
    size_t pos = 0;
    while (pos < count && name.compareTo(names[pos]) > 0) pos++;
    if (pos >= cap) continue;
    if (count < cap) count++;
    for (size_t k = count; k > pos + 1; k--) {
      strncpy(names[k - 1], names[k - 2], NoteFormat::NAME_PATH_SIZE);
    }
    strncpy(names[pos], name.c_str(), NoteFormat::NAME_PATH_SIZE - 1);
    names[pos][NoteFormat::NAME_PATH_SIZE - 1] = '\0';
    // strip the extension from the stored base name
    char* dot = strstr(names[pos], NoteFormat::EXT);
    if (dot != nullptr) *dot = '\0';
  }
  return count;
}

size_t NoteStore::load(const char* bookBaseName, char* pool, const size_t cap, Entry* entries, const size_t entryCap,
                       char* lineBuf, const size_t lineCap) {
  if (pool == nullptr || entries == nullptr || lineBuf == nullptr) {
    LOG_ERR("NOTE", "load: null argument");
    return 0;
  }
  if (cap == 0 || entryCap == 0 || lineCap < 32) {
    LOG_ERR("NOTE", "load: zero/undersized argument (pool %u, entries %u, line %u)", static_cast<unsigned>(cap),
            static_cast<unsigned>(entryCap), static_cast<unsigned>(lineCap));
    return 0;
  }
  char path[PATH_CAP];
  if (!buildPath(path, sizeof(path), bookBaseName)) return 0;

  HalFile f;
  if (!Storage.openFileForRead("NOTE", path, f)) {
    LOG_DBG("NOTE", "no note file: %s", path);
    return 0;
  }

  size_t count = 0;
  size_t used = 0;
  // Same front-packed-pool invariant as ChatHistory::loadDay: entries are
  // packed from the pool start; evicting the oldest re-labels offsets but
  // never memmoves text.
  const auto dropOldest = [&] {
    const size_t dropLen = entries[0].textLen + 1;
    for (size_t k = 1; k < count; k++) entries[k - 1] = entries[k];
    count--;
    used -= dropLen;
    for (size_t k = 0; k < count; k++) {
      entries[k].textOff = static_cast<uint32_t>(entries[k].textOff - dropLen);
    }
  };

  const auto placeEntry = [&](const char* line, const size_t lineLen) {
    Entry e{};
    size_t textLen = 0;
    bool overflow = false;
    for (int guard = 0; guard <= static_cast<int>(entryCap) + 1; ++guard) {
      if (count >= entryCap) {
        dropOldest();
        continue;
      }
      overflow = false;
      if (NoteFormat::parseLine(std::string_view(line, lineLen), e, pool + used, cap - used, textLen, &overflow)) {
        if (textLen > UINT16_MAX) return false;
        Entry& slot = entries[count++];
        slot.textOff = static_cast<uint32_t>(used);
        slot.textLen = static_cast<uint16_t>(textLen);
        slot.tsMs = e.tsMs;
        slot.spineIndex = e.spineIndex;
        slot.pageNumber = e.pageNumber;
        slot.pageCount = e.pageCount;
        used += textLen + 1;
        return true;
      }
      if (!overflow) return false;   // malformed line: nothing to gain by evicting
      if (count == 0) return false;  // text longer than an empty pool
      dropOldest();
    }
    return false;
  };

  // Chunked line reader (identical shape to ChatHistory::loadDay): HalFile
  // locking per read call makes byte-at-a-time scans thousands of mutex
  // round trips on the UI task.
  size_t pos = 0;
  size_t fill = 0;
  size_t lineNo = 0;
  bool eof = false;
  while (true) {
    const void* nl = memchr(lineBuf + pos, '\n', fill - pos);
    if (nl == nullptr) {
      if (eof) {
        if (fill > pos) {
          lineNo++;
          if (!placeEntry(lineBuf + pos, fill - pos)) {
            LOG_DBG("NOTE", "skipping line %u in %s", static_cast<unsigned>(lineNo), bookBaseName);
          }
        }
        break;
      }
      if (pos > 0) {
        memmove(lineBuf, lineBuf + pos, fill - pos);
        fill -= pos;
        pos = 0;
      }
      if (fill + 1 >= lineCap) {
        // One line longer than the scratch: skip to the next newline.
        LOG_DBG("NOTE", "oversized line skipped in %s", bookBaseName);
        fill = 0;
        pos = 0;
        // Drain the rest of this line directly.
        int c;
        while ((c = f.read()) > 0) {
          if (c == '\n') break;
        }
        continue;
      }
      const int n = f.read(lineBuf + fill, lineCap - fill - 1);
      if (n <= 0) {
        eof = true;
        continue;
      }
      fill += static_cast<size_t>(n);
      continue;
    }
    lineNo++;
    placeEntry(lineBuf + pos, static_cast<size_t>(static_cast<const char*>(nl) - (lineBuf + pos)));
    pos = static_cast<size_t>(static_cast<const char*>(nl) - lineBuf) + 1;
    if (pos >= fill) {
      pos = 0;
      fill = 0;
    }
  }
  LOG_DBG("NOTE", "loaded %u notes (%u bytes) from %s", static_cast<unsigned>(count), static_cast<unsigned>(used),
          bookBaseName);
  return count;
}

bool NoteStore::removeEntry(const char* bookBaseName, const size_t index, char* lineBuf, const size_t lineCap) {
  if (bookBaseName == nullptr || bookBaseName[0] == '\0' || lineBuf == nullptr || lineCap < 32) {
    LOG_ERR("NOTE", "removeEntry: bad argument");
    return false;
  }
  char path[PATH_CAP];
  if (!buildPath(path, sizeof(path), bookBaseName)) return false;
  if (!Storage.exists(path)) return false;

  // The sidecar names must not end in .ntf or listBooks would show them.
  char tmpPath[PATH_CAP];
  char bakPath[PATH_CAP];
  const int tmpLen = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  const int bakLen = snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
  if (tmpLen <= 0 || static_cast<size_t>(tmpLen) >= sizeof(tmpPath) || bakLen <= 0 ||
      static_cast<size_t>(bakLen) >= sizeof(bakPath)) {
    LOG_ERR("NOTE", "removeEntry: path too long: %s", path);
    return false;
  }

  size_t keptLines = 0;
  bool removed = false;
  bool failed = false;

  // Copy every line but `index` into the temp file. Scoped so both handles
  // are closed (DESTRUCTOR_CLOSES_FILE) before the renames below.
  {
    HalFile src;
    if (!Storage.openFileForRead("NOTE", path, src)) {
      LOG_ERR("NOTE", "removeEntry: open failed: %s", path);
      return false;
    }
    HalFile dst = Storage.open(tmpPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!dst.isOpen()) {
      LOG_ERR("NOTE", "removeEntry: temp open failed: %s", tmpPath);
      return false;
    }

    // Chunked line copy, same shape as load(): complete lines out of the
    // scratch, the partial tail carried over, one write per kept line. Index
    // counting assumes every line parses — load() would have skipped a
    // malformed one and the browser indices would already be off.
    size_t pos = 0;
    size_t fill = 0;
    size_t lineNo = 0;
    bool eof = false;

    const auto handleLine = [&](const char* line, const size_t len) {
      if (lineNo == index) {
        removed = true;
      } else {
        if (dst.write(line, len) != len) failed = true;
        keptLines++;
      }
      lineNo++;
    };

    while (!failed) {
      const void* nl = memchr(lineBuf + pos, '\n', fill - pos);
      if (nl == nullptr) {
        if (eof) {
          if (fill > pos) handleLine(lineBuf + pos, fill - pos);
          break;
        }
        if (pos > 0) {
          memmove(lineBuf, lineBuf + pos, fill - pos);
          fill -= pos;
          pos = 0;
        }
        if (fill + 1 >= lineCap) {
          // A line longer than the scratch cannot be copied without
          // truncating it: abort and leave the file as it was.
          LOG_ERR("NOTE", "removeEntry: line over %u bytes in %s", static_cast<unsigned>(lineCap), bookBaseName);
          failed = true;
          break;
        }
        const int n = src.read(lineBuf + fill, lineCap - fill - 1);
        if (n <= 0) {
          eof = true;
          continue;
        }
        fill += static_cast<size_t>(n);
        continue;
      }
      const size_t lineLen = static_cast<size_t>(static_cast<const char*>(nl) - (lineBuf + pos)) + 1;
      handleLine(lineBuf + pos, lineLen);
      pos += lineLen;
      if (pos >= fill) {
        pos = 0;
        fill = 0;
      }
    }
  }

  if (failed) {
    Storage.remove(tmpPath);
    return false;
  }
  if (!removed) {
    Storage.remove(tmpPath);
    LOG_ERR("NOTE", "removeEntry: index %u out of range in %s", static_cast<unsigned>(index), bookBaseName);
    return false;
  }
  if (keptLines == 0) {
    // Last note of the book: the file itself goes away.
    Storage.remove(tmpPath);
    if (!Storage.remove(path)) {
      LOG_ERR("NOTE", "removeEntry: could not delete %s", path);
      return false;
    }
    LOG_INF("NOTE", "removed last note of %s", bookBaseName);
    return true;
  }
  // Swap in the rewrite: move the old file aside first, so a failed rename
  // can roll the untouched copy back instead of dropping the book's notes.
  if (!Storage.rename(path, bakPath)) {
    Storage.remove(tmpPath);
    LOG_ERR("NOTE", "removeEntry: could not move %s aside", path);
    return false;
  }
  if (!Storage.rename(tmpPath, path)) {
    Storage.rename(bakPath, path);
    Storage.remove(tmpPath);
    LOG_ERR("NOTE", "removeEntry: rename failed for %s", path);
    return false;
  }
  Storage.remove(bakPath);
  LOG_INF("NOTE", "removed note %u of %s (%u kept)", static_cast<unsigned>(index), bookBaseName,
          static_cast<unsigned>(keptLines));
  return true;
}

bool NoteStore::removeBook(const char* bookBaseName) {
  char path[PATH_CAP];
  if (!buildPath(path, sizeof(path), bookBaseName)) return false;
  if (!Storage.exists(path)) return true;  // already gone: report success
  return Storage.remove(path);
}

}  // namespace Notes
