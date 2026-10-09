#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ChatHistoryFormat.h"

using OpenClaw::ChatHistoryFormat::buildLine;
using OpenClaw::ChatHistoryFormat::DATE_PATH_SIZE;
using OpenClaw::ChatHistoryFormat::dayPath;
using OpenClaw::ChatHistoryFormat::daysFromCivil;
using OpenClaw::ChatHistoryFormat::formatStampForOffset;
using OpenClaw::ChatHistoryFormat::groupDaysByName;
using OpenClaw::ChatHistoryFormat::historyDayRowLabel;
using OpenClaw::ChatHistoryFormat::HistoryGroup;
using OpenClaw::ChatHistoryFormat::historyGroupLabel;
using OpenClaw::ChatHistoryFormat::HistoryLevel;
using OpenClaw::ChatHistoryFormat::isoWeekdayMon1;
using OpenClaw::ChatHistoryFormat::parseDayDate;
using OpenClaw::ChatHistoryFormat::parseLine;
using OpenClaw::ChatHistoryFormat::STAMP_SIZE;
using OpenClaw::ChatHistoryFormat::weekOfMonth;

namespace {

// 2026-09-27 00:00:00 UTC = 20723 epoch days × 86400 (verified against the
// civil-from-days algorithm under test and an independent table).
constexpr int64_t MIDNIGHT_UTC_2026_09_27 = 1790467200000LL;
constexpr int64_t NOON_UTC_2026_09_27 = MIDNIGHT_UTC_2026_09_27 + 12 * 3600 * 1000LL;

std::string roundTrip(const std::string& text, size_t cap = 512) {
  std::vector<char> buf(cap, '\0');
  const size_t n = buildLine(buf.data(), cap, 1234, true, text);
  if (n == 0) return "<0>";
  OpenClaw::ChatHistoryFormat::Entry e{};
  char dst[512];
  size_t len = 0;
  if (!parseLine(std::string_view(buf.data(), n), e, dst, sizeof(dst), len)) return "<unparseable>";
  EXPECT_TRUE(e.isUser);
  EXPECT_EQ(e.tsMs, 1234u);
  return std::string(dst, len);
}

}  // namespace

TEST(ChatHistoryDayPath, UtcMidnightAndNoon) {
  char path[DATE_PATH_SIZE];
  ASSERT_TRUE(dayPath(path, MIDNIGHT_UTC_2026_09_27, 48));
  EXPECT_STREQ(path, "2026-09-27.jsonl");
  ASSERT_TRUE(dayPath(path, NOON_UTC_2026_09_27, 48));
  EXPECT_STREQ(path, "2026-09-27.jsonl");
}

TEST(ChatHistoryDayPath, PositiveOffsetCrossesDayBoundary) {
  char path[DATE_PATH_SIZE];
  // 2026-09-27 23:30 UTC = 2026-09-28 07:30 in UTC+8
  ASSERT_TRUE(dayPath(path, MIDNIGHT_UTC_2026_09_27 + 23 * 3600 * 1000LL + 30 * 60 * 1000LL, 48 + 32));
  EXPECT_STREQ(path, "2026-09-28.jsonl");
}

TEST(ChatHistoryDayPath, NegativeOffsetCrossesBack) {
  char path[DATE_PATH_SIZE];
  // 2026-09-27 02:00 UTC = 2026-09-26 18:00 in UTC-8
  ASSERT_TRUE(dayPath(path, MIDNIGHT_UTC_2026_09_27 + 2 * 3600 * 1000LL, 48 - 32));
  EXPECT_STREQ(path, "2026-09-26.jsonl");
}

TEST(ChatHistoryDayPath, UnsancEpochRejected) {
  char path[DATE_PATH_SIZE];
  // 1970-01-20 — what an SNTP-less boot reports.
  EXPECT_FALSE(dayPath(path, 1600000, 48));
}

TEST(ChatHistoryStamp, UtcNoonMatchesDayPathDate) {
  char stamp[STAMP_SIZE];
  // The date half must agree with the day file dayPath() picks for the same
  // instant, or a row could show 2026-09-27 while living in 2026-09-28.jsonl.
  ASSERT_TRUE(formatStampForOffset(stamp, sizeof(stamp), NOON_UTC_2026_09_27, 48));
  EXPECT_STREQ(stamp, "2026-09-27/12:00");
}

