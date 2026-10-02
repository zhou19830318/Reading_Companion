#include "ChatHistory.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

// clang-format off
// HalStorage.h must be seen before lwip-sensitive headers; keep Arduino first
// (same ordering constraint as VoiceActivity.cpp / SttClient.cpp).
#include <Arduino.h>
// clang-format on

namespace OpenClaw {

bool ChatHistory::append(const uint64_t tsMs, const int utcOffsetQuarterHoursBiased, const bool isUser,
                         const char* text, char* lineBuf, const size_t lineCap) {
  if (lineBuf == nullptr || lineCap < 32) {
    LOG_ERR("OCH", "append: no line scratch (%u bytes)", static_cast<unsigned>(lineCap));
    return false;
  }
  if (text == nullptr) return false;
  char dayName[ChatHistoryFormat::DATE_PATH_SIZE];
  if (!ChatHistoryFormat::dayPath(dayName, static_cast<int64_t>(tsMs), utcOffsetQuarterHoursBiased)) {
    // Clock not synced: a record filed under a wrong date cannot be found
    // again from the day panel, so skip rather than misfile.
    LOG_DBG("OCH", "no sane clock, history skipped");
    return false;
  }

  char path[48];
  if (!buildDayPath(path, sizeof(path), dayName)) return false;

  size_t len = ChatHistoryFormat::buildLine(lineBuf, lineCap, tsMs, isUser, std::string_view(text));
  if (len == 0) return false;
  // Emoji and other symbols no panel font can draw are dropped from the line
  // before it hits the file: read back, they would paint .notdef boxes in the
  // history panel. One in-place pass — the ASCII prefix and the trailing
  // newline are never flagged, so the record stays well-formed.
  len -= ChatHistoryFormat::stripStrippableSymbols(lineBuf, len);

  if (!Storage.ensureDirectoryExists(ChatHistoryFormat::DIR)) {
    LOG_ERR("OCH", "mkdir failed: %s", ChatHistoryFormat::DIR);
    return false;
  }

  // Append mode: one line per completed round trip. The HalFile destructor
  // closes (and releases the storage mutex) at scope exit; no explicit close
  // for a local handle (DESTRUCTOR_CLOSES_FILE=1).
  HalFile f = Storage.open(path, O_WRITE | O_APPEND | O_CREAT);
  if (!f.isOpen()) {
    LOG_ERR("OCH", "open for append failed: %s", path);
    return false;
  }
  const size_t written = f.write(lineBuf, len);
  if (written != len) {
    LOG_ERR("OCH", "short write %u/%u: %s", static_cast<unsigned>(written), static_cast<unsigned>(len), path);
    return false;
  }
  LOG_INF("OCH", "history +1 line (%u bytes) -> %s", static_cast<unsigned>(len), dayName);
  return true;
}

bool ChatHistory::removeDay(const char* dayName) {
  if (dayName == nullptr || dayName[0] == '\0') return false;
  char path[48];
  if (!buildDayPath(path, sizeof(path), dayName)) return false;
  // Nothing is open on the path (append/loadDay use local HalFiles that are
  // destroyed by now), so no close-before-remove ordering applies.
  if (!Storage.remove(path)) {
    LOG_ERR("OCH", "remove failed: %s", path);
    return false;
  }
  LOG_INF("OCH", "history day deleted: %s", dayName);
  return true;
}

size_t ChatHistory::listDays(char days[][DAY_NAME_SIZE], const size_t cap) {
  if (cap == 0) return 0;
  // Directory names only — no file content is read here. SDCardManager sorts
  // nothing; the plain names sort lexicographically, and YYYY-MM-DD sorts
  // chronologically, so a simple insertion into a sorted list is enough.
  // listFiles() returns up to `maxFiles` entries; ask for more than we keep
  // so a stray file (the directory may also hold other things someday) does
  // not crowd the window. Names that do not parse as YYYY-MM-DD.jsonl are
  // dropped here — the hierarchy view downstream keys entirely on the date.
  const std::vector<String> files = Storage.listFiles(ChatHistoryFormat::DIR, 160);
  size_t count = 0;
  for (const String& name : files) {
    if (!name.endsWith(".jsonl")) continue;
    int y = 0;
    int m = 0;
    int d = 0;
    if (!ChatHistoryFormat::parseDayDate(name.c_str(), y, m, d)) continue;
    // Sorted insert, newest first.
    size_t pos = 0;
    while (pos < count && name.compareTo(days[pos]) <= 0) pos++;
    if (pos >= cap) continue;
    const size_t end = count < cap ? count : cap;
    if (count < cap) count++;
    for (size_t k = end; k > pos; k--) {
      strncpy(days[k], days[k - 1], DAY_NAME_SIZE);
    }
    strncpy(days[pos], name.c_str(), DAY_NAME_SIZE - 1);
    days[pos][DAY_NAME_SIZE - 1] = '\0';
  }
  return count;
}

namespace {

// Accumulation state for one loadDays() call, shared across the files it
// reads. Invariant mirrors the old single-day loader: entries are front-
// packed in the pool (textOff runs 0, textLen+1, ... and the blocks are
// contiguous up to `used`), so evicting the head only re-labels offsets.
struct PoolState {
  char* pool;
  size_t cap;
  ChatHistory::Entry* entries;
  size_t entryCap;
  size_t count;
  size_t used;
};

void dropOldestEntry(PoolState& st) {
  const size_t dropLen = st.entries[0].textLen + 1;
  for (size_t k = 1; k < st.count; k++) st.entries[k - 1] = st.entries[k];
  st.count--;
  st.used -= dropLen;
  for (size_t k = 0; k < st.count; k++) {
    st.entries[k].textOff = static_cast<uint32_t>(st.entries[k].textOff - dropLen);
  }
}

// Decode one line straight into the free tail of the pool — no intermediate
// 160-byte text buffer to copy from. When the tail is too small (or the index
// is full), evict the oldest turn and retry; a record that cannot fit even an
// empty pool, or a line that fails to parse, is skipped.
bool placeEntry(PoolState& st, const char* line, const size_t lineLen) {
  ChatHistoryFormat::Entry e{};
  size_t textLen = 0;
  bool overflow = false;
  for (int guard = 0; guard <= static_cast<int>(st.entryCap) + 1; ++guard) {
    if (st.count >= st.entryCap) {
      dropOldestEntry(st);  // entryCap >= 1, so there is always one to drop here
      continue;
    }
    overflow = false;
    if (ChatHistoryFormat::parseLine(std::string_view(line, lineLen), e, st.pool + st.used, st.cap - st.used, textLen,
                                     &overflow)) {
      // Files written before the reply strip existed still carry emoji;
      // strip them on the way in so the panel never paints a box. In place,
      // before any offset is handed out, so textOff/textLen stay consistent.
      textLen -= ChatHistoryFormat::stripStrippableSymbols(st.pool + st.used, textLen);
      if (textLen > UINT16_MAX) return false;  // Entry.textLen is 16-bit
      ChatHistory::Entry& slot = st.entries[st.count++];
      slot.textOff = static_cast<uint32_t>(st.used);
      slot.textLen = static_cast<uint16_t>(textLen);
      slot.tsMs = e.tsMs;
      slot.isUser = e.isUser;
      st.used += textLen + 1;
      return true;
    }
    if (!overflow) return false;      // malformed line: nothing to gain by evicting
    if (st.count == 0) return false;  // text longer than an empty pool
    dropOldestEntry(st);
  }
  return false;
}

// Streams one open file's lines into the shared pool state. Chunked line
// reader: HalFile::read takes the storage mutex per call, so a byte-at-a-time
// scan would cost one lock/unlock per byte — thousands of them for a multi-KB
// reply line, on the UI task. lineBuf is the caller's LINE_CAP scratch; the
// read position resets per file (one file at a time, oldest -> newest).
void loadFileInto(HalFile& f, const char* dayName, PoolState& st, char* lineBuf, const size_t lineCap) {
  size_t pos = 0;   // start of unconsumed data in lineBuf
  size_t fill = 0;  // end of valid data in lineBuf
  size_t lineNo = 0;
  bool eof = false;
  while (true) {
    const void* nl = memchr(lineBuf + pos, '\n', fill - pos);
    if (nl == nullptr) {
      if (eof) {
        if (fill > pos) {  // file does not end in a newline
          lineNo++;
          if (!placeEntry(st, lineBuf + pos, fill - pos)) {
            LOG_DBG("OCH", "skipping line %u in %s", static_cast<unsigned>(lineNo), dayName);
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
        // One record is longer than the whole scratch buffer: drop it through
        // its newline and start fresh rather than parsing a headless fragment.
        fill = 0;
        pos = 0;
        char c = '\0';
        while (f.read(&c, 1) == 1 && c != '\n') {
        }
        if (c != '\n') eof = true;
        continue;
      }
      const int rd = f.read(lineBuf + fill, lineCap - 1 - fill);
      if (rd <= 0) {
        eof = true;
        continue;
      }
      fill += static_cast<size_t>(rd);
      continue;
    }
    const size_t lineLen = static_cast<size_t>(static_cast<const char*>(nl) - (lineBuf + pos));
    lineNo++;
    if (!placeEntry(st, lineBuf + pos, lineLen)) {
      LOG_DBG("OCH", "skipping line %u in %s", static_cast<unsigned>(lineNo), dayName);
    }
    pos += lineLen + 1;
  }
}

}  // namespace

size_t ChatHistory::loadDays(const char days[][DAY_NAME_SIZE], const size_t first, const size_t count, char* pool,
                             const size_t cap, Entry* entries, const size_t entryCap, char* lineBuf,
                             const size_t lineCap) {
  if (pool == nullptr || entries == nullptr || lineBuf == nullptr || days == nullptr) {
    LOG_ERR("OCH", "loadDays: null argument");
    return 0;
  }
  if (cap == 0 || entryCap == 0 || lineCap < 32 || count == 0) {
    LOG_ERR("OCH", "loadDays: zero/undersized argument (pool %u, entries %u, line %u, days %u)",
            static_cast<unsigned>(cap), static_cast<unsigned>(entryCap), static_cast<unsigned>(lineCap),
            static_cast<unsigned>(count));
    return 0;
  }
  PoolState st{pool, cap, entries, entryCap, 0, 0};
  // Oldest -> newest across the range: the keep-the-tail eviction then ends
  // on the newest content of the whole bucket (a week/month/year summarize
  // prompt wants the bucket's latest turns, not the first day's).
  for (size_t k = count; k-- > 0;) {
    const char* dayName = days[first + k];
    char path[48];
    if (!buildDayPath(path, sizeof(path), dayName)) continue;
    HalFile f;
    if (!Storage.openFileForRead("OCH", path, f)) {
      LOG_DBG("OCH", "no history file: %s", path);
      continue;
    }
    loadFileInto(f, dayName, st, lineBuf, lineCap);
  }
  LOG_INF("OCH", "loaded %u entries (%u bytes) across %u day(s)", static_cast<unsigned>(st.count),
          static_cast<unsigned>(st.used), static_cast<unsigned>(count));
  return st.count;
}

void ChatHistory::dayLabel(const char* dayName, char* dst, const size_t cap) {
  // "2026-09-27.jsonl" -> "2026-09-27"
  if (cap == 0) return;
  size_t n = 0;
  while (dayName[n] != '\0' && dayName[n] != '.' && n + 1 < cap) {
    dst[n] = dayName[n];
    n++;
  }
  dst[n] = '\0';
}

bool ChatHistory::buildDayPath(char* out, const size_t cap, const char* dayName) {
  const int n = snprintf(out, cap, "%s/%s", ChatHistoryFormat::DIR, dayName);
  return n > 0 && static_cast<size_t>(n) < cap;
}

}  // namespace OpenClaw
