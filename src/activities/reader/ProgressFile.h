#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "ProgressSlots.h"

namespace ProgressFile {

// Writes `len` bytes of reader progress to `<cachePath>/progress.bin` without
// ever leaving the canonical file half-written.
//
// The bytes go to a temporary `progress.bin.tmp` first; only once that is fully
// written and closed is it renamed over progress.bin. An interrupted write
// (power loss or a crash mid-SPI) therefore damages only the throwaway temp file.
// Previously a truncate-in-place write that was cut short left progress.bin with
// a broken FAT cluster chain that the firmware could neither rewrite nor clear,
// stranding the book on an old page (issue #2275).
//
// This is crash-safe, not metadata-atomic: on FAT the replace is remove + rename,
// two separate directory operations, so a crash between them can leave neither
// file -- which simply reads as "no saved progress" on next launch, never a
// corrupt or unclearable file. The point is that progress.bin is never torn.
//
// Note: this prevents corruption on a healthy card going forward. It cannot
// repair an already-corrupted progress.bin -- removing the stale file may itself
// fail at the FAT level, in which case recovery still requires fsck on a host.
//
// Returns true only if the new progress.bin is fully in place.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, size_t len) {
  const std::string finalPath = cachePath + "/progress.bin";
  const std::string tmpPath = cachePath + "/progress.bin.tmp";

  {
    HalFile f;
    if (!Storage.openFileForWrite("PRG", tmpPath, f)) {
      LOG_ERR("PRG", "Could not open temp progress file for write: %s", tmpPath.c_str());
      return false;
    }
    const size_t written = f.write(data, len);
    if (written != len) {
      LOG_ERR("PRG", "Short write saving progress to %s: %u/%u bytes", tmpPath.c_str(), (unsigned)written,
              (unsigned)len);
      return false;
    }
    f.flush();
    // f (the temp file) is closed at scope exit (DESTRUCTOR_CLOSES_FILE=1) before
    // the rename below -- SdFat must not rename a path that still has an open FsFile.
  }

  // SdFat's rename does not overwrite an existing destination, so drop the old
  // canonical file first. The brief window where neither file exists reads as
  // "no saved progress" on next launch -- never a corrupt, unclearable file.
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("PRG", "Failed to rename temp progress into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

// ── FR-01 dual-slot layer (docs/v4.0-development-plan.md M1) ───────────────
// writeAtomic above guarantees the canonical progress.bin is never torn but
// still loses the position entirely when power dies between its remove and
// rename. writeDualSlot adds two redundant slot files + a tiny freshness
// record; readNewest recovers the newest slot whenever progress.bin is lost.
// Format and staleness rules: ProgressSlots.h.

// Alternating write cursor, advanced by writeDualSlot. Static (not persisted):
// which slot is "next" only matters within one session, and readNewest is
// seq-driven, not cursor-driven.
inline uint8_t& dualSlotCursor() {
  static uint8_t cursor = 0;
  return cursor;
}

// Reads a whole small file into buf (up to cap bytes). Returns bytes read,
// or 0 when the file is absent or larger than cap.
inline size_t readSmallFile(const char* tag, const std::string& path, uint8_t* buf, size_t cap) {
  HalFile f;
  if (!Storage.openFileForRead(tag, path, f)) return 0;
  const int n = f.read(buf, cap);
  return n > 0 ? static_cast<size_t>(n) : 0;
}

// Writes buf to path whole. These are 9-20 byte metadata files; the FAT
// directory entry is allocated on first write and reused afterwards, so the
// per-save churn is one data-cluster rewrite. Returns true on full write.
inline bool writeSmallFile(const char* tag, const std::string& path, const uint8_t* buf, size_t len) {
  HalFile f;
  if (!Storage.openFileForWrite(tag, path, f)) {
    LOG_ERR("PRG", "Could not open %s for write", path.c_str());
    return false;
  }
  return f.write(buf, len) == len;
}

// Writes reader progress to the dual slots + freshness record.
//
// `seq` is the caller-owned monotonic counter (seed once via readNewestSeq at
// activity enter, then advance with ProgressSlots::nextSeq per save). Returns
// the seq persisted, or 0 on failure.
//
// Order matters: slot first, freshness second. If power dies between the two,
// readNewest still finds the new slot one save ahead of lastSeq — inside the
// staleness window. Only a failure during the slot write itself loses data,
// and then the previous slot (one save older) is still there.
inline uint16_t writeDualSlot(const std::string& cachePath, uint16_t seq, const uint8_t* payload, uint8_t len) {
  const ProgressSlots::Slot slot = (dualSlotCursor() & 1) == 0 ? ProgressSlots::Slot::A : ProgressSlots::Slot::B;
  dualSlotCursor() ^= 1;

  uint8_t slotBuf[ProgressSlots::SLOT_FILE_SIZE];
  const uint8_t slotLen = ProgressSlots::buildSlotFile(slotBuf, sizeof(slotBuf), slot, seq, payload, len);
  if (slotLen == 0) {
    LOG_ERR("PRG", "Slot file build failed (len=%u)", static_cast<unsigned>(len));
    return 0;
  }

  const std::string slotPath = cachePath + (slot == ProgressSlots::Slot::A ? "/progress_a.bin" : "/progress_b.bin");
  if (!writeSmallFile("PRG", slotPath, slotBuf, slotLen)) {
    LOG_ERR("PRG", "Slot write failed: %s", slotPath.c_str());
    return 0;
  }

  uint8_t freshBuf[ProgressSlots::FRESH_FILE_SIZE];
  const uint8_t freshLen = ProgressSlots::buildFreshness(freshBuf, sizeof(freshBuf), seq, slot, len);
  if (freshLen == 0 || !writeSmallFile("PRG", cachePath + "/progress.bin", freshBuf, freshLen)) {
    LOG_ERR("PRG", "Freshness write failed; slot %c seq %u still recoverable",
            slot == ProgressSlots::Slot::A ? 'A' : 'B', static_cast<unsigned>(seq));
  }
  return seq;
}

// Recovers the newest slot payload for one reader type.
//
// Reads progress.bin (freshness) + both slot files, arbitrates via
// ProgressSlots::pickNewest, and copies the winner's payload into out (up to
// cap bytes; the real payloads are 4-6). Returns true when a trustworthy slot
// was found.
//
// `seqOut`, when non-null, receives the freshest seq seen (freshness record
// or either slot) so a caller that owns a counter can resume past all
// existing state instead of restarting at 1 and colliding with stale slots.
inline bool readNewest(const std::string& cachePath, uint8_t* out, uint8_t cap, uint8_t expectedPayloadLen,
                       uint16_t* seqOut = nullptr) {
  uint8_t freshBuf[16];
  uint8_t bufA[ProgressSlots::SLOT_FILE_SIZE];
  uint8_t bufB[ProgressSlots::SLOT_FILE_SIZE];
  const size_t freshLen = readSmallFile("PRG", cachePath + "/progress.bin", freshBuf, sizeof(freshBuf));
  const size_t lenA = readSmallFile("PRG", cachePath + "/progress_a.bin", bufA, sizeof(bufA));
  const size_t lenB = readSmallFile("PRG", cachePath + "/progress_b.bin", bufB, sizeof(bufB));

  uint16_t hi = 0;
  uint16_t s = 0;
  uint8_t plen = 0;
  if (ProgressSlots::parseFreshness(freshBuf, freshLen, &s, nullptr, nullptr) && s > hi) hi = s;
  if (ProgressSlots::parseSlotFile(bufA, lenA, ProgressSlots::Slot::A, &s, nullptr, &plen) && s > hi) hi = s;
  if (ProgressSlots::parseSlotFile(bufB, lenB, ProgressSlots::Slot::B, &s, nullptr, &plen) && s > hi) hi = s;
  if (seqOut != nullptr) *seqOut = hi;

  ProgressSlots::Slot slot = ProgressSlots::Slot::A;
  if (!ProgressSlots::pickNewest(freshBuf, freshLen, bufA, lenA, bufB, lenB, expectedPayloadLen, &slot)) {
    return false;
  }

  const uint8_t* data = (slot == ProgressSlots::Slot::A) ? bufA : bufB;
  const size_t dataLen = (slot == ProgressSlots::Slot::A) ? lenA : lenB;
  if (!ProgressSlots::parseSlotFile(data, dataLen, slot, &s, out, &plen)) {
    LOG_ERR("PRG", "Winner slot failed re-parse");
    return false;
  }
  if (plen > cap) {
    LOG_ERR("PRG", "Slot payload %u exceeds caller buffer %u", static_cast<unsigned>(plen), static_cast<unsigned>(cap));
    return false;
  }
  LOG_INF("PRG", "Recovered progress from slot %c seq %u", slot == ProgressSlots::Slot::A ? 'A' : 'B',
          static_cast<unsigned>(s));
  return true;
}

// Freshest seq across the freshness record and both slots (the highest
// parseable seq is the floor for the caller's counter). Returns 0 when
// nothing parseable exists (fresh card / legacy-only cache).
inline uint16_t readNewestSeq(const std::string& cachePath) {
  uint16_t hi = 0;
  uint8_t out[ProgressSlots::MAX_PAYLOAD];
  readNewest(cachePath, out, sizeof(out), 0, &hi);
  return hi;
}

}  // namespace ProgressFile
