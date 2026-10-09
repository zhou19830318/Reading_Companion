#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageRamCache.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

// Blit one packed 2-bit row into the current render target, clipped to the
// active strip band. Shared by the PSRAM and .pxc paths so the unpack loop
// exists once.
void blitPackedRow(DirectPixelWriter& pw, const uint8_t* rowBuffer, int x, int y, int width) {
  pw.beginRow(y);
  // On a grayscale strip pass only a narrow column window of the image is in
  // the active band; skip the rest instead of unpacking+clipping every pixel.
  int colStart, colEnd;
  pw.bandColRange(x, width, colStart, colEnd);
  for (int col = colStart; col < colEnd; col++) {
    const int byteIdx = col >> 2;            // col / 4
    const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
    const uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;
    pw.writePixel(x + col, pixelValue);
  }
}

// PSRAM hit: the decode left the packed image in memory, so every strip pass
// reads RAM instead of re-streaming the .pxc off the SD card.
bool renderFromRamCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                        int expectedHeight) {
  const ImageRamCache::Entry* ram = ImageRamCache::find(cachePath);
  if (!ram) return false;

  const int widthDiff = abs(ram->width - expectedWidth);
  const int heightDiff = abs(ram->height - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    LOG_ERR("IMG", "PSRAM cache dimension mismatch: %ux%u vs %dx%d", ram->width, ram->height, expectedWidth,
            expectedHeight);
    return false;
  }

  LOG_DBG("IMG", "Loading from PSRAM: %s (%ux%u)", cachePath.c_str(), ram->width, ram->height);

  DirectPixelWriter pw;
  pw.init(renderer);
  const int bytesPerRow = ram->bytesPerRow;
  const int cachedWidth = ram->width;
  const int cachedHeight = ram->height;
  for (int row = 0; row < cachedHeight; row++) {
    blitPackedRow(pw, ram->pixels + (size_t)row * bytesPerRow, x, y + row, cachedWidth);
  }

  LOG_DBG("IMG", "PSRAM render complete");
  return true;
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  if (renderFromRamCache(renderer, cachePath, x, y, expectedWidth, expectedHeight)) {
    return true;
  }

  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  // Verify dimensions are close (allow 1 pixel tolerance for rounding differences)
  int widthDiff = abs(cachedWidth - expectedWidth);
  int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    LOG_ERR("IMG", "Cache dimension mismatch: %dx%d vs %dx%d", cachedWidth, cachedHeight, expectedWidth,
            expectedHeight);
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    blitPackedRow(pw, rowBuffer, x, y + row, cachedWidth);
  }

  free(readBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  // Try to render from cache first.
  //
  // Which cache: a packed image that fits in PSRAM is far cheaper to decode
  // (~0.5 s once, then free on every strip pass) than to stream back off the SD
  // card (~300 ms per pass, ~15 passes per render). So a .pxc left behind by an
  // older build is only consulted when PSRAM cannot hold this image or a PSRAM
  // allocation has already failed; otherwise the decode path runs and the result
  // lands in PSRAM.
  std::string cachePath = getCachePath(imagePath);
  const bool ramBacked = ImageRamCache::psramUsable(width, height);
  if (ramBacked) {
    if (renderFromRamCache(renderer, cachePath, x, y, width, height)) {
      return;
    }
  } else if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    // The source image may be gone while an older build's .pxc is not.
    if (ramBacked && renderFromCache(renderer, cachePath, x, y, width, height)) {
      return;
    }
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  serialization::readString(file, path);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new ImageBlock(path, w, h));
}
