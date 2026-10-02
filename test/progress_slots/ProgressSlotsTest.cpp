#include <ProgressSlots.h>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

// Host tests for the FR-01 dual-slot progress layout (docs/v4.0-development-plan.md M1).
// The firmware links these exact translation units (ProgressSlots.cpp).
namespace {

using ProgressSlots::Slot;

constexpr uint8_t PAYLOAD_A[6] = {0x01, 0x00, 0x2A, 0x00, 0x0C, 0x00};  // spine 1, page 42, count 12
constexpr uint8_t PAYLOAD_B[6] = {0x02, 0x00, 0x07, 0x00, 0x0C, 0x00};  // spine 2, page 7,  count 12
constexpr uint8_t TXT_PAYLOAD[4] = {0x33, 0x00, 0x00, 0x00};            // TXT page 51

// Builds a slot file with a real payload and returns it as a byte vector.
std::vector<uint8_t> makeSlot(Slot slot, uint16_t seq, const uint8_t* payload, uint8_t len) {
  std::vector<uint8_t> buf(ProgressSlots::SLOT_HEADER_SIZE + ProgressSlots::MAX_PAYLOAD, 0);
  const uint8_t n = ProgressSlots::buildSlotFile(buf.data(), buf.size(), slot, seq, payload, len);
  EXPECT_GT(n, 0u);
  buf.resize(n);
  return buf;
}

std::vector<uint8_t> makeFresh(uint16_t lastSeq, Slot slot, uint8_t payloadLen) {
  std::vector<uint8_t> buf(ProgressSlots::FRESH_FILE_SIZE, 0);
  const uint8_t n = ProgressSlots::buildFreshness(buf.data(), buf.size(), lastSeq, slot, payloadLen);
  EXPECT_EQ(n, ProgressSlots::FRESH_FILE_SIZE);
  return buf;
}

// ── crc8 / seq helpers ─────────────────────────────────────────────────────

TEST(Crc8, KnownVector) {
  // CRC-8/MAXIM of "123456789" is 0xA1 (check value of the catalogue entry).
  const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(ProgressSlots::crc8(check, sizeof(check)), 0xA1);
}

TEST(NextSeq, SkipsZeroOnWrap) {
  EXPECT_EQ(ProgressSlots::nextSeq(0), 1);
  EXPECT_EQ(ProgressSlots::nextSeq(41), 42);
  EXPECT_EQ(ProgressSlots::nextSeq(0xFFFE), 0xFFFF);
  EXPECT_EQ(ProgressSlots::nextSeq(0xFFFF), 1);  // never 0
}

TEST(SeqNear, WindowAndWrap) {
  EXPECT_TRUE(ProgressSlots::seqNear(100, 100));
  EXPECT_TRUE(ProgressSlots::seqNear(101, 100));
  EXPECT_TRUE(ProgressSlots::seqNear(116, 100));   // exactly +DELTA
  EXPECT_FALSE(ProgressSlots::seqNear(117, 100));  // beyond
  EXPECT_TRUE(ProgressSlots::seqNear(84, 100));    // behind within window
  EXPECT_TRUE(ProgressSlots::seqNear(3, 0xFFFF));  // wrapped
}

TEST(SeqNewer, DirectionalWindow) {
  EXPECT_TRUE(ProgressSlots::seqNewer(101, 100));
  EXPECT_FALSE(ProgressSlots::seqNewer(100, 101));
  EXPECT_FALSE(ProgressSlots::seqNewer(100, 100));
  EXPECT_TRUE(ProgressSlots::seqNewer(2, 0xFFFF));  // wrap
}

// ── slot file round trip ───────────────────────────────────────────────────

TEST(SlotFile, RoundTripBothSlots) {
  for (const Slot slot : {Slot::A, Slot::B}) {
    const auto buf = makeSlot(slot, 7, PAYLOAD_A, sizeof(PAYLOAD_A));
    uint16_t seq = 0;
    uint8_t payload[ProgressSlots::MAX_PAYLOAD];
    uint8_t len = 0;
    ASSERT_TRUE(ProgressSlots::parseSlotFile(buf.data(), buf.size(), slot, &seq, payload, &len));
    EXPECT_EQ(seq, 7);
    EXPECT_EQ(len, sizeof(PAYLOAD_A));
    EXPECT_EQ(std::memcmp(payload, PAYLOAD_A, sizeof(PAYLOAD_A)), 0);
  }
}

TEST(SlotFile, SlotAMagicIsRejectedAsSlotB) {
  const auto buf = makeSlot(Slot::A, 1, PAYLOAD_A, sizeof(PAYLOAD_A));
  EXPECT_FALSE(ProgressSlots::parseSlotFile(buf.data(), buf.size(), Slot::B, nullptr, nullptr, nullptr));
}

TEST(SlotFile, CorruptPayloadIsRejected) {
  auto buf = makeSlot(Slot::A, 1, PAYLOAD_A, sizeof(PAYLOAD_A));
  buf[ProgressSlots::SLOT_HEADER_SIZE + 1] ^= 0xFF;  // flip a payload byte
  EXPECT_FALSE(ProgressSlots::parseSlotFile(buf.data(), buf.size(), Slot::A, nullptr, nullptr, nullptr));
}

TEST(SlotFile, CorruptCrcIsRejected) {
  auto buf = makeSlot(Slot::A, 1, PAYLOAD_A, sizeof(PAYLOAD_A));
  buf.back() ^= 0x01;  // flip the CRC byte
  EXPECT_FALSE(ProgressSlots::parseSlotFile(buf.data(), buf.size(), Slot::A, nullptr, nullptr, nullptr));
}

TEST(SlotFile, TruncatedFileIsRejected) {
  auto buf = makeSlot(Slot::A, 1, PAYLOAD_A, sizeof(PAYLOAD_A));
  buf.pop_back();  // CRC byte gone
  EXPECT_FALSE(ProgressSlots::parseSlotFile(buf.data(), buf.size(), Slot::A, nullptr, nullptr, nullptr));
  EXPECT_FALSE(ProgressSlots::parseSlotFile(buf.data(), 3, Slot::A, nullptr, nullptr, nullptr));
  EXPECT_FALSE(ProgressSlots::parseSlotFile(nullptr, 0, Slot::A, nullptr, nullptr, nullptr));
}

TEST(SlotFile, FourByteTxtPayloadRoundTrips) {
  const auto buf = makeSlot(Slot::B, 999, TXT_PAYLOAD, sizeof(TXT_PAYLOAD));
  uint16_t seq = 0;
  uint8_t payload[ProgressSlots::MAX_PAYLOAD];
  uint8_t len = 0;
  ASSERT_TRUE(ProgressSlots::parseSlotFile(buf.data(), buf.size(), Slot::B, &seq, payload, &len));
  EXPECT_EQ(len, 4);
  EXPECT_EQ(std::memcmp(payload, TXT_PAYLOAD, 4), 0);
}

// ── freshness record ───────────────────────────────────────────────────────

TEST(Freshness, RoundTrip) {
  const auto buf = makeFresh(0x1234, Slot::B, 6);
  uint16_t seq = 0;
  Slot slot = Slot::A;
  uint8_t len = 0;
  ASSERT_TRUE(ProgressSlots::parseFreshness(buf.data(), buf.size(), &seq, &slot, &len));
  EXPECT_EQ(seq, 0x1234);
  EXPECT_EQ(slot, Slot::B);
  EXPECT_EQ(len, 6);
}

TEST(Freshness, CorruptRecordIsRejected) {
  auto buf = makeFresh(10, Slot::A, 6);
  buf[4] ^= 0xFF;  // flip a seq byte
  EXPECT_FALSE(ProgressSlots::parseFreshness(buf.data(), buf.size(), nullptr, nullptr, nullptr));
  EXPECT_FALSE(ProgressSlots::parseFreshness(buf.data(), 4, nullptr, nullptr, nullptr));
}

// ── pickNewest arbitration ────────────────────────────────────────────────

TEST(PickNewest, FollowsTheFreshnessRecordWhenBothSlotsValid) {
  // lastSeq says B is newest; A holds an older save.
  const auto a = makeSlot(Slot::A, 40, PAYLOAD_A, 6);
  const auto b = makeSlot(Slot::B, 41, PAYLOAD_B, 6);
  const auto fresh = makeFresh(41, Slot::B, 6);
  Slot out = Slot::A;
  ASSERT_TRUE(ProgressSlots::pickNewest(fresh.data(), fresh.size(), a.data(), a.size(), b.data(), b.size(), 6, &out));
  EXPECT_EQ(out, Slot::B);
}

TEST(PickNewest, FallsBackToOtherSlotWhenNamedOneIsCorrupt) {
  // The freshness record names B, but B's file is damaged: A (one save older,
  // still inside the staleness window) must win.
  auto b = makeSlot(Slot::B, 41, PAYLOAD_B, 6);
  b[5] ^= 0xFF;  // corrupt a seq byte -> CRC mismatch
  const auto a = makeSlot(Slot::A, 40, PAYLOAD_A, 6);
  const auto fresh = makeFresh(41, Slot::B, 6);
  Slot out = Slot::A;
  ASSERT_TRUE(ProgressSlots::pickNewest(fresh.data(), fresh.size(), a.data(), a.size(), b.data(), b.size(), 6, &out));
  EXPECT_EQ(out, Slot::A);
}

TEST(PickNewest, FreshnessRecordLostStillPicksHigherSeq) {
  // The remove+rename window in writeAtomic: progress.bin gone entirely, both
  // slots intact. The higher seq wins with no freshness record to gate.
  const auto a = makeSlot(Slot::A, 40, PAYLOAD_A, 6);
  const auto b = makeSlot(Slot::B, 41, PAYLOAD_B, 6);
  Slot out = Slot::A;
  ASSERT_TRUE(ProgressSlots::pickNewest(nullptr, 0, a.data(), a.size(), b.data(), b.size(), 6, &out));
  EXPECT_EQ(out, Slot::B);
}

TEST(PickNewest, SlotAheadOfFreshnessRecordWins) {
  // Power died between the slot write and the freshness rewrite: the slot is
  // one save AHEAD of lastSeq and must still be trusted.
  const auto a = makeSlot(Slot::A, 40, PAYLOAD_A, 6);
  const auto b = makeSlot(Slot::B, 41, PAYLOAD_B, 6);
  const auto fresh = makeFresh(40, Slot::A, 6);  // stale: names A at 40
  Slot out = Slot::A;
  ASSERT_TRUE(ProgressSlots::pickNewest(fresh.data(), fresh.size(), a.data(), a.size(), b.data(), b.size(), 6, &out));
  EXPECT_EQ(out, Slot::B);
}

TEST(PickNewest, StaleLeftoverSlotFromOlderSessionIsIgnored) {
  // lastSeq rolled far past A's seq (previous session), B is corrupt: nothing
  // trustworthy remains.
  auto b = makeSlot(Slot::B, 500, PAYLOAD_B, 6);
  b[5] ^= 0xFF;
  const auto a = makeSlot(Slot::A, 40, PAYLOAD_A, 6);
  const auto fresh = makeFresh(500, Slot::B, 6);
  Slot out = Slot::A;
  EXPECT_FALSE(ProgressSlots::pickNewest(fresh.data(), fresh.size(), a.data(), a.size(), b.data(), b.size(), 6, &out));
}

TEST(PickNewest, ForeignPayloadLengthIsRejected) {
  // A TXT-format slot (4 bytes) must not be resurrected as EPUB state (6).
  const auto a = makeSlot(Slot::A, 41, TXT_PAYLOAD, 4);
  const auto fresh = makeFresh(41, Slot::A, 4);
  Slot out = Slot::A;
  EXPECT_FALSE(ProgressSlots::pickNewest(fresh.data(), fresh.size(), a.data(), a.size(), nullptr, 0, 6, &out));
  // ...and the reverse direction: expectedPayloadLen 4 accepts it.
  ASSERT_TRUE(ProgressSlots::pickNewest(fresh.data(), fresh.size(), a.data(), a.size(), nullptr, 0, 4, &out));
  EXPECT_EQ(out, Slot::A);
}

TEST(PickNewest, BothSlotsAbsentIsNoProgress) {
  Slot out = Slot::A;
  const auto fresh = makeFresh(9, Slot::A, 6);
  EXPECT_FALSE(ProgressSlots::pickNewest(fresh.data(), fresh.size(), nullptr, 0, nullptr, 0, 6, &out));
  EXPECT_FALSE(ProgressSlots::pickNewest(nullptr, 0, nullptr, 0, nullptr, 0, 6, &out));
}

TEST(PickNewest, LegacyProgressBinIsNeverParsedAsFreshness) {
  // A pre-M1 progress.bin is 4 or 6 bytes — always shorter than the 9-byte
  // freshness record — so old caches are handled by the legacy fallback,
  // never misread here.
  const uint8_t legacy[6] = {0x01, 0x00, 0x2A, 0x00, 0x0C, 0x00};
  const auto a = makeSlot(Slot::A, 41, PAYLOAD_A, 6);
  Slot out = Slot::A;
  // The legacy bytes parse as garbage freshness (rejected), the slot still wins.
  ASSERT_TRUE(ProgressSlots::pickNewest(legacy, sizeof(legacy), a.data(), a.size(), nullptr, 0, 6, &out));
  EXPECT_EQ(out, Slot::A);
}

TEST(PickNewest, ZeroLengthExpectedAcceptsAnyConsistentSize) {
  // expectedPayloadLen 0 = "any": used by the seq-scanning helpers.
  const auto a = makeSlot(Slot::A, 41, TXT_PAYLOAD, 4);
  Slot out = Slot::A;
  ASSERT_TRUE(ProgressSlots::pickNewest(nullptr, 0, a.data(), a.size(), nullptr, 0, 0, &out));
  EXPECT_EQ(out, Slot::A);
}

}  // namespace
