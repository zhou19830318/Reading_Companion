#pragma once

// Voice note line format (docs/v4.0-development-plan.md M2): one file per
// book under /.crosspoint/notes/<base>.ntf, append-only JSONL-shaped lines:
//
//   NTF1 <tsMs> <spineIndex> <pageNumber> <pageCount> "<escaped text>"\n
//
// "NTF1 " is the format tag: version + a line shape a stray text file in the
// same directory cannot accidentally match (same pattern as OCL1). Fields are
// decimal, single-space separated; the text is a JSON string — raw UTF-8 for
// CJK (JSON allows it), only quote/backslash/control bytes escaped, control
// bytes as \u00xx so a note can never break the one-record-per-line
// structure. Any JSONL reader can parse the file.
//
// Position fields mirror the reading progress schema (ProgressSlots): the
// spine/page triple identifies where the note was spoken. This engine has no
// CFI; the v4.0 §4.3 schema's cfi field stays "".
//
// Header-only, pure C++ — no ESP-IDF, no Arduino — so test/notes compiles the
// same helpers the firmware uses (each consumer compiles this once).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace Notes {
namespace NoteFormat {

// Root of the note store, under the shared SD cache root the rest of the
// firmware already uses (.crosspoint/).
inline constexpr const char* DIR = "/.crosspoint/notes";

// "NTF1 " — see file comment.
inline constexpr std::string_view LINE_PREFIX = "NTF1 ";

// ".ntf" file extension.
inline constexpr const char* EXT = ".ntf";

// Stored base-name capacity (NUL included). SD long names can reach 255
// bytes, but the browser UI truncates its label anyway; capping here keeps
// every caller's fixed path buffer small ("/" + 96 + ".ntf" + NUL < 112).
// baseNameForBook refuses overlong names rather than truncating mid-UTF-8.
inline constexpr size_t NAME_PATH_SIZE = 96;

// Capacity of the page excerpt a quick mark ("快捷书签") stores with its tag.
// The reader truncates the current page's text to this before handing it over,
// so the composed line stays far inside LINE_CAP no matter how long the chapter
// page is. 192 bytes is ~60 CJK characters: enough to recognize the passage in
// the note list, short enough to read on one screen.
inline constexpr size_t EXCERPT_CAP = 192;

// One note record. Writers fill it (or pass the fields straight to
// buildLine); parsers fill it from a line. NoteStore::load() reuses the same
// struct as the pool index and additionally fills textOff/textLen (offset of
// the NUL-terminated text within the caller's pool).
struct Entry {
  uint64_t tsMs = 0;        // wall-clock millis when the note was saved
  uint16_t spineIndex = 0;  // EPUB spine position (TXT/XTC: 0)
  uint16_t pageNumber = 0;  // page within that spine doc at save time
  uint16_t pageCount = 0;
  uint32_t textOff = 0;  // load side: text offset in the caller's pool
  uint16_t textLen = 0;  // load side: text length in bytes (no NUL)
};

// ── implementation (header-only; rationale in the file comment) ───────────

inline bool baseNameForBook(const char* bookPath, char* out, const size_t cap) {
  if (bookPath == nullptr || out == nullptr || cap == 0) return false;
  const char* start = bookPath;
  while (*start == '/' || *start == '\\') ++start;
  // Cut the extension at the LAST dot so names with inner dots ("J.R.R.epub")
  // survive; a second book with the same base in another directory or another
  // format shares the note file — acceptable for v1, and deterministic.
  const char* lastSlash = nullptr;
  const char* lastDot = nullptr;
  for (const char* p = start; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      lastSlash = p;
      lastDot = nullptr;  // dot must be inside the base segment
    } else if (*p == '.') {
      lastDot = p;
    }
  }
  const char* base = lastSlash != nullptr ? lastSlash + 1 : start;
  const size_t fullLen = strlen(base);
  const size_t len = (lastDot != nullptr) ? static_cast<size_t>(lastDot - base) : fullLen;
  if (len == 0 || len >= cap) return false;
  size_t w = 0;
  for (size_t r = 0; r < len; ++r) {
    const char c = base[r];
    out[w++] = (c == '/' || c == '\\') ? '_' : c;
  }
  out[w] = '\0';
  return true;
}