TEST(ChatHistoryStamp, PositiveOffsetShiftsBothDateAndTime) {
  char stamp[STAMP_SIZE];
  // 2026-09-27 23:30 UTC = 2026-09-28 07:30 in UTC+8
  ASSERT_TRUE(formatStampForOffset(stamp, sizeof(stamp),
                                   MIDNIGHT_UTC_2026_09_27 + 23 * 3600 * 1000LL + 30 * 60 * 1000LL, 48 + 32));
  EXPECT_STREQ(stamp, "2026-09-28/07:30");
}

TEST(ChatHistoryStamp, NegativeOffsetCrossesBack) {
  char stamp[STAMP_SIZE];
  // 2026-09-27 02:00 UTC = 2026-09-26 18:00 in UTC-8
  ASSERT_TRUE(formatStampForOffset(stamp, sizeof(stamp), MIDNIGHT_UTC_2026_09_27 + 2 * 3600 * 1000LL, 48 - 32));
  EXPECT_STREQ(stamp, "2026-09-26/18:00");
}

TEST(ChatHistoryStamp, UnsancEpochRejected) {
  char stamp[STAMP_SIZE];
  // No timestamp beats a confidently wrong one: the caller then renders the
  // row without a prefix instead of stamping 1970-01-20.
  EXPECT_FALSE(formatStampForOffset(stamp, sizeof(stamp), 1600000, 48));
}

TEST(ChatHistoryStamp, TinyCapRejected) {
  char small[STAMP_SIZE - 1] = {};
  char nullOut[STAMP_SIZE] = {};
  EXPECT_FALSE(formatStampForOffset(small, sizeof(small), NOON_UTC_2026_09_27, 48));
  EXPECT_FALSE(formatStampForOffset(nullptr, sizeof(nullOut), NOON_UTC_2026_09_27, 48));
}

TEST(ChatHistoryLine, AsciiRoundTrip) { EXPECT_EQ(roundTrip("Hello, world!"), "Hello, world!"); }

TEST(ChatHistoryLine, Utf8PassesThrough) {
  // "你好" and an emoji — raw bytes must survive untouched.
  const std::string text = std::string("你好 \xF0\x9F\x98\x84 kitchen");
  EXPECT_EQ(roundTrip(text), text);
}

TEST(StrippableSymbolCodepoint, CoversEmojiRanges) {
  // Pin the ranges ChatReply::stripStrippableSymbols relies on. U+23F0 was
  // missing until 2026-09-29 — ⏰ reached the voice panel as a box.
  EXPECT_TRUE(OpenClaw::ChatHistoryFormat::isStrippableSymbolCodepoint(0x23F0));   // ⏰
  EXPECT_TRUE(OpenClaw::ChatHistoryFormat::isStrippableSymbolCodepoint(0x23F8));   // ⏸
  EXPECT_TRUE(OpenClaw::ChatHistoryFormat::isStrippableSymbolCodepoint(0x1F634));  // 😴
  EXPECT_TRUE(OpenClaw::ChatHistoryFormat::isStrippableSymbolCodepoint(0xFE0F));   // VS16
  EXPECT_TRUE(OpenClaw::ChatHistoryFormat::isStrippableSymbolCodepoint(0x200D));   // ZWJ
}

TEST(StrippableSymbolCodepoint, KeepsOrdinaryText) {
  EXPECT_FALSE(OpenClaw::ChatHistoryFormat::isStrippableSymbolCodepoint(0x4E2D));  // 中
  EXPECT_FALSE(OpenClaw::ChatHistoryFormat::isStrippableSymbolCodepoint(0x62));    // 'b'
  EXPECT_FALSE(OpenClaw::ChatHistoryFormat::isStrippableSymbolCodepoint(0x20));    // ' '
  EXPECT_FALSE(OpenClaw::ChatHistoryFormat::isStrippableSymbolCodepoint(0x3001));  // 、
}

