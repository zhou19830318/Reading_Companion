#include "ProgressSlots.h"

#include <cstring>

// See ProgressSlots.h for the format rationale. Everything here is pure
// byte-level layout + validation; the file I/O half (device-only) calls these
// builders/parsers, and the host tests exercise the same code.
namespace ProgressSlots {

// CRC-8/MAXIM: poly 0x31 (x^8+x^5+x^4+1) reflected -> 0x8C, init 0x00, no
// final xor. Table-free.
uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc & 0x01) ? static_cast<uint8_t>((crc >> 1) ^ 0x8C) : static_cast<uint8_t>(crc >> 1);
    }
  }
  return crc;
}

uint16_t nextSeq(uint16_t seq) {
  if (seq >= 0xFFFF) return 1;  // 0 is reserved for "never written"
  return static_cast<uint16_t>(seq + 1);
}

bool seqNear(uint16_t seq, uint16_t base) {
  const uint16_t diff = static_cast<uint16_t>(seq - base);  // mod 0x10000
  return diff <= STALE_SEQ_DELTA || diff >= static_cast<uint16_t>(0x10000U - STALE_SEQ_DELTA);
}

bool seqNewer(uint16_t a, uint16_t b) {
  const uint16_t diff = static_cast<uint16_t>(a - b);  // mod 0x10000
  return diff != 0 && diff <= STALE_SEQ_DELTA;
}

uint8_t buildSlotFile(uint8_t* out, size_t cap, Slot slot, uint16_t seq, const uint8_t* payload, uint8_t payloadLen) {
  if (out == nullptr || payload == nullptr || payloadLen == 0 || payloadLen > MAX_PAYLOAD) return 0;
  // File = SLOT_HEADER_SIZE + payloadLen + 1 CRC byte.
  if (cap < static_cast<size_t>(SLOT_HEADER_SIZE) + payloadLen + 1) return 0;
  out[0] = (slot == Slot::A) ? 0x50 : 0x51;  // 'P' / 'Q'
  out[1] = 0x53;                             // 'S'
  out[2] = 0x31;                             // '1'
  out[3] = (slot == Slot::A) ? 0x41 : 0x42;  // 'A' / 'B'
  out[4] = payloadLen;
  out[5] = static_cast<uint8_t>(seq & 0xFF);
  out[6] = static_cast<uint8_t>(seq >> 8);
  std::memcpy(out + SLOT_HEADER_SIZE, payload, payloadLen);
  // CRC covers len + seq + payload (everything after the fixed magic).
  out[SLOT_HEADER_SIZE + payloadLen] = crc8(out + 4, static_cast<size_t>(payloadLen) + 3);
  return static_cast<uint8_t>(SLOT_HEADER_SIZE + payloadLen + 1);
}

bool parseSlotFile(const uint8_t* data, size_t len, Slot expectedSlot, uint16_t* seq, uint8_t* payload,
                   uint8_t* payloadLen) {
  if (data == nullptr || len < static_cast<size_t>(SLOT_HEADER_SIZE) + 2) return false;  // payload[1] + CRC[1]
  const uint8_t magic0 = (expectedSlot == Slot::A) ? 0x50 : 0x51;
  const uint8_t slotId = (expectedSlot == Slot::A) ? 0x41 : 0x42;
  if (data[0] != magic0 || data[1] != 0x53 || data[2] != 0x31 || data[3] != slotId) return false;
  const uint8_t plen = data[4];
  if (plen == 0 || plen > MAX_PAYLOAD) return false;
  if (len < static_cast<size_t>(SLOT_HEADER_SIZE) + plen + 1) return false;
  if (crc8(data + 4, static_cast<size_t>(plen) + 3) != data[SLOT_HEADER_SIZE + plen]) return false;
  if (seq != nullptr) *seq = static_cast<uint16_t>(data[5] | (data[6] << 8));
  // Length is reported even when the caller skips the payload copy — the
  // picker needs it to reject foreign payload sizes.
  if (payloadLen != nullptr) *payloadLen = plen;
  if (payload != nullptr) {
    std::memcpy(payload, data + SLOT_HEADER_SIZE, plen);
  }
  return true;
}