inline size_t appendEscapedJson(char* out, const size_t cap, const std::string_view text) {
  size_t pos = 0;
  for (const unsigned char c : text) {
    char tmp[6];
    size_t n = 1;
    switch (c) {
      case '"':
        tmp[0] = '\\';
        tmp[1] = '"';
        n = 2;
        break;
      case '\\':
        tmp[0] = '\\';
        tmp[1] = '\\';
        n = 2;
        break;
      case '\n':
        tmp[0] = '\\';
        tmp[1] = 'n';
        n = 2;
        break;
      case '\r':
        tmp[0] = '\\';
        tmp[1] = 'r';
        n = 2;
        break;
      case '\t':
        tmp[0] = '\\';
        tmp[1] = 't';
        n = 2;
        break;
      default:
        if (c < 0x20) {
          // Hex digits must come from a table: '0'+nibble is wrong for
          // nibbles 10..15 (0x1B would emit "\u001;").
          static constexpr char HEX_DIGITS[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
          tmp[0] = '\\';
          tmp[1] = 'u';
          tmp[2] = '0';
          tmp[3] = '0';
          tmp[4] = HEX_DIGITS[c >> 4];
          tmp[5] = HEX_DIGITS[c & 0x0F];
          n = 6;
        } else {
          tmp[0] = static_cast<char>(c);
          n = 1;
        }
        break;
    }
    if (pos + n > cap) return 0;
    std::memcpy(out + pos, tmp, n);
    pos += n;
  }
  return pos;
}

inline size_t buildLine(char* out, const size_t cap, const uint64_t tsMs, const uint16_t spineIndex,
                        const uint16_t pageNumber, const uint16_t pageCount, const std::string_view text) {
  if (out == nullptr || cap < LINE_PREFIX.size() + 2) return 0;

  // Fixed head, then the escaped text. The trailing `"` and `\n` are appended
  // after the escape, so a truncated escape never leaves a broken record.
  int headLen = snprintf(out, cap, "%.*s%llu %u %u %u \"", static_cast<int>(LINE_PREFIX.size()), LINE_PREFIX.data(),
                         static_cast<unsigned long long>(tsMs), spineIndex, pageNumber, pageCount);
  if (headLen <= 0 || static_cast<size_t>(headLen) >= cap) return 0;
  size_t pos = static_cast<size_t>(headLen);
  // Head may leave fewer than 2 bytes (absurdly large ts in a tiny buffer);
  // bail before the "reserve two bytes" arithmetic below underflows.
  if (pos + 2 > cap) return 0;

  // Longest prefix of `text` whose escape fits the remaining room, reserving
  // two bytes for the closing quote and newline. appendEscapedJson returns 0
  // when it ran out, so the probe needs no scratch buffer: escape the prefix
  // straight into out+pos (the final accepted escape overwrites any partial
  // probe bytes, and a probe that returned 0 wrote nothing). Escaping is
  // monotonic — a prefix escapes to a prefix — so a simple shrink loop ends:
  // each iteration either accepts or shrinks by a byte, and the empty prefix
  // always escapes to zero bytes. Real notes are far shorter than the line
  // buffer, so the loop normally runs zero times.
  size_t keep = text.size();
  size_t escLen = 0;
  while (true) {
    escLen = appendEscapedJson(out + pos, cap - pos - 2, text.substr(0, keep));
    if (escLen > 0 || keep == 0) break;
    keep--;  // escape did not fit; drop one byte (UTF-8 boundary risk: the
             // truncation test pins that a CJK note never lands here)
  }
  if (keep > 0) pos += escLen;
  if (pos + 2 > cap) return 0;
  out[pos++] = '"';
  out[pos++] = '\n';
  return pos;
}

inline bool parseLine(const std::string_view line, Entry& e, char* textDst, const size_t textCap, size_t& textLen,
                      bool* overflow = nullptr) {
  if (overflow != nullptr) *overflow = false;
  textLen = 0;
  if (line.size() <= LINE_PREFIX.size()) return false;
  if (line.compare(0, LINE_PREFIX.size(), LINE_PREFIX) != 0) return false;
  const char* p = line.data() + LINE_PREFIX.size();
  const char* const end = line.data() + line.size();

  // Decimal fields separated by single spaces. A tiny helper keeps the
  // sequence honest; unsigned only, no negatives in this format.
  uint64_t fields[4] = {0, 0, 0, 0};
  for (int f = 0; f < 4; ++f) {
    if (p >= end || *p < '0' || *p > '9') return false;
    uint64_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      v = v * 10 + static_cast<uint64_t>(*p - '0');
      if (v > 0xFFFFFFFFFFFFULL) return false;  // absurd: not a ts or a u16 position
      ++p;
    }
    fields[f] = v;
    if (p >= end || *p != ' ') return false;
    ++p;
  }
  e.tsMs = fields[0];
  e.spineIndex = static_cast<uint16_t>(fields[1]);
  e.pageNumber = static_cast<uint16_t>(fields[2]);
  e.pageCount = static_cast<uint16_t>(fields[3]);

  // Opening quote.
  if (p >= end || *p != '"') return false;
  ++p;

  // JSON string body: decode escapes, reject a line whose quote never closes.
  size_t dst = 0;
  while (true) {
    if (p >= end) return false;  // unterminated
    const unsigned char c = static_cast<unsigned char>(*p++);
    if (c == '"') break;
    if (c == '\\') {
      if (p >= end) return false;
      const char esc = *p++;
      switch (esc) {
        case '"':
          if (dst < textCap) textDst[dst++] = '"';
          break;
        case '\\':
          if (dst < textCap) textDst[dst++] = '\\';
          break;
        case 'n':
          if (dst < textCap) textDst[dst++] = '\n';
          break;
        case 'r':
          if (dst < textCap) textDst[dst++] = '\r';
          break;
        case 't':
          if (dst < textCap) textDst[dst++] = '\t';
          break;
        case 'u': {
          if (p + 4 > end) return false;
          uint32_t v = 0;
          for (int k = 0; k < 4; ++k) {
            const char h = *p++;
            v <<= 4;
            if (h >= '0' && h <= '9') {
              v |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              v |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              v |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
              return false;
            }
          }
          // \uD800-\uDBFF must be followed by \uDC00-\uDFFF (a surrogate pair).
          if (v >= 0xD800 && v <= 0xDBFF) {
            if (p + 6 > end || p[0] != '\\' || p[1] != 'u') return false;
            p += 2;
            uint32_t lo = 0;
            for (int k = 0; k < 4; ++k) {
              const char h = *p++;
              lo <<= 4;
              if (h >= '0' && h <= '9') {
                lo |= static_cast<uint32_t>(h - '0');
              } else if (h >= 'a' && h <= 'f') {
                lo |= static_cast<uint32_t>(h - 'a' + 10);
              } else if (h >= 'A' && h <= 'F') {
                lo |= static_cast<uint32_t>(h - 'A' + 10);
              } else {
                return false;
              }
            }
            if (lo < 0xDC00 || lo > 0xDFFF) return false;
            v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
          } else if (v >= 0xDC00 && v <= 0xDFFF) {
            return false;  // lone low surrogate
          }
          // UTF-8 encode.
          char u8[4];
          size_t u8len = 0;
          if (v < 0x80) {
            u8[u8len++] = static_cast<char>(v);
          } else if (v < 0x800) {
            u8[u8len++] = static_cast<char>(0xC0 | (v >> 6));
            u8[u8len++] = static_cast<char>(0x80 | (v & 0x3F));
          } else if (v < 0x10000) {
            u8[u8len++] = static_cast<char>(0xE0 | (v >> 12));
            u8[u8len++] = static_cast<char>(0x80 | ((v >> 6) & 0x3F));
            u8[u8len++] = static_cast<char>(0x80 | (v & 0x3F));
          } else {
            u8[u8len++] = static_cast<char>(0xF0 | (v >> 18));
            u8[u8len++] = static_cast<char>(0x80 | ((v >> 12) & 0x3F));
            u8[u8len++] = static_cast<char>(0x80 | ((v >> 6) & 0x3F));
            u8[u8len++] = static_cast<char>(0x80 | (v & 0x3F));
          }
          for (size_t k = 0; k < u8len; ++k) {
            if (dst < textCap) textDst[dst++] = u8[k];
          }
          break;
        }
        default:
          return false;
      }
    } else {
      if (dst < textCap) textDst[dst++] = static_cast<char>(c);
    }
  }
  // Trailing CR/LF tolerated (buildLine writes LF; CRLF writers get it too;
  // a JSONL reader may pass the line with its terminator). Anything else
  // beyond the closing quote rejects.
  while (p != end && (*p == '\r' || *p == '\n')) ++p;
  if (p != end) return false;

  if (dst >= textCap) {
    if (overflow != nullptr) *overflow = true;
    return false;
  }
  textDst[dst] = '\0';
  textLen = dst;
  return true;
}

}  // namespace NoteFormat
}  // namespace Notes
