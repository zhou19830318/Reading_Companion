#include "HalMicrophone.h"

#include <Logging.h>

#ifdef ONEPAGE_C61
#include <Arduino.h>
#include <driver/i2s_common.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>

#include "HalGPIO.h"  // PDM_CLK / PDM_DIN
#endif

HalMicrophone HalMicrophone::instance;

#ifdef ONEPAGE_C61

namespace {
// Raw PDM words pulled per driver read. 256 words = 4096 PDM bits = 64 output
// samples. Must stay small: this buffer and the I2S channel's own DMA ring
// (dma_desc_num * dma_frame_num * 4 B = 1024 B, see begin()) are both
// DMA-capable internal RAM, the scarcest resource on the C61. With WiFi up the
// internal heap's largest free block is only ~3.5-4 KB, and the original
// 4 x 256 ring needed one contiguous 4096 B block, so the microphone could not
// initialise at all (measured: i2s_alloc_dma_desc -> init_std_rx failed: 257).
// The 1024 B ring keeps a single read inside it (512 B = half) with margin.
constexpr size_t RAW_WORDS_PER_READ = 256;

// Read timeout for one driver call. Short enough to keep the caller responsive,
// long enough that a normal chunk always arrives within it (one DMA buffer is
// ~8 ms of audio at 128 KB/s).
constexpr uint32_t READ_TIMEOUT_MS = 200;

constexpr int32_t CIC_HALF = HalMicrophone::CIC_GAIN / 2;  // 50% bit density
constexpr int32_t GAIN_Q15 = 32768;                        // 1.0x, tunable
constexpr int32_t DC_ALPHA_Q15 = 32604;                    // ~0.995 one-pole HP

// Bring-up diagnostics: log the first few driver reads so we can tell "no data"
// (timeout / all-zero words) apart from a filter problem. `ones` reports the PDM
// bit density over the whole read: an idle-but-powered PDM microphone sits near
// 50%, a dead/floating line reads 0% or 100%.
uint32_t gReadLogCount = 0;
}  // namespace

