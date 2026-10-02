#pragma once

#include <cstddef>
#include <cstdint>

// Microphone capture HAL (OnePage / ESP32-C61 only).
//
// WHY THIS LOOKS THE WAY IT DOES
// ------------------------------
// The OnePage board's microphone is a single-line PDM part wired to
// PDM_CLK (GPIO7) / PDM_DIN (GPIO3) — see lib/hal/HalGPIO.h. The ESP32-C61 has
// no hardware PDM->PCM converter (soc_caps for esp32c61 defines
// SOC_I2S_SUPPORTS_PDM_RX but NOT SOC_I2S_SUPPORTS_PDM2PCM), so the raw PDM
// bitstream has to be decimated to PCM in software:
//
//   raw PDM bits -> CIC decimator (anti-alias + decimate) -> DC blocker -> int16
//
// The bitstream itself is captured with the I2S peripheral in plain STD RX
// mode, using BCLK as the microphone's PDM clock — see HalMicrophone.cpp for
// why the driver's dedicated "raw PDM RX" mode is not used on this target.
//
// Output format: 16 kHz, mono, signed 16-bit PCM — what the cloud STT client
// expects. No heap is touched by read(); all filter state is member storage and
// the raw staging buffer is allocated once by begin().
//
// On non-C61 targets and in the simulator, begin() fails and read() reports
// zero samples; callers must treat the microphone as optional.
class HalMicrophone {
 public:
  static constexpr uint32_t SAMPLE_RATE = 16000;
  // PDM clock = SAMPLE_RATE * PDM_DECIMATION. 64x oversampling is the standard
  // ratio that yields 16 kHz from a ~1.024 MHz PDM microphone.
  static constexpr uint32_t PDM_DECIMATION = 64;
  static constexpr uint32_t PDM_CLOCK_HZ = SAMPLE_RATE * PDM_DECIMATION;

  // I2S STD-mode frame the capture runs in. The RX is deliberately STEREO with
  // 16-bit slots so that all 32 line bits of a frame (2 slots x 16 bits) are
  // stored back-to-back in the DMA buffer, i.e. the buffer *is* the raw PDM
  // bitstream. BCLK is derived by the driver as:
  //   BCLK = I2S_FRAME_RATE_HZ * I2S_SLOT_COUNT * I2S_SLOT_BITS
  // so the frame rate below is chosen to make BCLK equal PDM_CLOCK_HZ.
  static constexpr uint32_t I2S_SLOT_COUNT = 2;
  static constexpr uint32_t I2S_SLOT_BITS = 16;
  static constexpr uint32_t I2S_FRAME_RATE_HZ = PDM_CLOCK_HZ / (I2S_SLOT_COUNT * I2S_SLOT_BITS);

  // CIC order. 3 gives a usable anti-alias with a tiny cost.
  static constexpr int CIC_ORDER = 3;
  // CIC DC gain = PDM_DECIMATION ^ CIC_ORDER (64^3 = 262144), used to normalise.
  static constexpr int32_t CIC_GAIN = [] {
    int32_t g = 1;
    for (int i = 0; i < CIC_ORDER; i++) {
      g *= static_cast<int32_t>(PDM_DECIMATION);
    }
    return g;
  }();

  // Create and start the I2S PDM RX channel. Idempotent; returns false if the
  // hardware is unavailable or initialisation fails.
  bool begin();
  // Stop and release the channel. Safe to call when not started.
  void end();

  bool isActive() const { return active; }

  // Blocking read of up to maxSamples 16-bit mono PCM samples into `out`.
  // `samplesRead` is set to the number produced (0 on timeout). Returns false
  // only on a hard driver error.
  bool read(int16_t* out, size_t maxSamples, size_t& samplesRead);

  static HalMicrophone& getInstance() { return instance; }

 private:
  HalMicrophone() = default;
  HalMicrophone(const HalMicrophone&) = delete;
  HalMicrophone& operator=(const HalMicrophone&) = delete;

  static HalMicrophone instance;

  bool active = false;
  void* rxHandle = nullptr;  // i2s_chan_handle_t; kept opaque so this header stays IDF-free

  // Raw-PDM staging buffer. DMA-capable internal RAM, allocated once in begin().
  uint16_t* rawBuf = nullptr;
  size_t rawWords = 0;

  // CIC decimator state (int64 keeps a long run of identical bits from
  // overflowing the integrator section).
  int64_t integrator[CIC_ORDER] = {};
  int64_t comb[CIC_ORDER] = {};
  uint32_t decimPhase = 0;

  // One-pole DC blocker (high-pass) state, Q15 fixed point.
  int32_t dcPrevIn = 0;
  int32_t dcPrevOut = 0;

  // Software PDM->PCM. Consumes up to `wordCount` raw 16-bit PDM words and
  // writes at most `maxSamples` PCM samples; returns the number written.
  size_t processBits(const uint16_t* words, size_t wordCount, int16_t* out, size_t maxSamples);
};

// Helper macro, mirrors the other HAL singletons (Storage, GUI, ...).
#define MIC HalMicrophone::getInstance()
