#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Microphone capture session: chunked read, running level statistics, and the
// two ways a capture ends — the duration cap and the no-audio guard.
//
// Extracted from MicrophoneTestActivity (docs/voice-openclaw-port-plan.md §8.2)
// so the P0 bring-up screen and the voice screen share one capture path instead
// of two copies drifting apart. The HAL is reached through HalMicrophone, so
// this file is device-only and is deliberately NOT host-compilable — the same
// split as lib/voice/SttClient.
//
// The caller drives it: begin() starts a capture, poll() reads one 100 ms chunk
// per call (so an activity's loop() stays responsive), end() finalises. There is
// no task and no callback here; the activity owns the state machine and decides
// what a finished capture means.
namespace Voice {

class Recorder {
 public:
  // 100 ms at 16 kHz. Read straight into the capture buffer.
  static constexpr size_t CHUNK_SAMPLES = 1600;
  // A capture that has produced nothing at all by now is never going to work:
  // HalMicrophone reports a driver timeout as "zero samples", not as an error,
  // so this is the only thing that catches a dead microphone.
  static constexpr uint32_t SILENT_TIMEOUT_MS = 1500;
  // VAD stop: at least this much speech must be heard before the silence timer
  // may fire, so a cough or door slam does not yield an all-silence capture.
  static constexpr uint32_t VOICED_MIN_MS = 400;

  // Outcome of one poll().
  enum class Event : uint8_t {
    Recording,   // a chunk was read; keep polling
    Complete,    // duration cap, buffer full, or VAD silence stop — pcm() is ready
    NoAudio,     // nothing arrived within SILENT_TIMEOUT_MS
    ReadFailed,  // the driver reported an error
    NotStarted,  // poll() before a successful begin()
  };

  // Why begin() failed. The caller maps this to a translated string; lib/voice
  // does not depend on I18n.
  enum class Error : uint8_t { None, Alloc, Unavailable };

  ~Recorder();

  // Allocates the capture buffer (maxSeconds at sampleRate) and starts the
  // microphone. Returns false on allocation failure or unavailable hardware,
  // leaving error() set. Any previous capture is released first.
  //
  // silenceMs == 0 (the default): fixed-duration capture — ends only at the
  // duration cap or buffer full.
  // silenceMs > 0: VAD stop — the capture ends early once `silenceMs`
  // consecutive milliseconds stay below the speech threshold, provided at
  // least VOICED_MIN_MS of speech was heard first (a capture where nothing at
  // all was said ends the same way; the caller distinguishes via voicedMs()).
  bool begin(uint32_t sampleRate, uint32_t maxSeconds, uint32_t silenceMs = 0);

  // Speech detector for the VAD stop, as a chunk-RMS threshold. 0 disables
  // the VAD path entirely even when silenceMs is set. Must be set before
  // begin(); defaults are tuned against the measured levels of this
  // microphone (speech ≈1600-4000 RMS near the mic, room noise well below).
  void setVadThreshold(int32_t rms) { vadRms_ = rms; }

  // Reads up to CHUNK_SAMPLES and folds them into the statistics. On any end
  // condition the microphone is already stopped when this returns.
  Event poll();

  // Stops the microphone and computes the final rms / dcOffset. Idempotent, and
  // safe to call on a capture that never started.
  void end();

  // end() plus releasing the capture buffer. Call from the activity's onExit().
  void close();

  // Borrowed pointer to the captured PCM; valid between begin() and close().
  const int16_t* pcm() const { return buffer_.get(); }
  size_t samples() const { return samples_; }
  size_t capacity() const { return capacity_; }
  int32_t peak() const { return peak_; }
  int32_t rms() const { return rms_; }
  int32_t dcOffset() const { return dcOffset_; }
  // Wall-clock ms of audio above the VAD threshold (0 when the VAD is off).
  uint32_t voicedMs() const { return voicedMs_; }
  // True once the VAD decided the capture is over (Complete was returned for
  // this reason). False when the end came from the duration cap or buffer.
  bool endedBySilence() const { return endedBySilence_; }
  // Wall-clock time since begin(), 0 once the capture has ended. Bounded by the
  // clock, not the sample count, so a starved capture still advances.
  uint32_t elapsedMs() const;
  bool isActive() const { return active_; }
  Error error() const { return error_; }

 private:
  void updateLevels();

  std::unique_ptr<int16_t[]> buffer_;
  size_t capacity_ = 0;
  size_t samples_ = 0;
  int32_t peak_ = 0;
  // int64 accumulators: a long capture at full scale overflows int32.
  int64_t sumSquares_ = 0;
  int64_t dcSum_ = 0;
  int32_t rms_ = 0;
  int32_t dcOffset_ = 0;
  uint32_t maxMs_ = 0;
  uint32_t silenceMs_ = 0;
  uint32_t silenceRunMs_ = 0;
  uint32_t voicedMs_ = 0;
  int32_t vadRms_ = 0;
  bool endedBySilence_ = false;
  unsigned long startMs_ = 0;
  bool active_ = false;
  Error error_ = Error::None;
};

}  // namespace Voice