bool HalMicrophone::begin() {
  if (active) {
    return true;
  }

  i2s_chan_handle_t rx = nullptr;
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // 2 x 128 stereo 16-bit frames = 1024 B of DMA ring (~8 ms at 128 KB/s). Sized
  // to fit the internal DMA-capable heap that is left once WiFi is up; see the
  // note on RAW_WORDS_PER_READ above. read() never requests more than half of
  // it, so a full ring can be drained in one i2s_channel_read() call.
  chanCfg.dma_desc_num = 2;
  chanCfg.dma_frame_num = 128;
  chanCfg.auto_clear = true;

  esp_err_t err = i2s_new_channel(&chanCfg, nullptr, &rx);
  if (err != ESP_OK) {
    LOG_ERR("MIC", "i2s_new_channel failed: %d", static_cast<int>(err));
    return false;
  }

  // STD RX instead of the driver's raw-PDM RX mode. Rationale (verified on
  // hardware and in the IDF 5.5 sources): the raw PDM path on C61 never yields
  // a single DMA buffer - i2s_channel_read() times out with 0 bytes - and
  // upstream only runs its PDM *RX* example on the targets that have a
  // PDM->PCM converter (esp32/esp32s3/esp32p4), so the raw path is untested on
  // C61. STD mode is exercised on C61 and does exactly what a PDM microphone
  // needs: a continuous BCLK it can use as its PDM clock, plus one sampled bit
  // per BCLK. Running it in stereo with 16-bit slots means all 32 line bits of
  // a frame are stored consecutively, so the DMA buffer is the raw PDM
  // bitstream in order. WS is not routed: a PDM microphone has no WS pin.
  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_FRAME_RATE_HZ);
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  stdCfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.bclk = static_cast<gpio_num_t>(PDM_CLK);
  stdCfg.gpio_cfg.ws = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.din = static_cast<gpio_num_t>(PDM_DIN);
  stdCfg.gpio_cfg.invert_flags.mclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.bclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.ws_inv = false;

  err = i2s_channel_init_std_mode(rx, &stdCfg);
  if (err != ESP_OK) {
    LOG_ERR("MIC", "init_std_rx failed: %d", static_cast<int>(err));
    i2s_del_channel(rx);
    return false;
  }

  rawBuf = static_cast<uint16_t*>(heap_caps_malloc(RAW_WORDS_PER_READ * sizeof(uint16_t),
                                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (rawBuf == nullptr) {
    LOG_ERR("MIC", "raw buffer alloc failed (%u bytes)", static_cast<unsigned>(RAW_WORDS_PER_READ * sizeof(uint16_t)));
    i2s_del_channel(rx);
    return false;
  }
  rawWords = RAW_WORDS_PER_READ;

  err = i2s_channel_enable(rx);
  if (err != ESP_OK) {
    LOG_ERR("MIC", "i2s_channel_enable failed: %d", static_cast<int>(err));
    heap_caps_free(rawBuf);
    rawBuf = nullptr;
    i2s_del_channel(rx);
    return false;
  }

  for (int i = 0; i < CIC_ORDER; i++) {
    integrator[i] = 0;
    comb[i] = 0;
  }
  decimPhase = 0;
  dcPrevIn = 0;
  dcPrevOut = 0;
  gReadLogCount = 0;

  rxHandle = rx;
  active = true;

  // `bclk_hz` is what the hardware was actually programmed to, so log it rather
  // than the requested value: the microphone's PDM clock is BCLK here, so this
  // number is the capture's real oversampling rate.
  i2s_chan_info_t info = {};
  if (i2s_channel_get_info(rx, &info) == ESP_OK) {
    LOG_INF("MIC", "STD RX up: %lu Hz PCM, decim %lu, bclk %lu Hz, dma buf %lu B",
            static_cast<unsigned long>(SAMPLE_RATE), static_cast<unsigned long>(PDM_DECIMATION),
            static_cast<unsigned long>(info.bclk_hz), static_cast<unsigned long>(info.total_dma_buf_size));
  } else {
    LOG_INF("MIC", "STD RX up: %lu Hz PCM, requested bclk %lu Hz", static_cast<unsigned long>(SAMPLE_RATE),
            static_cast<unsigned long>(PDM_CLOCK_HZ));
  }
  return true;
}

void HalMicrophone::end() {
  if (rxHandle != nullptr) {
    auto* rx = static_cast<i2s_chan_handle_t>(rxHandle);
    i2s_channel_disable(rx);
    i2s_del_channel(rx);
    rxHandle = nullptr;
  }
  if (rawBuf != nullptr) {
    heap_caps_free(rawBuf);
    rawBuf = nullptr;
  }
  // Park the clock line low so the microphone is not clocked while idle (it
  // shares its power rail with the SD card and EPD — see HalGPIO).
  pinMode(PDM_CLK, OUTPUT);
  digitalWrite(PDM_CLK, LOW);
  active = false;
}

size_t HalMicrophone::processBits(const uint16_t* words, size_t wordCount, int16_t* out, size_t maxSamples) {
  constexpr int BITS_PER_WORD = 16;
  size_t written = 0;

  for (size_t w = 0; w < wordCount && written < maxSamples; w++) {
    const uint16_t word = words[w];
    for (int i = 0; i < BITS_PER_WORD; i++) {
      // MSB first: the raw PDM stream is packed high bit first.
      const int32_t bit = (word & (0x8000u >> i)) ? 1 : -1;

      // Integrator section, runs at the PDM bit rate.
      integrator[0] += bit;
      for (int s = 1; s < CIC_ORDER; s++) {
        integrator[s] += integrator[s - 1];
      }

      if (++decimPhase < PDM_DECIMATION) {
        continue;
      }
      decimPhase = 0;

      // Comb section, runs once per output sample (delay = 1 output sample).
      int64_t v = integrator[CIC_ORDER - 1];
      for (int s = 0; s < CIC_ORDER; s++) {
        const int64_t delayed = comb[s];
        comb[s] = v;
        v -= delayed;
      }

      // Normalise: 50% bit density maps to 0, full swing to +/-CIC_GAIN/2.
      const int32_t dev = static_cast<int32_t>(v) - CIC_HALF;
      int32_t sample = static_cast<int32_t>((static_cast<int64_t>(dev) * GAIN_Q15) >> 17);

      // DC blocker: removes the PDM microphone's static bias.
      const int32_t y = sample - dcPrevIn + ((DC_ALPHA_Q15 * dcPrevOut) >> 15);
      dcPrevIn = sample;
      dcPrevOut = y;

      if (y > 32767) {
        sample = 32767;
      } else if (y < -32768) {
        sample = -32768;
      } else {
        sample = y;
      }
      out[written++] = static_cast<int16_t>(sample);
    }
  }
  return written;
}

bool HalMicrophone::read(int16_t* out, size_t maxSamples, size_t& samplesRead) {
  samplesRead = 0;
  if (!active || out == nullptr || maxSamples == 0) {
    return false;
  }

  while (samplesRead < maxSamples) {
    size_t bytesRead = 0;
    const esp_err_t err = i2s_channel_read(static_cast<i2s_chan_handle_t>(rxHandle), rawBuf,
                                           rawWords * sizeof(uint16_t), &bytesRead, READ_TIMEOUT_MS);
    const size_t words = bytesRead / sizeof(uint16_t);
    if (gReadLogCount < 6) {
      gReadLogCount++;
      int32_t ones = 0;
      for (size_t i = 0; i < words; i++) {
        ones += __builtin_popcount(rawBuf[i]);
      }
      LOG_INF("MIC", "read#%u err=%d bytes=%u ones=%d/%u raw=%04x %04x %04x %04x", static_cast<unsigned>(gReadLogCount),
              static_cast<int>(err), static_cast<unsigned>(bytesRead), ones, static_cast<unsigned>(words * 16),
              words > 0 ? rawBuf[0] : 0, words > 1 ? rawBuf[1] : 0, words > 2 ? rawBuf[2] : 0,
              words > 3 ? rawBuf[3] : 0);
    }

    if (err == ESP_ERR_TIMEOUT) {
      break;  // no data in this window; report what we have
    }
    if (err != ESP_OK) {
      LOG_ERR("MIC", "i2s_channel_read failed: %d", static_cast<int>(err));
      return false;
    }
    if (words == 0) {
      break;
    }
    samplesRead += processBits(rawBuf, words, out + samplesRead, maxSamples - samplesRead);
  }
  return true;
}

#else  // !ONEPAGE_C61

bool HalMicrophone::begin() {
  LOG_ERR("MIC", "Microphone is only available on ONEPAGE_C61");
  return false;
}

void HalMicrophone::end() { active = false; }

size_t HalMicrophone::processBits(const uint16_t*, size_t, int16_t*, size_t) { return 0; }

bool HalMicrophone::read(int16_t* out, size_t maxSamples, size_t& samplesRead) {
  (void)out;
  (void)maxSamples;
  samplesRead = 0;
  return false;
}

#endif  // ONEPAGE_C61