TEST(StripStrippableSymbols, RemovesEmojiInPlace) {
  // The 2026-09-29 device screenshot: ⏰ (3 bytes) between CJK runs.
  const std::string src = "搞定！⏰ 明早 8:00";
  std::vector<char> buf(src.begin(), src.end());
  buf.push_back('\0');
  EXPECT_EQ(OpenClaw::ChatHistoryFormat::stripStrippableSymbols(buf.data(), src.size()), 3u);
  EXPECT_STREQ(buf.data(), "搞定！ 明早 8:00");
}

TEST(StripStrippableSymbols, KeepsBuiltLineShape) {
  // A full JSONL record: the ASCII prefix, the quotes and the trailing
  // newline must survive so the file stays parseable (this is what append()
  // runs over lineBuf before writing).
  const std::string src = "OCL1 a 1790467200123 \"hi⏰\"\n";
  std::vector<char> buf(src.begin(), src.end());
  buf.push_back('\0');
  EXPECT_EQ(OpenClaw::ChatHistoryFormat::stripStrippableSymbols(buf.data(), src.size()), 3u);
  EXPECT_STREQ(buf.data(), "OCL1 a 1790467200123 \"hi\"\n");
}

TEST(StripStrippableSymbols, NothingFlaggedLeavesBufferAlone) {
  std::vector<char> buf{'a', 'b', '\0'};
  EXPECT_EQ(OpenClaw::ChatHistoryFormat::stripStrippableSymbols(buf.data(), 2), 0u);
  EXPECT_STREQ(buf.data(), "ab");
  EXPECT_EQ(OpenClaw::ChatHistoryFormat::stripStrippableSymbols(nullptr, 4), 0u);
  EXPECT_EQ(OpenClaw::ChatHistoryFormat::stripStrippableSymbols(buf.data(), 0), 0u);
}

TEST(ChatHistoryLine, QuotesAndBackslashEscaped) { EXPECT_EQ(roundTrip("say \"hi\" \\ ok"), "say \"hi\" \\ ok"); }

TEST(ChatHistoryLine, ControlBytesNeverBreakLines) {
  // A newline inside the text would corrupt the one-record-per-line file if
  // it were emitted raw. Encoded as \u000a the raw line holds exactly one
  // '\n' — the terminator — and the round trip still recovers the text.
  std::vector<char> buf(256, '\0');
  const size_t n = buildLine(buf.data(), buf.size(), 1, true, std::string_view("a\nb"));
  ASSERT_GT(n, 0u);
  const std::string_view raw(buf.data(), n);
  ASSERT_EQ(raw.back(), '\n');
  EXPECT_EQ(raw.find('\n'), raw.size() - 1) << "only the terminator newline may appear";
  EXPECT_EQ(roundTrip(std::string("a\nb")), "a\nb");
  EXPECT_EQ(roundTrip(std::string("a\tb")), "a\tb");
}

TEST(ChatHistoryLine, AssistantRole) {
  std::vector<char> buf(256, '\0');
  const size_t n = buildLine(buf.data(), 256, 99, false, "hi");
  ASSERT_GT(n, 0u);
  EXPECT_EQ(std::string(buf.data()).find("OCL1 a 99 \"hi\""), 0u);
}

TEST(ChatHistoryLine, TruncationKeepsFileParseable) {
  // Build with a cap that forces mid-text truncation; the result must still
  // parse, and must not split a UTF-8 sequence.
  const std::string text = std::string("厨房怎么说 你好世界 \xF0\x9F\x98\x84");
  std::vector<char> buf(48, '\0');
  const size_t n = buildLine(buf.data(), buf.size(), 7, true, text);
  ASSERT_GT(n, 0u);
  OpenClaw::ChatHistoryFormat::Entry e{};
  char dst[256];
  size_t len = 0;
  ASSERT_TRUE(parseLine(std::string_view(buf.data(), n), e, dst, sizeof(dst), len));
  EXPECT_TRUE(e.isUser);
  EXPECT_EQ(e.tsMs, 7u);
  // Every decoded byte sequence must be valid UTF-8 (no torn CJK).
  size_t i = 0;
  while (i < len) {
    const unsigned char c = static_cast<unsigned char>(dst[i]);
    const size_t need = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
    ASSERT_GE(len - i, need) << "torn UTF-8 sequence at byte " << i;
    for (size_t k = 1; k < need; k++) {
      EXPECT_EQ((static_cast<unsigned char>(dst[i + k]) >> 6), 0x2);
    }
    i += need;
  }
  // Truncation dropped a suffix (or hit the cap exactly); never invented data.
  EXPECT_LE(std::string(dst, len), text);
}

