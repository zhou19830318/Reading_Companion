#include <gtest/gtest.h>

#include <string>

#include "Utf8.h"

namespace {

// Helpers to build NFD / expected byte sequences explicitly so the test does not
// depend on the encoding of this source file.
const std::string kCombGrave = "\xCC\x80";     // U+0300 COMBINING GRAVE ACCENT
const std::string kCombAcute = "\xCC\x81";     // U+0301 COMBINING ACUTE ACCENT
const std::string kCombCirc = "\xCC\x82";      // U+0302 COMBINING CIRCUMFLEX ACCENT
const std::string kCombDotBelow = "\xCC\xA3";  // U+0323 COMBINING DOT BELOW

}  // namespace

// ASCII and already-precomposed (NFC) text must pass through untouched (fast path).
TEST(Utf8ComposeNfc, PassesThroughAsciiAndNfc) {
  EXPECT_EQ(utf8ComposeNfc(""), "");
  EXPECT_EQ(utf8ComposeNfc("hello world"), "hello world");
  EXPECT_EQ(utf8ComposeNfc("caf\xC3\xA9"), "caf\xC3\xA9");  // é already U+00E9
}

// Single combining mark composes onto its base letter.
TEST(Utf8ComposeNfc, ComposesSingleMark) {
  EXPECT_EQ(utf8ComposeNfc("e" + kCombAcute), "\xC3\xA9");  // e + ́  -> é  (U+00E9)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombGrave), "\xC3\xA0");  // a + ̀  -> à  (U+00E0)
}

// Vietnamese letters carry two stacked marks; composition must accumulate them
// onto the intermediate precomposed character (this is the crux of the feature).
TEST(Utf8ComposeNfc, ComposesStackedVietnameseMarks) {
  // a + circumflex + acute -> ấ (U+1EA5)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombCirc + kCombAcute), "\xE1\xBA\xA5");
  // a + dot-below + circumflex (canonical order) -> ậ (U+1EAD)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombDotBelow + kCombCirc), "\xE1\xBA\xAD");
}

// A combining mark with no composition for its base is left unchanged, and the
// base is preserved.
TEST(Utf8ComposeNfc, LeavesUncomposableMarksIntact) {
  const std::string in = "q" + kCombAcute;  // no precomposed "q with acute"
  EXPECT_EQ(utf8ComposeNfc(in), in);
}

// A leading combining mark (no preceding base) is emitted unchanged.
TEST(Utf8ComposeNfc, HandlesLeadingMark) { EXPECT_EQ(utf8ComposeNfc(kCombAcute), kCombAcute); }

// Marks embedded in a longer word compose while surrounding text is preserved.
TEST(Utf8ComposeNfc, ComposesWithinWord) {
  // "Ti" + e+circ+acute + "ng" -> "Tiếng"
  EXPECT_EQ(utf8ComposeNfc("Ti" + std::string("e") + kCombCirc + kCombAcute + "ng"), "Ti\xE1\xBA\xBFng");
}

// ---- utf8ValidSequenceLen -------------------------------------------------

namespace {
const unsigned char* bytes(const char* s) { return reinterpret_cast<const unsigned char*>(s); }
}  // namespace

// Well-formed input reports its byte length, from ASCII through the 4-byte form.
TEST(Utf8ValidSequenceLen, AcceptsWellFormedSequences) {
  EXPECT_EQ(utf8ValidSequenceLen(bytes("a")), 1u);
  EXPECT_EQ(utf8ValidSequenceLen(bytes("")), 0u);                  // end of string
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xC3\xA9")), 2u);          // é
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xE4\xB8\xAD")), 3u);      // 中
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xF0\x9F\x98\x80")), 4u);  // U+1F600
}

// The shape this exists for: a lead byte with its tail missing, which is what
// a byte-sized snprintf of a CJK title leaves behind.
TEST(Utf8ValidSequenceLen, RejectsTruncatedSequences) {
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xE4")), 0u);
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xE4"
                                       "A")),
            0u);                                               // lead then plain ASCII
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xE4\x84")), 0u);      // one continuation of two
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xF0\x9F\x98")), 0u);  // 3 of 4 bytes
}

// The rest of RFC 3629: shape, range and overlong checks, not just the tail.
TEST(Utf8ValidSequenceLen, RejectsIllFormedButCompleteSequences) {
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\x84")), 0u);              // stray continuation byte
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xC0\xAF")), 0u);          // overlong '/'
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xE0\x80\xAF")), 0u);      // overlong 3-byte form
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xED\xA0\x80")), 0u);      // U+D800 surrogate half
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xF4\x90\x80\x80")), 0u);  // above U+10FFFF
  EXPECT_EQ(utf8ValidSequenceLen(bytes("\xF5\x80\x80\x80")), 0u);  // impossible lead byte
}
