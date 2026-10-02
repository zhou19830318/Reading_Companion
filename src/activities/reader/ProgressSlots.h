#pragma once

#include <cstddef>
#include <cstdint>

// FR-01 dual-slot reading progress (docs/v4.0-development-plan.md M1).
//
// Problem this solves: ProgressFile::writeAtomic() guarantees progress.bin is
// never TORN, but the FAT replace is remove + rename — two directory
// operations. Power lost between them leaves NEITHER progress.bin nor the
// temp file, which reads as "no saved progress" and strands the reader on an
// old page (or chapter 0) — the window writeAtomic's own header documents.
//
// This module adds two independent slots + a freshness record. Every save
// writes ONE slot (alternating) and then rewrites the small freshness record;
// a save is only lost if power dies during THAT save, and the reader falls
// back to the previous slot. Recovery = newest valid slot by monotonic
// sequence number.
//
// Layout (little-endian; fields are memcpy'd byte by byte, never punned —
// unaligned multi-byte loads fault on this core):
//
//   slot file (progress_a.bin / progress_b.bin), 8+plen bytes:
//     [0] 'P' or 'Q'    — slot A / slot B magic
//     [1] 'S'
//     [2] '1'           — format version
//     [3] 'A' or 'B'    — slot id, redundant + self-describing
//     [4] payloadLen    — reader byte count (EPUB 6, TXT 4, XTC 4)
//     [5..6] seq        — monotonic write counter, u16 LE
//     [7..] payload     — the reader's own bytes, verbatim
//     [last] crc        — CRC-8/MAXIM over [4..last): len + seq + payload
//
//   freshness record (progress.bin, 9 bytes — replaces the readers' legacy
//   4/6-byte file in place):
//     [0..3] 'PSF1'
//     [4..5] lastSeq    — seq of the newest committed slot
//     [6]    lastSlot   — 0 = A, 1 = B
//     [7]    payloadLen
//     [8]    crc        — CRC-8/MAXIM over [4..8)
//
// progress.bin is REWRITTEN in this format on the first dual-slot save. An
// OLD firmware reading the same card sees a 9-byte progress.bin — neither
// legacy size (4/6) — and reads "no progress": the book opens at chapter 0,
// which is the old behaviour after a lost replace anyway, never a torn read.
// NEW firmware ignores a legacy progress.bin (too short to parse) and falls
// back to whichever slot is newest, which also covers a crash between the
// slot write and the freshness rewrite.
//
// Staleness rules ("which slot wins"):
//   - a slot whose magic/version/len/CRC is broken is ignored;
//   - a slot whose payloadLen differs from expectedPayloadLen is foreign
//     (another reader type or an old format) — ignored;
//   - a slot whose seq is within STALE_SEQ_DELTA BEHIND the freshness record
//     is treated as current (the freshness record lags by at most one save);
//     anything further behind is a leftover from a previous session that
//     already rolled the counter past it — ignored. A slot NEWER than
//     lastSeq by one is also accepted: that is a slot whose freshness rewrite
//     was interrupted.
//
// Pure C++ — no Arduino/ESP-IDF — so test/progress_slots compiles the same
// translation units the firmware links. Storage I/O lives in the device-only
// half (ProgressFile.h writeDualSlot/readNewest).
namespace ProgressSlots {

// Slots are small; 64 covers every reader payload with room for one more
// field without another format bump.
static constexpr uint8_t MAX_PAYLOAD = 64;
// 5 fixed bytes + 2 seq bytes; the payload and one CRC byte follow.
static constexpr uint8_t SLOT_HEADER_SIZE = 7;
static constexpr uint8_t SLOT_FILE_SIZE = SLOT_HEADER_SIZE + 1 + MAX_PAYLOAD;
static constexpr uint8_t FRESH_FILE_SIZE = 9;
// How far behind lastSeq a slot may be and still count as current. 16 saves
// of margin: wrap handling needs (uint16_t)(lastSeq - seq) to stay far from
// 0x8000, and 16 rolls >> the one-save lag the record can have.
static constexpr uint16_t STALE_SEQ_DELTA = 16;

enum class Slot : uint8_t { A = 0, B = 1 };

// Serialises one slot file into `out`. Returns bytes written (SLOT_HEADER_SIZE
// + payloadLen), or 0 on bad input.
uint8_t buildSlotFile(uint8_t* out, size_t cap, Slot slot, uint16_t seq, const uint8_t* payload, uint8_t payloadLen);

// Parses + validates one slot file. Returns true and fills seq/payload/
// payloadLen when magic, version, slot id, length, and CRC all check out.
bool parseSlotFile(const uint8_t* data, size_t len, Slot expectedSlot, uint16_t* seq, uint8_t* payload,
                   uint8_t* payloadLen);

// Serialises the freshness record (FRESH_FILE_SIZE bytes). Returns byte count
// or 0 on bad input.
uint8_t buildFreshness(uint8_t* out, size_t cap, uint16_t lastSeq, Slot lastSlot, uint8_t payloadLen);

// Parses + validates the freshness record. Returns true and fills the outputs
// when magic and CRC check out.
bool parseFreshness(const uint8_t* data, size_t len, uint16_t* lastSeq, Slot* lastSlot, uint8_t* payloadLen);

// CRC-8/MAXIM (reflected poly 0x8B, init 0x00, no final xor) — table-free.
uint8_t crc8(const uint8_t* data, size_t len);

// Next sequence number, honouring wrap: 0xFFFF -> 1 (0 stays reserved as
// "never written"; skipping it keeps an all-zero erased region invalid).
uint16_t nextSeq(uint16_t seq);

// True when `seq` is within STALE_SEQ_DELTA of `base` in either direction
// (mod-0x10000): current-enough per the staleness rules above.
bool seqNear(uint16_t seq, uint16_t base);

// True when `a` is at most STALE_SEQ_DELTA newer than `b` (mod-0x10000).
bool seqNewer(uint16_t a, uint16_t b);

// Picks the newest trustworthy slot.
//
// freshData/freshLen: the freshness record as read (len 0 = absent/corrupt).
// dataA/lenA, dataB/lenB: the two slot files as read (len 0 = absent).
// expectedPayloadLen: the caller's payload size — slots with a different
//   payloadLen are foreign and never returned (0 accepts any one consistent
//   size, used by tests).
//
// Precedence: among the slots that parse, match the payload length, and pass
// the staleness check (when the freshness record parses), the highest seq
// wins. With no usable freshness record, the highest valid seq wins.
// Returns false when nothing is trustworthy — the caller falls back to the
// legacy behaviour (open at chapter 0).
bool pickNewest(const uint8_t* freshData, size_t freshLen, const uint8_t* dataA, size_t lenA, const uint8_t* dataB,
                size_t lenB, uint8_t expectedPayloadLen, Slot* outSlot);

}  // namespace ProgressSlots
