#include <Arduino.h>
#include <Logging.h>
#include <NetBootstrap.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <sys/time.h>

#include <cstddef>
#include <ctime>

namespace NetBootstrap {
namespace {

// 2020-09-13 — comfortably inside every realistic certificate validity window,
// and far enough in the past that a clock stuck at the epoch fails it.
constexpr time_t MIN_VALID_EPOCH = 1600000000;

// --- TLS allocator override ------------------------------------------------
// pioarduino links prebuilt IDF archives: firmware.map resolves
// esp_mbedtls_mem_calloc to libmbedcrypto.a(esp_mem.c.obj) (built with the
// default INTERNAL policy) and mbedtls_ssl_setup to libmbedtls_2.a, while the
// mbedtls sources this build compiles never reach the link line. So setting
// CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC in platformio.ini cannot change where the
// handshake buffers are allocated, and it does not:
//   post-P2 log: MaxAlloc 4852 B -> mbedtls_ssl_setup -0x7F00
//   (MBEDTLS_ERR_SSL_ALLOC_FAILED, ssl.h:109) -> ESP_ERR_HTTP_CONNECT.
// mbedtls_ssl_setup allocates two ~16.5 KB in/out buffers, which no internal
// block can satisfy while WiFi is up.
//
// MBEDTLS_PLATFORM_MEMORY is enabled (esp_config.h:128) and mbedtls_calloc is
// an indirect call through mbedtls_calloc_func (verified by disassembling the
// prebuilt platform.c.obj), so installing our own pair via
// mbedtls_platform_set_calloc_free() redirects every mbedtls allocation. No
// archive in the link references that setter, so nothing overwrites it —
// mbedtls_platform_setup() is compiled as `return 0`.
//
// PSRAM first, internal as fallback, for every size: this mirrors Espressif's
// own MBEDTLS_EXTERNAL_MEM_ALLOC mode (components/mbedtls/port/esp_mem.c:18),
// which is a supported configuration on SPIRAM devices alongside the hardware
// SHA/ECC accelerators — i.e. mbedtls's own buffers are never handed to the
// DMA engines (the AES DMA path allocates with heap_caps(..., MALLOC_CAP_DMA |
// MALLOC_CAP_INTERNAL) directly, esp_aes_dma_core.c:455). The internal
// fallback only ever sees the request when PSRAM is exhausted, so behaviour
// degrades to today's instead of failing outright.
//
// A size threshold was tried first and is not enough: with WiFi up the
// internal heap sits at MaxAlloc ~3.7 KB / Min Free 483 B, so MPI growth for
// an RSA-2048 signature check failed below any useful threshold and surfaced
// as -0x4290 = MBEDTLS_ERR_RSA_PUBLIC_FAILED (-0x4280)
//              + MBEDTLS_ERR_MPI_ALLOC_FAILED (-0x0010).
void* tlsCalloc(size_t count, size_t size) {
  if (count == 0 || size == 0 || count > SIZE_MAX / size) return nullptr;
  if (void* p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return p;
  return heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void tlsFree(void* ptr) { heap_caps_free(ptr); }

}  // namespace

void ensureTlsAllocator() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
  LOG_INF("NET", "mbedTLS allocator: PSRAM-first");
}

bool systemTimeValid() { return time(nullptr) >= MIN_VALID_EPOCH; }

// esp_crt_bundle rejects certificates whose not-before/not-after dates exclude
// the current time. This board has no RTC and the app's NTP sync is gated on
// halClock.isAvailable() (false on OnePage), so the system clock is at the
// epoch until something sets it — and every HTTPS/wss handshake then fails.
// Sync once, bounded, before TLS. Returns false when the clock still looks
// wrong; the caller reports it.
bool ensureSystemTime() {
  if (systemTimeValid()) return true;

  LOG_INF("NET", "system clock unset, syncing via SNTP");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  constexpr int kAttempts = 50;  // 5 s
  for (int i = 0; i < kAttempts; ++i) {
    if (systemTimeValid()) {
      LOG_INF("NET", "clock set: epoch %lld", static_cast<long long>(time(nullptr)));
      return true;
    }
    delay(100);
  }

  LOG_ERR("NET", "NTP sync timed out — TLS certificate validation will fail");
  return false;
}

bool setSystemTimeFromEpochMs(int64_t epochMs) {
  if (systemTimeValid()) return true;
  if (epochMs < static_cast<int64_t>(MIN_VALID_EPOCH) * 1000) return false;
  timeval tv = {};
  tv.tv_sec = static_cast<time_t>(epochMs / 1000);
  tv.tv_usec = static_cast<suseconds_t>((epochMs % 1000) * 1000);
  if (settimeofday(&tv, nullptr) != 0) {
    LOG_ERR("NET", "settimeofday failed for epoch %lld", static_cast<long long>(tv.tv_sec));
    return false;
  }
  LOG_INF("NET", "clock set from gateway time: epoch %lld", static_cast<long long>(tv.tv_sec));
  return systemTimeValid();
}

}  // namespace NetBootstrap