TEST(ChatHistoryLine, TinyCapReturnsZero) {
  char buf[8];
  EXPECT_EQ(buildLine(buf, sizeof(buf), 1, true, "x"), 0u);
}

TEST(ChatHistoryParse, RejectsGarbage) {
  OpenClaw::ChatHistoryFormat::Entry e{};
  char dst[64];
  size_t len = 0;
  EXPECT_FALSE(parseLine("", e, dst, sizeof(dst), len));
  EXPECT_FALSE(parseLine("not a history line", e, dst, sizeof(dst), len));
  EXPECT_FALSE(parseLine("OCL1 x 123 \"t\"", e, dst, sizeof(dst), len));
  EXPECT_FALSE(parseLine("OCL1 u abc \"t\"", e, dst, sizeof(dst), len));
  EXPECT_FALSE(parseLine("OCL1 u 123 t", e, dst, sizeof(dst), len));
  EXPECT_FALSE(parseLine("OCL1 u 123 \"unterminated", e, dst, sizeof(dst), len));
  // Destination too small for the text: fail, don't truncate silently.
  char tiny[4];
  EXPECT_FALSE(parseLine("OCL1 u 123 \"abcd\"", e, tiny, sizeof(tiny), len));
}

TEST(ChatHistoryParse, UnknownEscapesRejected) {
  OpenClaw::ChatHistoryFormat::Entry e{};
  char dst[64];
  size_t len = 0;
  EXPECT_FALSE(parseLine("OCL1 u 123 \"bad \\q escape\"", e, dst, sizeof(dst), len));
  EXPECT_FALSE(parseLine("OCL1 u 123 \"cut \\u12\"", e, dst, sizeof(dst), len));
}

TEST(ChatHistoryParse, SurrogatePairDecodes) {
  OpenClaw::ChatHistoryFormat::Entry e{};
  char dst[64];
  size_t len = 0;
  // U+1F604 = surrogate pair D83D DE04 -> F0 9F 98 84.
  ASSERT_TRUE(parseLine("OCL1 a 5 \"\\uD83D\\uDE04\"", e, dst, sizeof(dst), len));
  EXPECT_EQ(std::string(dst, len), std::string("\xF0\x9F\x98\x84"));
}

// ── buildSummaryPrompt ─────────────────────────────────────────────────────

namespace {

// Front-packs texts into a pool and fills entries the way ChatHistory::loadDay
// does (contiguous textOff, NUL after each text).
struct SummaryFixture {
  std::vector<char> pool;
  std::vector<OpenClaw::ChatHistoryFormat::Entry> entries;

  explicit SummaryFixture(const std::vector<std::pair<bool, std::string>>& rows) {
    for (const auto& row : rows) {
      OpenClaw::ChatHistoryFormat::Entry e{};
      e.textOff = static_cast<uint32_t>(pool.size());
      e.textLen = static_cast<uint16_t>(row.second.size());
      e.isUser = row.first;
      pool.insert(pool.end(), row.second.begin(), row.second.end());
      pool.push_back('\0');
      entries.push_back(e);
    }
  }
};

}  // namespace

TEST(SummaryPrompt, KeepsEveryEntryWhenItFits) {
  const SummaryFixture fx({{true, "你好"}, {false, "回答"}});
  char dst[128];
  ASSERT_TRUE(OpenClaw::ChatHistoryFormat::buildSummaryPrompt(dst, sizeof(dst), fx.entries.data(), fx.entries.size(),
                                                              fx.pool.data(), "Sum", "U:", "A:"));
  EXPECT_EQ(std::string(dst), "Sum\nU: 你好\nA: 回答\n");
}