uint8_t buildFreshness(uint8_t* out, size_t cap, uint16_t lastSeq, Slot lastSlot, uint8_t payloadLen) {
  if (out == nullptr || payloadLen == 0 || payloadLen > MAX_PAYLOAD) return 0;
  if (cap < FRESH_FILE_SIZE) return 0;
  out[0] = 0x50;  // 'P'
  out[1] = 0x53;  // 'S'
  out[2] = 0x46;  // 'F'
  out[3] = 0x31;  // '1'
  out[4] = static_cast<uint8_t>(lastSeq & 0xFF);
  out[5] = static_cast<uint8_t>(lastSeq >> 8);
  out[6] = (lastSlot == Slot::A) ? 0 : 1;
  out[7] = payloadLen;
  out[8] = crc8(out + 4, 4);
  return FRESH_FILE_SIZE;
}

bool parseFreshness(const uint8_t* data, size_t len, uint16_t* lastSeq, Slot* lastSlot, uint8_t* payloadLen) {
  if (data == nullptr || len < FRESH_FILE_SIZE) return false;
  if (data[0] != 0x50 || data[1] != 0x53 || data[2] != 0x46 || data[3] != 0x31) return false;
  if (crc8(data + 4, 4) != data[8]) return false;
  if (lastSeq != nullptr) *lastSeq = static_cast<uint16_t>(data[4] | (data[5] << 8));
  if (lastSlot != nullptr) *lastSlot = (data[6] == 0) ? Slot::A : Slot::B;
  if (payloadLen != nullptr) *payloadLen = data[7];
  return true;
}

namespace {

// One candidate slot: parse once, keep the fields the picker needs.
struct Candidate {
  bool valid = false;
  uint16_t seq = 0;
  uint8_t payloadLen = 0;
};

Candidate examine(const uint8_t* data, size_t len, Slot slot) {
  Candidate c;
  c.valid = parseSlotFile(data, len, slot, &c.seq, nullptr, &c.payloadLen);
  return c;
}

}  // namespace

bool pickNewest(const uint8_t* freshData, size_t freshLen, const uint8_t* dataA, size_t lenA, const uint8_t* dataB,
                size_t lenB, uint8_t expectedPayloadLen, Slot* outSlot) {
  Candidate a = examine(dataA, lenA, Slot::A);
  Candidate b = examine(dataB, lenB, Slot::B);

  // Foreign payload sizes never win: a slot from another reader type (or an
  // old format) must not be resurrected as this reader's position.
  if (expectedPayloadLen != 0) {
    if (a.valid && a.payloadLen != expectedPayloadLen) a.valid = false;
    if (b.valid && b.payloadLen != expectedPayloadLen) b.valid = false;
  }

  // Staleness gate against the freshness record, when it parses. A slot is
  // dropped when its seq is neither near lastSeq nor exactly the "one ahead"
  // case (an interrupted freshness rewrite). seqNear already accepts ±DELTA,
  // which subsumes the one-ahead case.
  uint16_t lastSeq = 0;
  bool haveFresh = parseFreshness(freshData, freshLen, &lastSeq, nullptr, nullptr);
  if (haveFresh) {
    if (a.valid && !seqNear(a.seq, lastSeq)) a.valid = false;
    if (b.valid && !seqNear(b.seq, lastSeq)) b.valid = false;
  }

  if (!a.valid && !b.valid) return false;
  // Prefer the higher seq; exact ties are impossible by construction (the two
  // slots alternate and never carry the same seq in one session), but if a
  // card somehow has one, slot A wins deterministically.
  const Slot winner = (a.valid && (!b.valid || a.seq >= b.seq)) ? Slot::A : Slot::B;
  if (outSlot != nullptr) *outSlot = winner;
  return true;
}

}  // namespace ProgressSlots
