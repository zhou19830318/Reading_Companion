#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "NoteFormat.h"

using Notes::NoteFormat::baseNameForBook;
using Notes::NoteFormat::buildLine;
using Notes::NoteFormat::DIR;
using Notes::NoteFormat::Entry;
using Notes::NoteFormat::EXT;
using Notes::NoteFormat::LINE_PREFIX;
using Notes::NoteFormat::NAME_PATH_SIZE;

namespace {

// Build a line, then parse it back. Returns "<0>"/"<unparseable>" on failure
// so the EXPECT itself shows which half broke.
std::string roundTrip(uint64_t tsMs, uint16_t spine, uint16_t page, uint16_t pageCount, const std::string& text,
                      size_t cap = 512) {
  std::vector<char> buf(cap, '\0');
  const size_t n = buildLine(buf.data(), cap, tsMs, spine, page, pageCount, text);
  if (n == 0) return "<0>";
  Entry e{};
  char dst[512];
  size_t len = 0;
  if (!Notes::NoteFormat::parseLine(std::string_view(buf.data(), n), e, dst, sizeof(dst), len)) {
    return "<unparseable>";
  }
  EXPECT_EQ(e.tsMs, tsMs);
  EXPECT_EQ(e.spineIndex, spine);
  EXPECT_EQ(e.pageNumber, page);
  EXPECT_EQ(e.pageCount, pageCount);
  return std::string(dst, len);
}

}  // namespace

TEST(NoteFormatConstants, StoreLayout) {
  EXPECT_STREQ(DIR, "/.crosspoint/notes");
  EXPECT_STREQ(EXT, ".ntf");
  EXPECT_EQ(LINE_PREFIX, "NTF1 ");
}

TEST(NoteBaseName, PlainPath) {
  char out[NAME_PATH_SIZE];
  ASSERT_TRUE(baseNameForBook("/books/三体.epub", out, sizeof(out)));
  EXPECT_STREQ(out, "三体");
}

TEST(NoteBaseName, LastDotWins) {
  char out[NAME_PATH_SIZE];
  ASSERT_TRUE(baseNameForBook("/books/J.R.R.Tolkien.epub", out, sizeof(out)));
  EXPECT_STREQ(out, "J.R.R.Tolkien");
}

TEST(NoteBaseName, NoExtension) {
  char out[NAME_PATH_SIZE];
  ASSERT_TRUE(baseNameForBook("/books/plainname", out, sizeof(out)));
  EXPECT_STREQ(out, "plainname");
}

TEST(NoteBaseName, DotInDirectoryDoesNotCut) {
  char out[NAME_PATH_SIZE];
  ASSERT_TRUE(baseNameForBook("/a.b/cname.txt", out, sizeof(out)));
  EXPECT_STREQ(out, "cname");
}

TEST(NoteBaseName, RootSlashStripped) {
  char out[NAME_PATH_SIZE];
  ASSERT_TRUE(baseNameForBook("book.epub", out, sizeof(out)));
  EXPECT_STREQ(out, "book");
}

TEST(NoteBaseName, RejectsEmptyAndOverlong) {
  char out[NAME_PATH_SIZE];
  EXPECT_FALSE(baseNameForBook("/books/.epub", out, sizeof(out)));
  std::string longName(NAME_PATH_SIZE, 'x');  // >= cap after the dot is cut
  EXPECT_FALSE(baseNameForBook(longName.c_str(), out, sizeof(out)));
}

TEST(NoteLine, RoundTripAscii) { EXPECT_EQ(roundTrip(1790467200123ULL, 3, 12, 200, "hello note"), "hello note"); }

TEST(NoteLine, RoundTripCjk) {
  // Raw UTF-8 passes through unescaped (JSON allows it), CJK included.
  EXPECT_EQ(roundTrip(1, 0, 0, 1, "在第三章停下,记一句"), "在第三章停下,记一句");
}

TEST(NoteLine, RoundTripEscapes) {
  EXPECT_EQ(roundTrip(7, 1, 2, 9, std::string("q\"b\\s\nl\rt\tab")), std::string("q\"b\\s\nl\rt\tab"));
}

TEST(NoteLine, ControlBytesBecomeUnicodeEscapes) {
  std::vector<char> buf(256, '\0');
  const size_t n = buildLine(buf.data(), buf.size(), 5, 0, 0, 1,
                             std::string("a\x01"
                                         "b"));
  ASSERT_GT(n, 0U);
  const std::string line(buf.data(), n);
  EXPECT_NE(line.find("\\u0001"), std::string::npos);
  // \u0001 in the file must come back as the control byte.
  EXPECT_EQ(roundTrip(5, 0, 0, 1,
                      std::string("a\x01"
                                  "b")),
            std::string("a\x01"
                        "b"));
}

TEST(NoteLine, NewlineCannotBreakRecord) {
  std::vector<char> buf(256, '\0');
  const size_t n = buildLine(buf.data(), buf.size(), 5, 0, 0, 1, std::string("a\nb"));
  ASSERT_GT(n, 0U);
  // Exactly one line, and the newline only appears escaped.
  size_t newlines = 0;
  for (const char c : std::string(buf.data(), n)) newlines += (c == '\n') ? 1 : 0;
  EXPECT_EQ(newlines, 1U);
  EXPECT_EQ(roundTrip(5, 0, 0, 1, std::string("a\nb")), std::string("a\nb"));
}