TEST(SummaryPrompt, OverflowDropsOldestKeepsNewestInOrder) {
  // head "IN\n" = 3, cap 20 -> avail 16 = exactly two "X: 0000\n" lines (8).
  const SummaryFixture fx({{true, "0000"}, {false, "1111"}, {true, "2222"}});
  char dst[20];
  ASSERT_TRUE(OpenClaw::ChatHistoryFormat::buildSummaryPrompt(dst, sizeof(dst), fx.entries.data(), fx.entries.size(),
                                                              fx.pool.data(), "IN", "U:", "A:"));
  EXPECT_EQ(std::string(dst), "IN\nA: 1111\nU: 2222\n");
  EXPECT_EQ(strlen(dst), sizeof(dst) - 1);  // fills the buffer minus its NUL
}

TEST(SummaryPrompt, OldestEntryCutAtUtf8Boundary) {
  // avail = 8, tag overhead 4 -> take 4 bytes of "你"*10, trimmed back from a
  // mid-character cut to the one whole 你 (3 bytes) that fits.
  const SummaryFixture fx({{true, "你你你你你你你你你你"}});
  char dst[11];
  ASSERT_TRUE(OpenClaw::ChatHistoryFormat::buildSummaryPrompt(dst, sizeof(dst), fx.entries.data(), fx.entries.size(),
                                                              fx.pool.data(), "I", "U:", "A:"));
  EXPECT_EQ(std::string(dst), "I\nU: 你\n");
  // Whole prompt is valid UTF-8 (nothing torn).
  size_t i = 0;
  const size_t len = strlen(dst);
  while (i < len) {
    const unsigned char c = static_cast<unsigned char>(dst[i]);
    const size_t need = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
    ASSERT_GE(len - i, need) << "torn UTF-8 at byte " << i;
    i += need;
  }
}

TEST(SummaryPrompt, RejectsEmptyAndTooSmallInputs) {
  const SummaryFixture fx({{true, "x"}});
  char dst[64];
  EXPECT_FALSE(OpenClaw::ChatHistoryFormat::buildSummaryPrompt(dst, sizeof(dst), fx.entries.data(), 0, fx.pool.data(),
                                                               "I", "U:", "A:"));
  EXPECT_FALSE(OpenClaw::ChatHistoryFormat::buildSummaryPrompt(dst, sizeof(dst), fx.entries.data(), fx.entries.size(),
                                                               fx.pool.data(), "", "U:", "A:"));
  char tiny[8];
  EXPECT_FALSE(OpenClaw::ChatHistoryFormat::buildSummaryPrompt(tiny, sizeof(tiny), fx.entries.data(), fx.entries.size(),
                                                               fx.pool.data(), "a much longer instruction",
                                                               "U:", "A:"));
  EXPECT_FALSE(OpenClaw::ChatHistoryFormat::buildSummaryPrompt(nullptr, 64, fx.entries.data(), fx.entries.size(),
                                                               fx.pool.data(), "I", "U:", "A:"));
}

// ── history hierarchy (year / month / week / day) ────────────────────────────

TEST(HistoryParseDayDate, AcceptsDayNames) {
  int y = 0;
  int m = 0;
  int d = 0;
  ASSERT_TRUE(parseDayDate("2026-09-24.jsonl", y, m, d));
  EXPECT_EQ(y, 2026);
  EXPECT_EQ(m, 9);
  EXPECT_EQ(d, 24);
  ASSERT_TRUE(parseDayDate("2024-02-29", y, m, d));  // leap day accepted
  EXPECT_EQ(y, 2024);
  EXPECT_EQ(m, 2);
  EXPECT_EQ(d, 29);
}

TEST(HistoryParseDayDate, RejectsForeignNames) {
  int y = 0;
  int m = 0;
  int d = 0;
  EXPECT_FALSE(parseDayDate("2026-09-24.txt", y, m, d));  // only ".jsonl" suffix
  EXPECT_FALSE(parseDayDate("2026-09-24.j", y, m, d));
  EXPECT_FALSE(parseDayDate("2026-13-01", y, m, d));
  EXPECT_FALSE(parseDayDate("2026-00-10", y, m, d));
  EXPECT_FALSE(parseDayDate("2026-09-00", y, m, d));
  EXPECT_FALSE(parseDayDate("2026-09-32", y, m, d));
  EXPECT_FALSE(parseDayDate("20260924", y, m, d));
  EXPECT_FALSE(parseDayDate("2026-0924", y, m, d));
  EXPECT_FALSE(parseDayDate("", y, m, d));
  EXPECT_FALSE(parseDayDate("junk", y, m, d));
}

