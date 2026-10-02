#pragma once

#include <cstddef>
#include <cstdint>

#include "ChatHistoryFormat.h"

// SD-backed chat history (plan §8.3/§8.6): append-only JSONL under
// /.crosspoint/chat/, one file per local day. This is the only layer that
// touches HalStorage for history; VoiceActivity renders from the structures
// below and never opens a file itself.
//
// Memory discipline: a full day is never loaded as one String (the SD card
// holds days of conversation; String doubling on a 8 MB PSRAM heap is fine,
// but the historical UI only needs one screen of text at a time). Instead:
//   - listDays()  reads directory names only (SDCardManager::listFiles).
//   - loadDays()  streams day files through parseLine into a fixed-size
//                 PSRAM pool the caller owns, keeping the last N bytes of
//                 text (newest messages) and the per-entry index. One call
//                 can span a year/month/week bucket: files are fed oldest ->
//                 newest so the keep-the-tail eviction ends on the newest
//                 content.
//   - append()    opens the day file in append mode, writes one line, closes.
//                 One write per completed round trip, never per UI action —
//                 same SPIFFS/SD throttling discipline as progress saves.
namespace OpenClaw {

class ChatHistory {
 public:
  // Index of one parsed entry inside the day pool. Same shape as the format
  // layer's parsed record, aliased so loadDays() can hand it straight over.
  using Entry = ChatHistoryFormat::Entry;

  static constexpr size_t MAX_ENTRIES = 32;

  // Scratch for one JSONL line, shared by append() and loadDays(). A streamed
  // reply can be CHAT_BUF_CAP (4 KB) of text and every quote, backslash or
  // control byte doubles (or sextuples) on escape, so one line wants ~6 KB —
  // which is twice the loop task's stack (4 KB, sdkconfig.onepage) and more
  // than the largest internal block once WiFi is up (~4 KB). Hence
  // caller-owned PSRAM: VoiceActivity allocates it once in onEnter().
  // append() and loadDays() both run on that task and never overlap, so they
  // take the same buffer.
  static constexpr size_t LINE_CAP = 6144;

  // Day names retained for the browser: "YYYY-MM-DD.jsonl" (16 chars + NUL)
  // sorted newest first. 128 ≈ four months of continuous chat — enough to
  // derive the year/month/week/day hierarchy over a realistic window while
  // the member array stays at 128×20 = 2560 B of RAM (a VoiceActivity
  // member, freed with the activity). The directory listing asks for 160 so
  // stray files do not crowd out valid day names.
  static constexpr size_t DAY_NAME_SIZE = 20;
  static constexpr size_t MAX_DAYS = 128;

  // ── write side ────────────────────────────────────────────────────────
  // Appends one turn to the day file of `epochMs` (local offset applied).
  // Creates the directory and file on first use. Returns false on SD failure
  // (the round trip itself is unaffected — history is best-effort).
  // lineBuf/lineCap: caller-owned scratch, LINE_CAP bytes recommended. A
  // line that does not fit is truncated at an escape boundary rather than
  // written short (see buildLine), so a huge reply still lands as history.
  static bool append(uint64_t tsMs, int utcOffsetQuarterHoursBiased, bool isUser, const char* text, char* lineBuf,
                     size_t lineCap);

  // Deletes one day's file (name from listDays). Best-effort like append:
  // returns false on SD failure, true when the file is gone. No handle is
  // open on the path at call time (append/loadDays use local files).
  static bool removeDay(const char* dayName);

  // ── read side ─────────────────────────────────────────────────────────
  // Lists day file names, newest first. Only names that parse as
  // "YYYY-MM-DD.jsonl" count (a foreign file in the directory cannot appear
  // in the browser). Returns the count (<= cap).
  static size_t listDays(char days[][DAY_NAME_SIZE], size_t cap);

  // Loads days[first, first+count) into `pool`, feeding the files OLDEST
  // FIRST so the pool keeps the newest tail across the whole range (the
  // per-file keep-the-tail policy above, applied to the union). A summarize
  // prompt over a month/week/year bucket therefore sees the bucket's most
  // recent turns. Entries end up ordered oldest -> newest.
  //   days/first/count: slice of a listDays() result (first+count within it)
  //   pool/cap:         caller-owned byte pool (PSRAM recommended)
  //   entries:          caller-owned index array (entryCap entries)
  //   lineBuf/cap:      caller-owned read scratch, LINE_CAP bytes
  // Returns the number of entries loaded (0 on missing files; unparseable
  // lines are skipped per line, never fatal).
  static size_t loadDays(const char days[][DAY_NAME_SIZE], size_t first, size_t count, char* pool, size_t cap,
                         Entry* entries, size_t entryCap, char* lineBuf, size_t lineCap);

  // "YYYY-MM-DD" label from a day file name (writes into dst, NUL-terminated).
  static void dayLabel(const char* dayName, char* dst, size_t cap);

 private:
  static bool buildDayPath(char* out, size_t cap, const char* dayName);
};

}  // namespace OpenClaw
