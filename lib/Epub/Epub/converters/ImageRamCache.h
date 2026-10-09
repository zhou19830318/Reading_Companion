#pragma once

#include <Logging.h>
#include <esp_heap_caps.h>
#include <stdint.h>

#include <cstring>
#include <string>

// PSRAM-backed store for fully decoded 2-bit packed images, keyed by the .pxc
// path ImageBlock derives from the image path.
//
// Why this exists: before it, a cold JPEG decode streamed the packed image to
// the SD card row-band by row-band (PixelCache) and every subsequent render
// pass streamed it back off the card — measured at 96% of cold decode time and
// ~275 ms per warm pass, because the card only sustains ~11 KB/s of writes and
// ~40 ms per read. The packed image for a full-page comic panel is 34-84 KB
// (width*height/4), and the ESP32-C61 has 8 MB of PSRAM with ~7.5 MB free, so
// the whole thing fits with room to spare. Holding it in PSRAM removes the SD
// round trip entirely: decode stops writing mid-stream and the ~14 grayscale
// strip passes read memory instead of the card.
//
// Memory: entries are allocated with MALLOC_CAP_SPIRAM only, so nothing here
// competes with the internal ~380 KB DRAM budget used by the decoder, the
// framebuffer and the stack. At most kMaxEntries are live, each capped at
// kMaxImageBytes (a full-page 482x728 image is ~86 KB), and eviction is LRU.
// The registry itself is a static POD array (no std::string, no allocation):
// keys are compared as 64-bit FNV-1a hashes of the cache path, so the whole
// table costs a few hundred bytes of DRAM and never fragments.
namespace ImageRamCache {

struct Entry {
  uint64_t keyHash = 0;
  uint8_t* pixels = nullptr;
  size_t size = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t bytesPerRow = 0;
  uint32_t stamp = 0;
  bool ready = false;
};

static constexpr int kMaxEntries = 8;
static constexpr size_t kMaxImageBytes = 256 * 1024;

inline Entry* slots() {
  static Entry table[kMaxEntries];
  return table;
}

inline uint32_t& tick() {
  static uint32_t t = 0;
  return t;
}

// Set when a PSRAM allocation actually failed (as opposed to the image simply
// being over kMaxImageBytes). While set, readers fall back to the .pxc file
// instead of re-decoding into a slot that cannot be created.
inline bool& allocFailed() {
  static bool failed = false;
  return failed;
}

// True when an image of these dimensions should be served from PSRAM rather
// than from an on-disk .pxc. Decoding costs ~0.5 s once and is then free on
// every strip pass; streaming the .pxc back costs ~300 ms per pass across
// ~15 passes per render.
inline bool psramUsable(int width, int height) {
  if (width <= 0 || height <= 0) return false;
  const size_t bytes = (size_t)((width + 3) / 4) * (size_t)height;
  return bytes <= kMaxImageBytes && !allocFailed();
}

// FNV-1a over the cache path. Collisions would make one image render another's
// pixels; with a 64-bit hash over a handful of live keys that is negligible.
inline uint64_t hashKey(const std::string& key) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < key.size(); i++) {
    h ^= static_cast<uint8_t>(key[i]);
    h *= 1099511628211ULL;
  }
  return h;
}

// Returns the complete packed image for `key`, or nullptr. Touches the LRU
// stamp so the entry survives until it stops being rendered.
inline Entry* find(const std::string& key) {
  const uint64_t h = hashKey(key);
  for (int i = 0; i < kMaxEntries; i++) {
    Entry& e = slots()[i];
    if (e.pixels && e.ready && e.keyHash == h) {
      e.stamp = ++tick();
      return &e;
    }
  }
  return nullptr;
}

// Claims a slot holding `bytes` bytes for `key` and returns it with `pixels`
// zeroed, or nullptr when the image is too big for PSRAM or PSRAM is out.
// The entry is not visible to find() until setReady() — the caller fills the
// buffer and only then publishes it.
inline Entry* reserve(const std::string& key, int width, int height, int bytesPerRow) {
  const size_t bytes = (size_t)bytesPerRow * (size_t)height;
  if (bytes == 0 || bytes > kMaxImageBytes) return nullptr;

  const uint64_t h = hashKey(key);
  Entry* slot = nullptr;
  for (int i = 0; i < kMaxEntries && !slot; i++) {
    Entry& e = slots()[i];
    if (e.pixels && e.keyHash == h) slot = &e;  // same key: re-reserve after abort
  }
  for (int i = 0; i < kMaxEntries && !slot; i++) {
    if (!slots()[i].pixels) slot = &slots()[i];
  }
  if (!slot) {
    Entry* lru = &slots()[0];
    for (int i = 1; i < kMaxEntries; i++) {
      if (slots()[i].stamp < lru->stamp) lru = &slots()[i];
    }
    slot = lru;
    if (slot->pixels) {
      LOG_DBG("IMG", "PSRAM cache evict (%u bytes)", (unsigned)slot->size);
      heap_caps_free(slot->pixels);
      slot->pixels = nullptr;
      slot->ready = false;
    }
  }

  if (slot->pixels && slot->size != bytes) {
    heap_caps_free(slot->pixels);
    slot->pixels = nullptr;
    slot->ready = false;
  }
  if (!slot->pixels) {
    slot->pixels = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (!slot->pixels) {
      allocFailed() = true;
      LOG_ERR("IMG", "PSRAM cache alloc failed (%u bytes)", (unsigned)bytes);
      return nullptr;
    }
    slot->size = bytes;
  }
  allocFailed() = false;

  memset(slot->pixels, 0, slot->size);
  slot->keyHash = h;
  slot->width = (uint16_t)width;
  slot->height = (uint16_t)height;
  slot->bytesPerRow = (uint16_t)bytesPerRow;
  slot->stamp = ++tick();
  slot->ready = false;
  return slot;
}

inline void setReady(Entry* e) {
  if (e) e->ready = true;
}

// Drops the entry's pixels (the decode failed or the cache was abandoned).
inline void release(Entry* e) {
  if (!e || !e->pixels) return;
  heap_caps_free(e->pixels);
  e->pixels = nullptr;
  e->size = 0;
  e->ready = false;
  e->keyHash = 0;
}

inline void clear() {
  for (int i = 0; i < kMaxEntries; i++) release(&slots()[i]);
}

}  // namespace ImageRamCache
