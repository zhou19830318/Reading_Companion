#pragma once

// esp_heap_caps.h exists only in the ESP-IDF toolchain; the native host tests
// (test/CMakeLists.txt) compile this header too, so probe for it instead of
// keying off ESP_PLATFORM, which PlatformIO's Arduino framework does not
// guarantee to define.
#if defined(__has_include)
#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#define CROSSPOINT_HAVE_HEAP_CAPS 1
#endif
#endif

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// Nothrow versions of std::make_unique. Return nullptr on allocation failure
// instead of calling abort() (the default when exceptions are disabled on ESP32).
//
// Single object:
//   auto obj = makeUniqueNoThrow<PNG>();
//   if (!obj) { LOG_ERR("TAG", "OOM"); return false; }
//
// Array:
//   auto buf = makeUniqueNoThrow<uint8_t[]>(size);
//   if (!buf) { LOG_ERR("TAG", "OOM"); return false; }
//   buf[0] = 0xFF;
//   someApi(buf.get(), size);
//

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  using Elem = std::remove_extent_t<T>;
  return std::unique_ptr<T>(new (std::nothrow) Elem[count]());
}

// Helper struct to call a cleanup function on exit from any scope.
// Use with a lambda to avoid unnecessary allocations from std::function/std::bind:
// Example:
//   auto jpeg = makeUniqueNoThrow<JPEGDEC>();
//   ScopedCleanup cleanup{[&jpeg]{ jpeg->close(); }};
//
template <typename F>
struct [[nodiscard]] ScopedCleanup final {
  const F fn;
  explicit ScopedCleanup(F f) : fn{std::move(f)} {}
  ScopedCleanup(const ScopedCleanup&) = delete;
  ScopedCleanup& operator=(const ScopedCleanup&) = delete;
  ScopedCleanup(ScopedCleanup&&) = delete;
  ScopedCleanup& operator=(ScopedCleanup&&) = delete;
  ~ScopedCleanup() { fn(); }
};

template <typename F>
ScopedCleanup(F) -> ScopedCleanup<F>;

// std::allocator-compatible allocator that asks for PSRAM first.
//
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 pins every plain malloc()/new of
// ≤ 4096 B to internal DRAM, and WiFi + LWIP leave only ~6 KB of that heap
// free (plan §8, rule R2). Fixed working buffers (upload staging, glyph
// tables) are bulk CPU-read data — never handed to a DMA peripheral — so they
// belong in PSRAM:
//   std::vector<uint8_t, PsramAllocator<uint8_t>> buf(UPLOAD_SIZE);
// Falls back to internal DRAM when PSRAM is exhausted; deallocate() releases
// either because heap_caps_free() accepts any capability-tagged block.
// ESP-IDF toolchain only (see CROSSPOINT_HAVE_HEAP_CAPS above).
#if defined(CROSSPOINT_HAVE_HEAP_CAPS)
template <typename T>
struct PsramAllocator {
  using value_type = T;
  PsramAllocator() noexcept = default;
  template <typename U>
  PsramAllocator(const PsramAllocator<U>&) noexcept {}
  T* allocate(const std::size_t n) {
    if (n == 0) return nullptr;
    if (void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM)) return static_cast<T*>(p);
    return static_cast<T*>(std::malloc(n * sizeof(T)));
  }
  void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }
};

template <typename T, typename U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept {
  return true;
}
template <typename T, typename U>
bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept {
  return false;
}
#endif  // CROSSPOINT_HAVE_HEAP_CAPS