TEST(HistoryCalendar, DaysFromCivilEpochAndKnownDates) {
  EXPECT_EQ(daysFromCivil(1970, 1, 1), 0);
  EXPECT_EQ(daysFromCivil(1970, 1, 2), 1);
  EXPECT_EQ(daysFromCivil(2000, 3, 1), 11017);   // 2000-01-01 = 10957, leap Feb
  EXPECT_EQ(daysFromCivil(2024, 2, 29), 19782);  // 2024-01-01 = 19723
  // Same anchor the dayPath tests use: MIDNIGHT_UTC_2026_09_27 = 20723 days.
  EXPECT_EQ(daysFromCivil(2026, 9, 27), MIDNIGHT_UTC_2026_09_27 / 86400000LL);
}

TEST(HistoryCalendar, IsoWeekdayMondayFirst) {
  EXPECT_EQ(isoWeekdayMon1(1970, 1, 1), 4);   // Thursday (the epoch anchor)
  EXPECT_EQ(isoWeekdayMon1(1970, 1, 4), 7);   // Sunday
  EXPECT_EQ(isoWeekdayMon1(1970, 1, 5), 1);   // Monday
  EXPECT_EQ(isoWeekdayMon1(2000, 1, 1), 6);   // Saturday
  EXPECT_EQ(isoWeekdayMon1(2026, 9, 30), 3);  // Wednesday
}

TEST(HistoryCalendar, WeekOfMonthMondayAnchored) {
  // September 2024: the 1st is a Sunday, so week 1 is the single-day tail of
  // the 1st's week, the 2nd starts run 2, and the month has the max 6 runs.
  EXPECT_EQ(weekOfMonth(2024, 9, 1), 1);
  EXPECT_EQ(weekOfMonth(2024, 9, 2), 2);
  EXPECT_EQ(weekOfMonth(2024, 9, 8), 2);  // Sunday closes run 2
  EXPECT_EQ(weekOfMonth(2024, 9, 9), 3);  // Monday opens run 3
  EXPECT_EQ(weekOfMonth(2024, 9, 29), 5);
  EXPECT_EQ(weekOfMonth(2024, 9, 30), 6);  // lone Monday tail
  // December 2025: the 1st is a Monday, week 1 starts on it.
  EXPECT_EQ(weekOfMonth(2025, 12, 1), 1);
  EXPECT_EQ(weekOfMonth(2025, 12, 7), 1);  // Sunday still inside run 1
  EXPECT_EQ(weekOfMonth(2025, 12, 8), 2);
  EXPECT_EQ(weekOfMonth(2025, 12, 31), 5);
  // August 2024: the 1st is a Thursday, so run 1 began Mon Jul 29 — the week
  // may start in the previous month, but every day of THIS month still maps.
  EXPECT_EQ(weekOfMonth(2024, 8, 1), 1);
  EXPECT_EQ(weekOfMonth(2024, 8, 4), 1);  // Sunday of run 1
  EXPECT_EQ(weekOfMonth(2024, 8, 5), 2);
}

TEST(HistoryGroup, SplitsYearsAndKeepsRuns) {
  const char names[][11] = {"2026-01-02", "2026-01-01", "2025-12-31"};  // newest first
  HistoryGroup g[4]{};
  ASSERT_EQ(groupDaysByName(names, 0, 3, HistoryLevel::Year, g, 4), 2u);
  EXPECT_EQ(g[0].first, 0);
  EXPECT_EQ(g[0].count, 2);
  EXPECT_EQ(g[1].first, 2);
  EXPECT_EQ(g[1].count, 1);
  ASSERT_EQ(groupDaysByName(names, 0, 3, HistoryLevel::Month, g, 4), 2u);
  EXPECT_EQ(g[0].count, 2);
  EXPECT_EQ(g[0].value, 1);  // January
  EXPECT_EQ(g[1].count, 1);
  EXPECT_EQ(g[1].value, 12);  // December
  ASSERT_EQ(groupDaysByName(names, 0, 3, HistoryLevel::Day, g, 4), 3u);
  EXPECT_EQ(g[2].first, 2);
  EXPECT_EQ(g[2].count, 1);
}

