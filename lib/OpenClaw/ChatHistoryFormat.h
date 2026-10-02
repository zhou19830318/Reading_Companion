#pragma once

#include <OpenClawHandshake.h>
#include <Utf8.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

// Chat history on the SD card, one file per local day (plan §8.3):
//
//   /.crosspoint/chat/2026-09-27.jsonl
//   OCL1 u 1790467200123 "text"\n
//   OCL1 a 1790467205999 "reply"\n
//
// The file is append-only JSONL: one turn per line, chronological, role "u"
// (user transcript) or "a" (assistant reply). The per-day split is what the
// history panel shows (§8.3 left panel), and appending to an existing file
// keeps the SD write cost to one sector per round trip — no read-modify-write
// of a growing document, unlike the settings file.
//
// The line format is a strict subset of JSON: the value is a JSON string, so
// the file also parses with any JSONL reader. Non-ASCII bytes (the STT and
// the agent both answer in CJK) pass through as raw UTF-8, which JSON allows;
// only quote, backslash and control bytes are escaped, and control bytes are
// emitted as \u00xx so a newline can never break the one-record-per-line
// structure.
//
// "OCL1 " is the format tag: version + a line shape a stray text file in the
// same directory cannot accidentally match.
//
// Also lives here: the local-time helpers (day splitting, wall-clock stamping)
// so the file name a turn is stored under and the "HH:MM" the panel shows for
// it are computed by the same code. OnePage has no RTC and no reachable UTC-
// offset picker (the status-bar entries are X3-gated on halClock.isAvailable()),
// so callers pass the effective offset (settings.clockUtcOffsetQ).
//
// Pure C++ — no ESP-IDF, no Arduino — so test/chat_history compiles the same
// helpers the firmware uses.
namespace OpenClaw {
namespace ChatHistoryFormat {

// Root of the history store. Lives under the shared SD cache root the rest of
// the firmware already uses (.crosspoint/, see KOReaderCredentialStore.cpp).
inline constexpr const char* DIR = "/.crosspoint/chat";

// "OCL1 " — see file comment.
inline constexpr std::string_view LINE_PREFIX = "OCL1 ";

// "YYYY-MM-DD.jsonl" is 16 chars; a little headroom in case a caller builds
// "dir/" + path in the same buffer.
inline constexpr size_t DATE_PATH_SIZE = 24;

// ── codepoint classes ────────────────────────────────────────────────────────
// Returns true when the codepoint never carries conversational content and is
// not in any .cpfont: emoji and pictographs (astral planes), variation
// selectors, zero-width joiners, skin-tone modifiers, enclosed-character
// blocks (flags/number circles), emoji dingbats. Stripping them keeps replies
// on panels whose fonts (LXGW-class .cpfont, builtin Latin UI) have no glyph
// for them — otherwise every emoji renders as a box and its UTF-8 bytes sit in
// the history file forever. Kept here so the host tests pin the byte-level
// behaviour the renderer and the history file both rely on.
inline bool isStrippableSymbolCodepoint(uint32_t cp) {
  return (cp >= 0x1F000 && cp <= 0x1FFFF)     // emoji, pictographs, game symbols
         || (cp >= 0x10000 && cp <= 0x1FFFF)  // SMP: historic scripts etc. (subset of the above)
         || (cp >= 0xE000 && cp <= 0xF8FF)    // private use (icon-font codepoints)
         || (cp >= 0x2600 && cp <= 0x27BF)    // misc symbols, dingbats (most are emoji-presentation)
         || (cp >= 0x2B00 && cp <= 0x2BFF)    // misc symbols and arrows
         || (cp >= 0x1F300 && cp <= 0x1F5FF)  // (redundant but explicit) misc symbols and pictographs
         || (cp >= 0xFE00 && cp <= 0xFE0F)    // variation selectors (emoji presentation)
         || (cp >= 0x1F3FB && cp <= 0x1F3FF)  // skin-tone modifiers
         || (cp >= 0x200D && cp <= 0x200F)    // ZWJ, LRM, RLM (ZWNJ U+200C kept: some scripts need it)
         || (cp >= 0x2060 && cp <= 0x2064)    // invisible operators
         || (cp >= 0xFE0F && cp <= 0xFE0F) || (cp >= 0x1F900 && cp <= 0x1F9FF)  // supplemental symbols and pictographs
         || (cp >= 0x1FA00 && cp <= 0x1FAFF)                                    // symbols and pictographs extended-A
         || (cp == 0x20E3)                                                      // keycap base
         || (cp >= 0x2100 && cp <= 0x214F)    // letterlike (™ © often emoji-presentation)
         || (cp >= 0x2300 && cp <= 0x23FF)    // misc technical (⏰ U+23F0, ⌚⌛✂✈ live here)
         || (cp >= 0x1F100 && cp <= 0x1F2FF)  // enclosed alphanumeric supplement (flags, circled)
         || (cp >= 0x2460 && cp <= 0x24FF)    // enclosed alphanumerics (circled digits)
         || (cp >= 0x2190 && cp <= 0x21FF)    // arrows (double-encoded with VS16 as emoji)
         || (cp == 0x30FB || (cp >= 0x303D && cp <= 0x303E));
}

// True when the codepoint is worth rendering at all. Control bytes (except
// TAB/LF) are already excluded from history lines; for display they collapse
// to nothing. Unassigned/broken codepoints come through utf8NextCodepoint() as
// U+FFFD and are kept — they mark genuine decode damage rather than padding.
inline bool isRenderableCodepoint(uint32_t cp) {
  if (cp == '\t' || cp == '\n') return true;
  if (cp < 0x20 || cp == 0x7F) return false;  // control bytes
  return !isStrippableSymbolCodepoint(cp);
}

// Strips every isStrippableSymbolCodepoint() flag from `text` in place —
// NUL at `len` is rewritten one kept-length further in. Returns bytes
// removed (0 leaves the buffer untouched).
//
// One implementation shared by the three places that must agree or the panel
// shows a box again: ChatReply (live reply, delta + final),
// ChatHistory::loadDay (files written before the strip existed) and
// ChatHistory::append (keeps new files clean). Kept bytes are copied
// verbatim, so a sequence truncated at a delta boundary survives for the
// next delta to complete. Pure C++ (host-tested).
inline size_t stripStrippableSymbols(char* text, const size_t len) {
  if (text == nullptr || len == 0) return 0;
  auto* r = reinterpret_cast<const unsigned char*>(text);
  const unsigned char* const end = r + len;
  auto* w = reinterpret_cast<unsigned char*>(text);
  size_t removed = 0;
  while (r < end) {
    const unsigned char* before = r;
    const uint32_t cp = utf8NextCodepoint(&r);
    if (r == before) {
      // Decoder made no progress (embedded NUL): keep the byte, or the
      // loop would spin forever.
      *w++ = *r++;
      continue;
    }
    if (cp != 0 && isStrippableSymbolCodepoint(cp)) {
      removed += static_cast<size_t>(r - before);
      continue;
    }
    // w <= before always, so the forward copy never overtakes the read.
    while (before < r) *w++ = *before++;
  }
  if (removed == 0) return 0;
  text[len - removed] = '\0';
  return removed;
}

// ── local time ───────────────────────────────────────────────────────────────
// Days since 1970-01-01 -> calendar fields (Howard Hinnant's civil_from_days).
// Shared by dayPath() and formatTimeForOffset() so the file name and the panel
// clock cannot disagree about where a day ends.
inline void civilFromDays(const int64_t days, int& year, int& month, int& day) {
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;                                       // [0, 146096]
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
  const int64_t mp = (5 * doy + 2) / 153;                       // [0, 11]
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;               // [1, 31]
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;                  // [1, 12]
  const int64_t yy = y + (m <= 2 ? 1 : 0);
  year = static_cast<int>(yy);
  month = static_cast<int>(m);
  day = static_cast<int>(d);
}

// Clamped quarter-hour offset (biased encoding) applied to an epoch.
inline int64_t localEpochSeconds(const int64_t epochMs, const int utcOffsetQuarterHoursBiased) {
  int q = utcOffsetQuarterHoursBiased - 48;
  if (q < -48) q = -48;
  if (q > 56) q = 56;  // +14:00 — matches the settings range
  return epochMs / 1000 + q * 900;
}

// Per-day grouping uses the user's clock offset (settings.clockUtcOffsetQ is
// biased by 48: 48 = UTC+0, 0 = UTC-12, 104 = UTC+14 — the same encoding
// HalClock::formatTime takes).
bool dayPath(char (&out)[DATE_PATH_SIZE], int64_t epochMs, int utcOffsetQuarterHoursBiased);

// One turn. isUser: role "u", else "a".
//
// The line is always complete and parseable — when it does not fit, text is
// truncated at an escape boundary rather than the record being dropped
// (history is a display log, not a protocol frame; a truncated record beats a
// lost one). Returns the bytes written, or 0 only when cap is too small for
// even an empty text.
size_t buildLine(char* out, size_t cap, uint64_t tsMs, bool isUser, std::string_view text);

// One parsed record of a day line. textOff/textLen index the entry's text
// inside a caller-owned pool once it has been copied there (ChatHistory::
// loadDay); the parser itself only fills tsMs/isUser and the decoded text.
struct Entry {
  uint32_t textOff = 0;  // offset into the caller's pool
  uint16_t textLen = 0;
  uint64_t tsMs = 0;
  bool isUser = false;
};

// Parses one line [line.begin(), line.end()). Decoded text is written to
// textDst (NUL-terminated, at most textCap-1 bytes); a text longer than the
// destination fails the parse — the caller decides whether that means "skip
// the record" or "stop". Trailing CR/LF (and spaces) are tolerated; anything
// else that is not exactly the OCL1 shape returns false.
//
// overflow (optional) separates the two failure modes: it is set exactly when
// the parse failed for lack of room in textDst, never for a malformed line.
// ChatHistory::loadDay uses it to evict the oldest turn and retry instead of
// dropping a perfectly good record — an assistant reply is the one thing the
// history panel exists to show, and it is also the one entry likely to be
// larger than the destination.
bool parseLine(std::string_view line, Entry& entry, char* textDst, size_t textCap, size_t& textLen,
               bool* overflow = nullptr);

// ── history hierarchy (year / month / week / day) ─────────────────────────────
// The history browser drills Year → Month → Week → Day over the flat per-day
// files (storage stays one file per day; the hierarchy is a view built from
// the sorted name list). Every day file sits on exactly ONE chain — a week
// bucket is the Monday-anchored week runs of ITS month, so it never spills
// across a month boundary — and delete/summarize at a level operate on
// exactly the days that level's list shows. Pure C++ (host-tested).

// Browser levels, outermost first. Day is the innermost list (its Confirm
// opens the turns view); Count is the number of list levels.
enum class HistoryLevel : uint8_t { Year = 0, Month = 1, Week = 2, Day = 3, Count = 4 };

// One contiguous run of day names sharing a key at one level. first/count are
// absolute indices into the sorted (newest-first) name array. value carries
// the level's key where a consumer needs it: month (1..12) or week-of-month
// (1..6); unused for Year/Day (their labels derive from the name itself).
struct HistoryGroup {
  uint16_t first = 0;
  uint16_t count = 0;
  uint8_t value = 0;
};

// Max groups one level can produce: years inside a 128-day listing window
// (≤3), months per year (12), Monday-anchored week runs per month (6), days
// per week (7). 16 leaves headroom and keeps the VoiceActivity member tiny.
inline constexpr size_t MAX_HISTORY_GROUPS = 16;

// "YYYY-MM-DD" or "YYYY-MM-DD.jsonl" → calendar fields. False when the shape
// is wrong (non-digits, bad separators, month/day out of range).
bool parseDayDate(const char* name, int& year, int& month, int& day);

// Proleptic Gregorian date → days since 1970-01-01 (inverse of
// civilFromDays above; Howard Hinnant's days_from_civil).
int64_t daysFromCivil(int year, int month, int day);

// ISO weekday of a date: Monday = 1 … Sunday = 7 (1970-01-01 was Thursday).
int isoWeekdayMon1(int year, int month, int day);

// Monday-anchored week-of-month for a day of (year, month): week 1 starts at
// the Monday of the 1st's week (so it may begin in the previous month and the
// 1st lands in it whatever weekday it falls on), then 2, … Max 6 — a month
// has at most 6 such runs (e.g. September 2024: 1st is a Sunday).
int weekOfMonth(int year, int month, int day);

// Groups names[start, start+count) — sorted newest-first, a precondition
// listDays() guarantees — into runs for `level`. Year keys on the calendar
// year, Month on year*100+month, Week on year*10000+month*100+weekOfMonth
// (month in the key so a week run can never straddle a month), Day is one
// group per name. An unparsable name keys as a sentinel (group value 0), so
// it can never merge into a real calendar group. Returns groups written; 0
// when cap or count is 0 (a single call is bounded by MAX_HISTORY_GROUPS).
template <size_t N>
size_t groupDaysByName(const char (*names)[N], size_t start, size_t count, HistoryLevel level, HistoryGroup* out,
                       size_t cap);

// Label for one group: Year "2026", Month "2026-09",
// Week "09-22 ~ 09-28" (single-day week collapses to "09-24"), Day
// "2026-09-24". NUL-terminated, truncated to cap (cap >= 16 recommended).
template <size_t N>
void historyGroupLabel(const char (*names)[N], const HistoryGroup& group, HistoryLevel level, char* dst, size_t cap);

// Row label for the FLAT day browser: every day file in ONE reverse-
// chronological list, no Year/Month/Week drill-down (a mainstream chat
// history is a flat list with date labels; the hierarchy was deeper than
// any of them). Relative to `todayDays` (days-since-epoch of the user's
// local today, or < 0 while the clock is unsynced):
//   diff 0/1/2 -> todayStr / yesterdayStr / dayBeforeStr (caller-translated,
//                 keeping this layer i18n-free like buildSummaryPrompt)
//   any other day of today's year -> "MM-DD"
//   any other year, or unknown clock -> full "YYYY-MM-DD"
// An unparsable name is copied through as-is (returns false). NUL-
// terminated, truncated to cap (cap >= 16 recommended).
inline bool historyDayRowLabel(char* dst, size_t cap, const char* name, int64_t todayDays, const char* todayStr,
                               const char* yesterdayStr, const char* dayBeforeStr);

// ---- implementation -------------------------------------------------------

inline bool dayPath(char (&out)[DATE_PATH_SIZE], const int64_t epochMs, const int utcOffsetQuarterHoursBiased) {
  // Same sanity floor as the handshake: an ESP32 that never ran SNTP reports
  // 1970, which would file every conversation under 1970-01-20.
  if (epochMs < MIN_SANE_EPOCH_MS) return false;
  const int64_t localSec = localEpochSeconds(epochMs, utcOffsetQuarterHoursBiased);
  // Floor division: days before 1970 must round down.
  const int64_t days = localSec >= 0 ? localSec / 86400 : (localSec - 86399) / 86400;
  int year = 0;
  int month = 0;
  int day = 0;
  civilFromDays(days, year, month, day);
  snprintf(out, DATE_PATH_SIZE, "%04d-%02d-%02d.jsonl", year, month, day);
  return true;
}

// "HH:MM" (needs cap >= 6) — the wall clock of `epochMs` under the user's
// offset, i.e. what a clock next to the conversation would have shown. Pure so
// the history panel can timestamp stored entries; OnePage has no RTC to ask.
// Fails below the sane-epoch floor, like dayPath(): an unsynced clock has no
// meaningful wall time to show.
inline bool formatTimeForOffset(char* out, const size_t cap, const int64_t epochMs,
                                const int utcOffsetQuarterHoursBiased) {
  if (out == nullptr || cap < 6) return false;
  if (epochMs < MIN_SANE_EPOCH_MS) return false;
  const int64_t localSec = localEpochSeconds(epochMs, utcOffsetQuarterHoursBiased);
  const int64_t secsOfDay = ((localSec % 86400) + 86400) % 86400;
  snprintf(out, cap, "%02d:%02d", static_cast<int>(secsOfDay / 3600), static_cast<int>((secsOfDay / 60) % 60));
  return true;
}

// UTF-8 encode one code point (BMP + astral), returns byte count.
inline size_t utf8Encode(uint32_t cp, char* dst) {
  if (cp < 0x80) {
    dst[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    dst[0] = static_cast<char>(0xC0 | (cp >> 6));
    dst[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    dst[0] = static_cast<char>(0xE0 | (cp >> 12));
    dst[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  dst[0] = static_cast<char>(0xF0 | (cp >> 18));
  dst[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  dst[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  dst[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

inline size_t buildLine(char* out, const size_t cap, const uint64_t tsMs, const bool isUser, std::string_view text) {
  if (cap < 24) return 0;
  size_t n = 0;
  const auto put = [&](const char c) {
    if (n + 2 > cap) return false;  // keep room for the NUL
    out[n++] = c;
    out[n] = '\0';
    return true;
  };
  const auto putTwo = [&](const char a, const char b) { return put(a) && put(b); };
  // Prefix copy (raw loop: LINE_PREFIX is a std::string_view; any_of would
  // not let me abort with the buildLine error code mid-sequence).
  for (const char c : LINE_PREFIX) {
    if (!put(c)) return 0;
  }
  if (!putTwo(isUser ? 'u' : 'a', ' ')) return 0;
  char tsBuf[24];
  const int tsLen = snprintf(tsBuf, sizeof(tsBuf), "%llu", static_cast<unsigned long long>(tsMs));
  for (int i = 0; i < tsLen; i++) {
    if (!put(tsBuf[i])) return 0;
  }
  if (!putTwo(' ', '"')) return 0;
  // Emit one UTF-8 sequence (or its escape) per iteration. The reservation is
  // chunkLen + 3 (closing quote, newline, NUL), so truncation lands between
  // whole sequences — never mid-escape, never mid-CJK-character, which would
  // leave an invalid UTF-8 file behind.
  while (!text.empty()) {
    char chunk[8];
    size_t chunkLen = 0;
    size_t seqLen = 1;
    const unsigned char c = static_cast<unsigned char>(text[0]);
    if (c == '"') {
      chunk[chunkLen++] = '\\';
      chunk[chunkLen++] = '"';
    } else if (c == '\\') {
      chunk[chunkLen++] = '\\';
      chunk[chunkLen++] = '\\';
    } else if (c < 0x20) {
      // Not "HEX": Arduino's Print.h defines HEX = 16 as an enum constant,
      // and this header compiles inside that world too.
      static constexpr char kHexDigits[] = "0123456789abcdef";
      chunk[chunkLen++] = '\\';
      chunk[chunkLen++] = 'u';
      chunk[chunkLen++] = '0';
      chunk[chunkLen++] = '0';
      chunk[chunkLen++] = kHexDigits[(c >> 4) & 0xF];
      chunk[chunkLen++] = kHexDigits[c & 0xF];
    } else {
      // Whole sequence: lead byte plus its continuations, all of which must
      // exist in the input for the copy to be well-formed.
      seqLen = c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
      if (seqLen > text.size()) seqLen = text.size();
      chunkLen = seqLen;
      for (size_t k = 0; k < seqLen; k++) chunk[k] = text[k];
    }
    if (n + chunkLen + 3 > cap) break;  // no room for this sequence + closer
    for (size_t k = 0; k < chunkLen; k++) {
      out[n++] = chunk[k];
    }
    text.remove_prefix(seqLen);
  }
  out[n] = '\0';
  if (!putTwo('"', '\n')) return 0;
  return n;
}

inline bool parseLine(std::string_view line, Entry& entry, char* textDst, const size_t textCap, size_t& textLen,
                      bool* overflow) {
  textLen = 0;
  if (overflow != nullptr) *overflow = false;
  if (textDst == nullptr || textCap == 0) {
    // "Nowhere to write" is capacity, not corruption: the caller evicts and
    // retries rather than treating the record as malformed.
    if (overflow != nullptr) *overflow = true;
    return false;
  }
  textDst[0] = '\0';
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
    line.remove_suffix(1);
  }
  if (line.size() <= LINE_PREFIX.size() || line.substr(0, LINE_PREFIX.size()) != LINE_PREFIX) return false;
  line.remove_prefix(LINE_PREFIX.size());

  const size_t sp1 = line.find(' ');
  if (sp1 == std::string_view::npos || sp1 == 0) return false;
  const std::string_view role = line.substr(0, sp1);
  if (role == "u") {
    entry.isUser = true;
  } else if (role == "a") {
    entry.isUser = false;
  } else {
    return false;
  }
  line.remove_prefix(sp1 + 1);

  const size_t sp2 = line.find(' ');
  if (sp2 == std::string_view::npos || sp2 == 0) return false;
  uint64_t ts = 0;
  for (const char c : line.substr(0, sp2)) {
    if (c < '0' || c > '9') return false;
    ts = ts * 10 + static_cast<uint64_t>(c - '0');
  }
  entry.tsMs = ts;
  line.remove_prefix(sp2 + 1);

  // JSON string value. remove_prefix(1) above guarantees non-empty here, so
  // only the closing-quote check is needed on the back — the second empty()
  // check is for the reader, not the analyzer (it knows it is dead).
  if (line.empty() || line.front() != '"') return false;
  line.remove_prefix(1);
  if (line.empty()) return false;  // "" -> nothing between the quotes
  if (line.back() != '"') return false;
  line.remove_suffix(1);

  size_t o = 0;
  const auto emit = [&](const char c) {
    if (o + 2 > textCap) {  // keep room for the NUL
      if (overflow != nullptr) *overflow = true;
      return false;
    }
    textDst[o++] = c;
    textDst[o] = '\0';
    return true;
  };
  for (size_t i = 0; i < line.size();) {
    const char c = line[i];
    if (c != '\\') {
      if (!emit(c)) return false;
      i++;
      continue;
    }
    if (i + 1 >= line.size()) return false;
    const char e = line[i + 1];
    i += 2;
    switch (e) {
      case '"':
        if (!emit('"')) return false;
        break;
      case '\\':
        if (!emit('\\')) return false;
        break;
      case '/':
        if (!emit('/')) return false;
        break;
      case 'n':
        if (!emit('\n')) return false;
        break;
      case 'r':
        if (!emit('\r')) return false;
        break;
      case 't':
        if (!emit('\t')) return false;
        break;
      case 'b':
        if (!emit('\b')) return false;
        break;
      case 'f':
        if (!emit('\f')) return false;
        break;
      case 'u': {
        if (i + 4 > line.size()) return false;
        uint32_t cp = 0;
        for (int k = 0; k < 4; k++) {
          const char h = line[i + k];
          uint32_t v;
          if (h >= '0' && h <= '9') {
            v = static_cast<uint32_t>(h - '0');
          } else if (h >= 'a' && h <= 'f') {
            v = static_cast<uint32_t>(h - 'a' + 10);
          } else if (h >= 'A' && h <= 'F') {
            v = static_cast<uint32_t>(h - 'A' + 10);
          } else {
            return false;
          }
          cp = cp * 16 + v;
        }
        i += 4;
        // Surrogate pair -> astral code point; a lone surrogate decodes to
        // U+FFFD so the text never contains an invalid UTF-8 sequence.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= line.size() && line[i] == '\\' && line[i + 1] == 'u') {
          uint32_t lo = 0;
          bool loOk = true;
          for (int k = 0; k < 4; k++) {
            const char h = line[i + 2 + k];
            uint32_t v;
            if (h >= '0' && h <= '9') {
              v = static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              v = static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              v = static_cast<uint32_t>(h - 'A' + 10);
            } else {
              loOk = false;
              break;
            }
            lo = lo * 16 + v;
          }
          if (loOk && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        char u8[4];
        const size_t un = utf8Encode(cp, u8);
        for (size_t k = 0; k < un; k++) {
          if (!emit(u8[k])) return false;
        }
        break;
      }
      default:
        return false;
    }
  }
  textLen = o;
  return true;
}

// Builds a "summarize this day" chat prompt into dst: one instruction line
// (already translated and dated by the caller), then each entry as
// "<tag> <text>". Selection runs newest-first — a day that does not fit keeps
// its most recent turns — while emission stays chronological. The oldest
// included entry may be cut at a UTF-8 boundary so a mid-codepoint slice can
// never reach the model or the panel. Returns false when the instruction alone
// does not fit or not one entry has room (nothing to summarize).
//
// Pure C++ (host-tested). The caller bounds cap by the chat message cap; this
// layer does not know it (OpenClawChat.h owns that constant).
inline bool buildSummaryPrompt(char* dst, const size_t cap, const Entry* entries, const size_t count, const char* pool,
                               const char* instruction, const char* youTag, const char* assistantTag) {
  // No minimum cap beyond what snprintf + the head check below enforce: a
  // buffer too small for the instruction is rejected there, with the NUL
  // reserved by the avail arithmetic.
  if (dst == nullptr || entries == nullptr || pool == nullptr || instruction == nullptr || instruction[0] == '\0' ||
      youTag == nullptr || assistantTag == nullptr || count == 0) {
    return false;
  }
  const int head = snprintf(dst, cap, "%s\n", instruction);
  if (head <= 0 || static_cast<size_t>(head) >= cap) return false;
  const size_t used = static_cast<size_t>(head);

  // Pass 1: how many of the newest entries fit, and how much of the oldest.
  // One byte of cap stays reserved for the terminating NUL.
  size_t avail = cap - used - 1;
  size_t firstIdx = count;  // oldest entry included
  size_t partialLen = 0;    // its text length when cut short (0 = intact)
  bool got = false;
  for (size_t i = count; i-- > 0;) {
    const Entry& e = entries[i];
    const size_t tagLen = strlen(e.isUser ? youTag : assistantTag);
    const char* text = pool + e.textOff;
    const size_t tlen = e.textLen;
    const size_t need = tagLen + 1 + tlen + 1;  // "<tag> <text>\n"
    if (need <= avail) {
      avail -= need;
      firstIdx = i;
      got = true;
      continue;
    }
    // No room for this entry whole: take a UTF-8-safe prefix if tag, one byte
    // of text and the newline still fit, then stop — older turns stay out.
    const size_t overhead = tagLen + 2;
    if (avail > overhead) {
      size_t take = avail - overhead;
      if (take > tlen) take = tlen;
      // text[take] is a continuation byte only when the cut landed inside a
      // character; back off to the boundary before it.
      while (take > 0 && (static_cast<unsigned char>(text[take]) & 0xC0) == 0x80) take--;
      if (take > 0) {
        firstIdx = i;
        partialLen = take;
        got = true;
      }
    }
    break;
  }
  if (!got) return false;

  // Pass 2: emit oldest -> newest with the exact sizes pass 1 settled on.
  size_t pos = used;
  for (size_t k = firstIdx; k < count; k++) {
    const Entry& e = entries[k];
    const size_t tlen = (k == firstIdx && partialLen > 0) ? partialLen : e.textLen;
    const int n = snprintf(dst + pos, cap - pos, "%s %.*s\n", e.isUser ? youTag : assistantTag, static_cast<int>(tlen),
                           pool + e.textOff);
    if (n <= 0 || static_cast<size_t>(n) >= cap - pos) break;  // defensive; sizes pre-checked
    pos += static_cast<size_t>(n);
  }
  dst[pos] = '\0';
  return true;
}

// ── history hierarchy implementation ─────────────────────────────────────────

inline bool parseDayDate(const char* name, int& year, int& month, int& day) {
  if (name == nullptr) return false;
  const auto digit = [](const char c) { return c >= '0' && c <= '9'; };
  if (!(digit(name[0]) && digit(name[1]) && digit(name[2]) && digit(name[3]) && name[4] == '-' && digit(name[5]) &&
        digit(name[6]) && name[7] == '-' && digit(name[8]) && digit(name[9]))) {
    return false;
  }
  // Bare "YYYY-MM-DD" or exactly "YYYY-MM-DD.jsonl" (the directory listing
  // suffix); anything else after the date is a foreign file, not a day record.
  if (name[10] != '\0') {
    static constexpr char JSONL[] = ".jsonl";
    for (size_t i = 0; i < sizeof(JSONL); i++) {  // includes the NUL
      if (name[10 + i] != JSONL[i]) return false;
    }
  }
  year = (name[0] - '0') * 1000 + (name[1] - '0') * 100 + (name[2] - '0') * 10 + (name[3] - '0');
  month = (name[5] - '0') * 10 + (name[6] - '0');
  day = (name[8] - '0') * 10 + (name[9] - '0');
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

inline int64_t daysFromCivil(const int year, const int month, const int day) {
  const int y = year - (month <= 2 ? 1 : 0);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = static_cast<int64_t>(y) - era * 400;                       // [0, 399]
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;  // [0, 365]
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                     // [0, 146096]
  return era * 146097 + doe - 719468;
}

inline int isoWeekdayMon1(const int year, const int month, const int day) {
  const int64_t days = daysFromCivil(year, month, day);
  // 1970-01-01 (days 0) was a Thursday. r: 0 = Thursday … 6 = Wednesday;
  // (r + 3) % 7 maps that onto Monday = 1 … Sunday = 7.
  const int r = static_cast<int>(((days % 7) + 7) % 7);
  return ((r + 3) % 7) + 1;
}

inline int weekOfMonth(const int year, const int month, const int day) {
  // Anchor both the month's 1st and the queried day to their Mondays; the
  // difference is whole weeks regardless of where inside the weeks they sit.
  const int64_t firstMonday = daysFromCivil(year, month, 1) - (isoWeekdayMon1(year, month, 1) - 1);
  const int64_t hereMonday = daysFromCivil(year, month, day) - (isoWeekdayMon1(year, month, day) - 1);
  return static_cast<int>((hereMonday - firstMonday) / 7) + 1;
}

template <size_t N>
inline size_t groupDaysByName(const char (*names)[N], const size_t start, const size_t count, const HistoryLevel level,
                              HistoryGroup* out, const size_t cap) {
  if (names == nullptr || out == nullptr || cap == 0 || count == 0) return 0;
  // Key for one day at this level. Two days are in the same group exactly
  // while their keys match; Day uses the array index so every name splits.
  const auto keyOf = [&](const size_t idx) -> uint32_t {
    int y = 0;
    int m = 0;
    int d = 0;
    if (!parseDayDate(names[idx], y, m, d)) return 0xFFFFFFFFu;
    switch (level) {
      case HistoryLevel::Year:
        return static_cast<uint32_t>(y);
      case HistoryLevel::Month:
        return static_cast<uint32_t>(y * 100 + m);
      case HistoryLevel::Week:
        // Month in the key as well: a week group can never straddle a month
        // boundary even if a caller forgets to narrow the range first.
        return static_cast<uint32_t>(y * 10000 + m * 100 + weekOfMonth(y, m, d));
      case HistoryLevel::Day:
      default:
        return static_cast<uint32_t>(idx);
    }
  };
  size_t written = 0;
  size_t i = 0;
  while (i < count && written < cap) {
    const size_t idx = start + i;
    const uint32_t key = keyOf(idx);
    size_t run = 1;
    while (i + run < count && keyOf(start + i + run) == key) run++;
    HistoryGroup& g = out[written++];
    g.first = static_cast<uint16_t>(idx);
    g.count = static_cast<uint16_t>(run);
    g.value = 0;  // stays 0 when the run's first name does not parse
    int y = 0;
    int m = 0;
    int d = 0;
    if (parseDayDate(names[idx], y, m, d)) {
      switch (level) {
        case HistoryLevel::Month:
          g.value = static_cast<uint8_t>(m);
          break;
        case HistoryLevel::Week:
          g.value = static_cast<uint8_t>(weekOfMonth(y, m, d));
          break;
        default:
          g.value = 0;
          break;
      }
    }
    i += run;
  }
  return written;
}

template <size_t N>
inline void historyGroupLabel(const char (*names)[N], const HistoryGroup& group, const HistoryLevel level, char* dst,
                              const size_t cap) {
  if (dst == nullptr || cap == 0) return;
  dst[0] = '\0';
  if (names == nullptr || group.count == 0) return;
  const char* newest = names[group.first];                    // end of the range
  const char* oldest = names[group.first + group.count - 1];  // start of the range
  switch (level) {
    case HistoryLevel::Year:
      snprintf(dst, cap, "%.4s", newest);  // "2026"
      break;
    case HistoryLevel::Month:
      snprintf(dst, cap, "%.7s", newest);  // "2026-09"
      break;
    case HistoryLevel::Week: {
      // "MM-DD ~ MM-DD" from inside the "YYYY-MM-DD" names; a one-day week
      // shows just the date.
      const char* end = newest + 5;
      const char* begin = oldest + 5;
      if (group.count == 1 || (newest[5] == begin[0] && newest[6] == begin[1] && newest[7] == begin[2] &&
                               newest[8] == begin[3] && newest[9] == begin[4])) {
        snprintf(dst, cap, "%.5s", end);
      } else {
        snprintf(dst, cap, "%.5s ~ %.5s", begin, end);
      }
      break;
    }
    case HistoryLevel::Day:
    default:
      snprintf(dst, cap, "%.10s", newest);  // "2026-09-24"
      break;
  }
}

inline bool historyDayRowLabel(char* dst, const size_t cap, const char* name, const int64_t todayDays,
                               const char* todayStr, const char* yesterdayStr, const char* dayBeforeStr) {
  if (dst == nullptr || cap == 0) return false;
  dst[0] = '\0';
  if (name == nullptr) return false;
  int y = 0;
  int m = 0;
  int d = 0;
  if (!parseDayDate(name, y, m, d)) {
    snprintf(dst, cap, "%s", name);  // foreign file: show it verbatim
    return false;
  }
  if (todayDays >= 0) {
    const int64_t diff = todayDays - daysFromCivil(y, m, d);
    const char* rel = diff == 0 ? todayStr : diff == 1 ? yesterdayStr : diff == 2 ? dayBeforeStr : nullptr;
    if (rel != nullptr && rel[0] != '\0') {
      snprintf(dst, cap, "%s", rel);
      return true;
    }
    int todayYear = 0;
    int tm = 0;
    int td = 0;
    civilFromDays(todayDays, todayYear, tm, td);
    if (y == todayYear) {  // (also catches a clock-skewed "future" day)
      snprintf(dst, cap, "%02d-%02d", m, d);
      return true;
    }
  }
  snprintf(dst, cap, "%.10s", name);  // "YYYY-MM-DD" (name may carry ".jsonl")
  return true;
}

}  // namespace ChatHistoryFormat
}  // namespace OpenClaw
