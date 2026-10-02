#pragma once

#include <cstddef>
#include <cstdint>

#include "NoteFormat.h"

// SD-backed voice notes (docs/v4.0-development-plan.md M2): append-only NTF1
// files under /.crosspoint/notes/, one file per book. This is the only layer
// that touches HalStorage for notes; the activities render from the structures
// filled here and never open a file themselves.
//
// Memory discipline mirrors OpenClaw::ChatHistory: a book's notes are never
// loaded as one String. listBooks() reads file names only; load() streams the
// file through NoteFormat::parseLine into a caller-owned PSRAM pool, keeping
// the newest entries (the same front-packed-pool invariant as the chat day
// loader); append() opens the book's file in append mode, writes one line,
// closes. One write per saved note — the same SD throttling discipline as the
// history and progress layers.
//
// Device-only: HalStorage. The format half (NoteFormat.h) is host-tested.
namespace Notes {

class NoteStore {
 public:
  using Entry = NoteFormat::Entry;

  // Caller scratch for one NTF1 line. A spoken note is CHAT_MESSAGE_CAP-sized
  // at most, but escapes can sextuple control bytes; the chat history line
  // scratch is 6 KB for the same reason. VoiceActivity owns one in PSRAM.
  static constexpr size_t LINE_CAP = 2048;

  // ── write side ────────────────────────────────────────────────────────
  // Appends one note to <bookBaseName>.ntf. Creates the directory and file on
  // first use. Returns false on SD failure (the caller surfaces that; the
  // capture itself is not lost until this fails — the caller retries).
  static bool append(const char* bookBaseName, uint64_t tsMs, uint16_t spineIndex, uint16_t pageNumber,
                     uint16_t pageCount, const char* text, char* lineBuf, size_t lineCap);

  // ── read side ─────────────────────────────────────────────────────────
  // Lists note file base names, sorted by name. Returns the count (<= cap).
  // Only files ending in .ntf are listed. Caller owns the name buffers
  // (NAME_PATH_SIZE stride, e.g. a PSRAM block).
  static size_t listBooks(char names[][NoteFormat::NAME_PATH_SIZE], size_t cap);

  // Loads one book's notes (by base name from listBooks) into `pool`. Keeps
  // the LAST entries: when the file holds more than fits, the oldest are
  // dropped so the browser shows the latest notes. Entries are ordered
  // oldest -> newest. lineBuf is read scratch (LINE_CAP bytes).
  static size_t load(const char* bookBaseName, char* pool, size_t cap, Entry* entries, size_t entryCap, char* lineBuf,
                     size_t lineCap);

  // Removes one entry — `index` in the ordering load() returns (oldest ->
  // newest) — by rewriting the file without that line: source streams through
  // the caller's line scratch into a temp file, then temp replaces source, so
  // the buffer is sized by line length and never by the file. The last entry
  // of a book deletes the file itself (listBooks then drops the book). False
  // on SD failure or an index past the end; the file is left untouched then.
  static bool removeEntry(const char* bookBaseName, size_t index, char* lineBuf, size_t lineCap);

  // Deletes one book's note file. Returns true when it is gone (or was never
  // there).
  static bool removeBook(const char* bookBaseName);
};

}  // namespace Notes
