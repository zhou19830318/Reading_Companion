#include "Recorder.h"

#include <Arduino.h>
#include <HalMicrophone.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>

namespace Voice {

Recorder::~Recorder() { close(); }

bool Recorder::begin(uint32_t sampleRate, uint32_t maxSeconds, uint32_t silenceMs) {
  close();
  maxMs_ = maxSeconds * 1000UL;
  silenceMs_ = (vadRms_ > 0) ? silenceMs : 0;
  capacity_ = static_cast<size_t>(sampleRate) * maxSeconds;

  buffer_ = makeUniqueNoThrow<int16_t[]>(capacity_);
  if (!buffer_) {
    LOG_ERR("REC", "OOM: %u bytes capture buffer", static_cast<unsigned>(capacity_ * sizeof(int16_t)));
    capacity_ = 0;
    error_ = Error::Alloc;
    return false;
  }

  if (!MIC.begin()) {
    buffer_.reset();
    capacity_ = 0;
    error_ = Error::Unavailable;
    return false;
  }

  samples_ = 0;
  peak_ = 0;
  sumSquares_ = 0;
  dcSum_ = 0;
  rms_ = 0;
  dcOffset_ = 0;
  silenceRunMs_ = 0;
  voicedMs_ = 0;
  endedBySilence_ = false;
  startMs_ = millis();
  active_ = true;
  error_ = Error::None;
  LOG_INF("REC", "recording %u s (%u samples @ %u Hz, vad %s)", static_cast<unsigned>(maxSeconds),
          static_cast<unsigned>(capacity_), static_cast<unsigned>(sampleRate),
          silenceMs_ > 0 ? "on" : "off");
  return true;
}

Recorder::Event Recorder::poll() {
  if (!active_) return Event::NotStarted;

  // The capture is bounded by wall-clock time, not by the sample count: a
  // microphone delivering fewer samples than expected must still finish.
  if (millis() - startMs_ >= maxMs_) {
    end();
    return Event::Complete;
  }

  const size_t want = std::min(CHUNK_SAMPLES, capacity_ - samples_);
  size_t got = 0;
  if (!MIC.read(buffer_.get() + samples_, want, got)) {
    end();
    return Event::ReadFailed;
  }

  for (size_t i = 0; i < got; i++) {
    const int32_t s = buffer_[samples_ + i];
    const int32_t a = s < 0 ? -s : s;
    if (a > peak_) {
      peak_ = a;
    }
    sumSquares_ += static_cast<int64_t>(s) * s;
    dcSum_ += s;
  }
  samples_ += got;
  updateLevels();

  // VAD stop: silence after speech ends the capture early. Timed in *audio*
  // time, not wall-clock: this chunk's own rms decides which side of the
  // threshold it is on, and each chunk covers an exact slice of audio
  // (CHUNK_SAMPLES at the sample rate). A 1.5 s stop is therefore exact, and
  // a stalled read cannot inflate the counters.
  if (silenceMs_ > 0) {
    static constexpr uint32_t chunkMs = (CHUNK_SAMPLES * 1000UL) / 16000;  // 100 ms at 16 kHz
    if (rms_ >= vadRms_) {
      voicedMs_ += chunkMs;
      silenceRunMs_ = 0;
    } else {
      silenceRunMs_ += chunkMs;
    }
    if (voicedMs_ >= VOICED_MIN_MS && silenceRunMs_ >= silenceMs_) {
      endedBySilence_ = true;
      end();
      return Event::Complete;
    }
  }

  // Nothing at all from the RX path by now means the capture cannot succeed.
  if (samples_ == 0 && millis() - startMs_ >= SILENT_TIMEOUT_MS) {
    end();
    return Event::NoAudio;
  }
  if (samples_ >= capacity_) {
    end();
    return Event::Complete;
  }
  return Event::Recording;
}

void Recorder::end() {
  if (active_) {
    MIC.end();
    active_ = false;
  }
  // Recomputed here as well as per chunk so a capture ended early still reports
  // its levels, and so end() is meaningful on an untouched Recorder.
  updateLevels();
  dcOffset_ = samples_ > 0 ? static_cast<int32_t>(dcSum_ / static_cast<int64_t>(samples_)) : 0;
}

void Recorder::close() {
  end();
  buffer_.reset();
  capacity_ = 0;
  samples_ = 0;
  peak_ = 0;
  sumSquares_ = 0;
  dcSum_ = 0;
  rms_ = 0;
  dcOffset_ = 0;
  silenceRunMs_ = 0;
  voicedMs_ = 0;
  endedBySilence_ = false;
  error_ = Error::None;
}

uint32_t Recorder::elapsedMs() const { return active_ ? static_cast<uint32_t>(millis() - startMs_) : 0; }

void Recorder::updateLevels() {
  rms_ = samples_ > 0 ? static_cast<int32_t>(sqrtf(static_cast<float>(sumSquares_) / static_cast<float>(samples_))) : 0;
}

}  // namespace Voice
