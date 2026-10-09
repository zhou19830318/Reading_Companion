#pragma once

#include <cstdint>
#include <string>
#define REPLACEMENT_GLYPH 0xFFFD

uint32_t utf8NextCodepoint(const unsigned char** string);
// Appends a Unicode codepoint to a std::string in UTF-8 encoding.
void utf8AppendCodepoint(uint32_t cp, std::string& out);
// Remove the last UTF-8 codepoint from a std::string and return the new size.
size_t utf8RemoveLastChar(std::string& str);
// Truncate string by removing N UTF-8 codepoints from the end.
void utf8TruncateChars(std::string& str, size_t numChars);

// Canonical composition (NFC) for the Latin / Vietnamese range: precomposes a
// base letter followed by combining diacritical mark(s) into a single codepoint.
// Needed because the device fonts have no combining-mark positioning, so text
// stored in NFD (e.g. some EPUB chapter titles) otherwise renders broken.
std::string utf8ComposeNfc(const std::string& in);

// Truncate a raw char buffer to the last complete UTF-8 codepoint boundary.
// Returns the new length (<= len). If the buffer ends mid-sequence, the
// incomplete trailing bytes are excluded.
int utf8SafeTruncateBuffer(const char* buf, int len);

// Length in bytes (1..4) of the well-formed UTF-8 sequence starting at `s`,
// or 0 when there is no such sequence — end of string, or bytes that do not
// form one. Covers the whole RFC 3629 shape: continuation bytes, overlong
// forms, UTF-16 surrogate halves and anything above U+10FFFF are rejected, not
// just a lead byte missing its tail.
//
// `s` must be NUL-terminated: a sequence running into the terminator is
// incomplete and therefore ill formed, and the scan never reads past that
// terminator — a missing continuation is always detected before it would.
size_t utf8ValidSequenceLen(const unsigned char* s);

// Returns true for CJK characters that allow line breaks on either side without hyphenation.
// Covers CJK Unified Ideographs, Hiragana, Katakana, Hangul Syllables, CJK punctuation,
// and fullwidth forms — the ranges where word boundaries are implicit per character.
inline bool utf8IsCjkBreakable(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x3000 && cp <= 0x303F)     // CJK Symbols and Punctuation
         || (cp >= 0x3040 && cp <= 0x309F)     // Hiragana
         || (cp >= 0x30A0 && cp <= 0x30FF)     // Katakana
         || (cp >= 0x3130 && cp <= 0x318F)     // Hangul Compatibility Jamo
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xAC00 && cp <= 0xD7AF)     // Hangul Syllables
         || (cp >= 0xD7B0 && cp <= 0xD7FF)     // Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2A6DF)   // CJK Extension B
         || (cp >= 0x2A700 && cp <= 0x2B73F);  // CJK Extension C
}

// Returns true for Unicode combining diacritical marks that should not advance the cursor.
inline bool utf8IsCombiningMark(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F)      // Combining Diacritical Marks
         || (cp >= 0x1DC0 && cp <= 0x1DFF)   // Combining Diacritical Marks Supplement
         || (cp >= 0x20D0 && cp <= 0x20FF)   // Combining Diacritical Marks for Symbols
         || (cp >= 0xFE20 && cp <= 0xFE2F);  // Combining Half Marks
}