TEST(HistoryGroup, SplitsWeeksInsideOneMonth) {
  // Newest first; weeks 6, 3, 2, 2, 1 of September 2024 (the listed days are
  // not contiguous in the calendar — runs form on equal keys, not adjacency).
  const char names[][11] = {"2024-09-30", "2024-09-09", "2024-09-08", "2024-09-02", "2024-09-01"};
  HistoryGroup g[8]{};
  const size_t n = groupDaysByName(names, 0, 5, HistoryLevel::Week, g, 8);
  ASSERT_EQ(n, 4u);
  EXPECT_EQ(g[0].first, 0);
  EXPECT_EQ(g[0].count, 1);
  EXPECT_EQ(g[0].value, 6);
  EXPECT_EQ(g[1].first, 1);
  EXPECT_EQ(g[1].count, 1);
  EXPECT_EQ(g[1].value, 3);
  EXPECT_EQ(g[2].first, 2);
  EXPECT_EQ(g[2].count, 2);
  EXPECT_EQ(g[2].value, 2);
  EXPECT_EQ(g[3].first, 4);
  EXPECT_EQ(g[3].count, 1);
  EXPECT_EQ(g[3].value, 1);
  // All five share one month key at Month level.
  ASSERT_EQ(groupDaysByName(names, 0, 5, HistoryLevel::Month, g, 8), 1u);
  EXPECT_EQ(g[0].count, 5);
  EXPECT_EQ(g[0].value, 9);
}

TEST(HistoryGroup, WeekNeverStraddlesMonth) {
  // 2024-10-01 (Tue, week 1 of Oct) and 2024-09-30 (Mon, week 6 of Sep) sit
  // in the SAME Monday week but different months — the month inside the key
  // keeps them apart so a week bucket can never span two month labels.
  const char names[][11] = {"2024-10-01", "2024-09-30"};
  HistoryGroup g[4]{};
  ASSERT_EQ(groupDaysByName(names, 0, 2, HistoryLevel::Week, g, 4), 2u);
  EXPECT_EQ(g[0].count, 1);
  EXPECT_EQ(g[0].value, 1);  // week 1 of October
  EXPECT_EQ(g[1].count, 1);
  EXPECT_EQ(g[1].value, 6);  // week 6 of September
}

TEST(HistoryGroup, CapStopsAfterFirstGroup) {
  const char names[][11] = {"2026-01-01", "2025-01-01"};
  HistoryGroup g[1]{};
  EXPECT_EQ(groupDaysByName(names, 0, 2, HistoryLevel::Year, g, 1), 1u);
  EXPECT_EQ(g[0].count, 1);
  EXPECT_EQ(groupDaysByName(names, 0, 0, HistoryLevel::Year, g, 1), 0u);
}

TEST(HistoryGroup, UnparsableNameKeysAsSentinel) {
  const char names[][11] = {"junk", "2026-01-01"};
  HistoryGroup g[4]{};
  ASSERT_EQ(groupDaysByName(names, 0, 2, HistoryLevel::Year, g, 4), 2u);
  EXPECT_EQ(g[0].count, 1);
  EXPECT_EQ(g[0].value, 0);  // sentinel group carries no calendar value
  EXPECT_EQ(g[1].count, 1);
}

TEST(HistoryLabel, FormatsPerLevel) {
  const char names[][11] = {"2026-09-24", "2026-05-01"};
  char label[24];
  historyGroupLabel(names, HistoryGroup{0, 2, 0}, HistoryLevel::Year, label, sizeof(label));
  EXPECT_EQ(std::string(label), "2026");
  historyGroupLabel(names, HistoryGroup{0, 1, 9}, HistoryLevel::Month, label, sizeof(label));
  EXPECT_EQ(std::string(label), "2026-09");
  historyGroupLabel(names, HistoryGroup{1, 1, 5}, HistoryLevel::Month, label, sizeof(label));
  EXPECT_EQ(std::string(label), "2026-05");
  historyGroupLabel(names, HistoryGroup{1, 1, 0}, HistoryLevel::Day, label, sizeof(label));
  EXPECT_EQ(std::string(label), "2026-05-01");
}