TEST(NoteLine, UndersizedBufferReturnsZero) {
  // Head itself does not fit -> 0, never a broken record.
  std::vector<char> buf(8, '\0');
  EXPECT_EQ(buildLine(buf.data(), buf.size(), 1, 0, 0, 1, "note"), 0U);
}

TEST(NoteLine, TextTruncatesToCleanPrefix) {
  // Head fits but the text does not: buildLine emits the longest prefix of
  // the text that fits (atomic truncation), still a parseable record. cap 24
  // = head(14, incl. the field-separator space + quote) + 8 text bytes
  // + quote + newline — here an exact fit.
  std::vector<char> buf(24, '\0');
  const size_t n = buildLine(buf.data(), buf.size(), 1, 0, 0, 1, "a longer note than fits");
  ASSERT_EQ(n, 24U);
  EXPECT_EQ(std::string(buf.data(), n), "NTF1 1 0 0 1 \"a longer\"\n");
}

TEST(NoteLine, TruncationIsAtomic) {
  // Shrink the buffer byte by byte: every non-zero result must round-trip to
  // a clean prefix of the input (no partial escapes, no missing quote).
  const std::string text =
      "quote\" back\\ end"
      "\xff";
  for (size_t cap = 16; cap < 96; ++cap) {
    std::vector<char> buf(cap, '\0');
    const size_t n = buildLine(buf.data(), cap, 99, 4, 5, 6, text);
    if (n == 0) continue;
    Entry e{};
    char dst[128];
    size_t len = 0;
    ASSERT_TRUE(Notes::NoteFormat::parseLine(std::string_view(buf.data(), n), e, dst, sizeof(dst), len))
        << "cap=" << cap;
    EXPECT_EQ(e.tsMs, 99U) << "cap=" << cap;
    const std::string got(dst, len);
    EXPECT_EQ(got, text.substr(0, len)) << "cap=" << cap;
  }
}

TEST(NoteLine, ParseRejectsGarbage) {
  Entry e{};
  char dst[128];
  size_t len = 0;
  const bool ok = Notes::NoteFormat::parseLine(std::string_view("not a note line\x02"), e, dst, sizeof(dst), len);
  EXPECT_FALSE(ok);
  // Prefix matches but the fields are not numbers.
  const bool ok2 = Notes::NoteFormat::parseLine(std::string_view("NTF1 x y z \"t\""), e, dst, sizeof(dst), len);
  EXPECT_FALSE(ok2);
  // Unterminated string.
  const bool ok3 = Notes::NoteFormat::parseLine(std::string_view("NTF1 1 2 3 4 \"open"), e, dst, sizeof(dst), len);
  EXPECT_FALSE(ok3);
}

TEST(NoteLine, ParseRejectsLoneSurrogate) {
  Entry e{};
  char dst[128];
  size_t len = 0;
  const bool ok = Notes::NoteFormat::parseLine(std::string_view("NTF1 1 2 3 4 \"\\ud83d\""), e, dst, sizeof(dst), len);
  EXPECT_FALSE(ok);
  // A valid pair decodes to the astral-plane UTF-8 sequence.
  const bool ok2 =
      Notes::NoteFormat::parseLine(std::string_view("NTF1 1 2 3 4 \"\\ud83d\\ude00\""), e, dst, sizeof(dst), len);
  ASSERT_TRUE(ok2);
  EXPECT_EQ(std::string(dst, len), std::string("\xF0\x9F\x98\x80"));
}

TEST(NoteLine, ParseToleratesTrailingCr) {
  std::vector<char> buf(128, '\0');
  const size_t n = buildLine(buf.data(), buf.size(), 8, 0, 0, 1, "crlf file");
  ASSERT_GT(n, 0U);
  std::string line(buf.data(), n);
  line.insert(line.size() - 1, "\r");  // before the \n, as a CRLF writer would
  Entry e{};
  char dst[64];
  size_t len = 0;
  ASSERT_TRUE(Notes::NoteFormat::parseLine(line, e, dst, sizeof(dst), len));
  EXPECT_EQ(std::string(dst, len), "crlf file");
}

TEST(NoteLine, ParseOverflowFlags) {
  Entry e{};
  char dst[4];  // deliberately tiny
  size_t len = 0;
  bool overflow = false;
  const bool ok = Notes::NoteFormat::parseLine(std::string_view("NTF1 1 2 3 4 \"too long for dst\""), e, dst,
                                               sizeof(dst), len, &overflow);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(overflow);
}

TEST(NoteLine, RealLineShape) {
  std::vector<char> buf(256, '\0');
  const size_t n = buildLine(buf.data(), buf.size(), 1790467200123ULL, 12, 3, 210, "h incomes 8");
  ASSERT_GT(n, 0U);
  EXPECT_EQ(std::string(buf.data(), n), "NTF1 1790467200123 12 3 210 \"h incomes 8\"\n");
}
