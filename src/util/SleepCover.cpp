#include "SleepCover.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstddef>

#include "CrossPointSettings.h"

namespace SleepCover {

namespace {
constexpr size_t COPY_CHUNK = 2048;
}  // namespace

bool apply(const char* srcPath) {
  if (srcPath == nullptr || srcPath[0] == '\0') return false;

  auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_CHUNK);
  if (!buffer) {
    LOG_ERR("SCOVER", "OOM: %u bytes", static_cast<unsigned>(COPY_CHUNK));
    return false;
  }

  bool success = false;
  HalFile inFile, outFile;
  if (Storage.openFileForRead("SCOVER", srcPath, inFile)) {
    if (Storage.openFileForWrite("SCOVER", "/sleep.bmp", outFile)) {
      success = true;
      int bytesRead;
      while ((bytesRead = inFile.read(buffer.get(), COPY_CHUNK)) > 0) {
        if (outFile.write(buffer.get(), static_cast<size_t>(bytesRead)) != static_cast<size_t>(bytesRead)) {
          success = false;
          break;
        }
      }
      // An exact-size read loop that ends on 0 is a complete copy; a read
      // error returns negative and must not pass as one.
      if (bytesRead < 0) success = false;
      outFile.close();
    }
    inFile.close();
  }

  if (!success) {
    LOG_ERR("SCOVER", "copy failed: %s -> /sleep.bmp", srcPath);
    return false;
  }
  SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
  SETTINGS.saveToFile();
  LOG_INF("SCOVER", "cover set from %s", srcPath);
  return true;
}

}  // namespace SleepCover