TEST(HistoryLabel, WeekSingleAndRange) {
  const char names[][11] = {"2024-09-03", "2024-09-02", "2024-08-31"};  // newest first
  char label[24];
  historyGroupLabel(names, HistoryGroup{0, 2, 0}, HistoryLevel::Week, label, sizeof(label));
  EXPECT_EQ(std::string(label), "09-02 ~ 09-03");  // oldest ~ newest
  historyGroupLabel(names, HistoryGroup{0, 1, 0}, HistoryLevel::Week, label, sizeof(label));
  EXPECT_EQ(std::string(label), "09-03");
  historyGroupLabel(names, HistoryGroup{2, 1, 0}, HistoryLevel::Week, label, sizeof(label));
  EXPECT_EQ(std::string(label), "08-31");
  historyGroupLabel(names, HistoryGroup{0, 0, 0}, HistoryLevel::Week, label, sizeof(label));
  EXPECT_EQ(std::string(label), "");  // empty group -> empty label
}

// ── historyDayRowLabel (flat browser row labels) ────────────────────────────

TEST(HistoryRowLabel, RelativeNearToday) {
  const int64_t today = daysFromCivil(2026, 9, 30);
  char label[24];
  EXPECT_TRUE(historyDayRowLabel(label, sizeof(label), "2026-09-30.jsonl", today, "Today", "Yesterday", "DayBefore"));
  EXPECT_EQ(std::string(label), "Today");
  EXPECT_TRUE(historyDayRowLabel(label, sizeof(label), "2026-09-29", today, "Today", "Yesterday", "DayBefore"));
  EXPECT_EQ(std::string(label), "Yesterday");
  EXPECT_TRUE(historyDayRowLabel(label, sizeof(label), "2026-09-28.jsonl", today, "Today", "Yesterday", "DayBefore"));
  EXPECT_EQ(std::string(label), "DayBefore");
}

TEST(HistoryRowLabel, SameYearShowsMonthDay) {
  const int64_t today = daysFromCivil(2026, 9, 30);
  char label[24];
  EXPECT_TRUE(historyDayRowLabel(label, sizeof(label), "2026-09-15.jsonl", today, "T", "Y", "D"));
  EXPECT_EQ(std::string(label), "09-15");
  EXPECT_TRUE(historyDayRowLabel(label, sizeof(label), "2026-01-05", today, "T", "Y", "D"));
  EXPECT_EQ(std::string(label), "01-05");
  // Clock-skewed "future" day: still today's year, still the short form.
  EXPECT_TRUE(historyDayRowLabel(label, sizeof(label), "2026-10-01.jsonl", today, "T", "Y", "D"));
  EXPECT_EQ(std::string(label), "10-01");
}

TEST(HistoryRowLabel, OtherYearShowsFullDate) {
  const int64_t today = daysFromCivil(2026, 9, 30);
  char label[24];
  EXPECT_TRUE(historyDayRowLabel(label, sizeof(label), "2025-12-31.jsonl", today, "T", "Y", "D"));
  EXPECT_EQ(std::string(label), "2025-12-31");
}

TEST(HistoryRowLabel, UnknownClockFallsBackToFullDate) {
  char label[24];
  EXPECT_TRUE(historyDayRowLabel(label, sizeof(label), "2026-09-30.jsonl", -1, "T", "Y", "D"));
  EXPECT_EQ(std::string(label), "2026-09-30");
  EXPECT_TRUE(historyDayRowLabel(label, sizeof(label), "2026-09-29", -1, "T", "Y", "D"));
  EXPECT_EQ(std::string(label), "2026-09-29");
}

TEST(HistoryRowLabel, EmptyRelativeStringsUseDateForms) {
  const int64_t today = daysFromCivil(2026, 9, 30);
  char label[24];
  EXPECT_TRUE(historyDayRowLabel(label, sizeof(label), "2026-09-30", today, "", "", ""));
  EXPECT_EQ(std::string(label), "09-30");
}

TEST(HistoryRowLabel, UnparsableNameCopiedRaw) {
  char label[24];
  EXPECT_FALSE(historyDayRowLabel(label, sizeof(label), "stray.txt", -1, "T", "Y", "D"));
  EXPECT_EQ(std::string(label), "stray.txt");
}
