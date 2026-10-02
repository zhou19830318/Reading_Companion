#pragma once

#include <Epub.h>
#include <Logging.h>

#include "ProgressFile.h"
#include "ProgressSlots.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory.
//
// FR-01 dual-slot: the payload is the same 6 bytes the legacy progress.bin
// carried (spineIndex, pageNumber, pageCount — u16 LE each); writeDualSlot
// adds two redundant slot copies + a freshness record so a power cut during
// the FAT remove+rename can no longer lose the position entirely. `seq` is
// the caller-owned counter (see EpubReaderActivity::progressSeq_).
// Returns true on success.
inline bool saveProgress(const Epub& epub, uint16_t seq, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  if (ProgressFile::writeDualSlot(epub.getCachePath(), seq, data, sizeof(data)) == 0) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils
