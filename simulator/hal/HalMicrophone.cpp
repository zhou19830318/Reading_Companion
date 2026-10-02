#include "HalMicrophone.h"

#include <Logging.h>

// Simulator back-end for the microphone HAL. The real implementation lives in
// lib/hal/HalMicrophone.cpp and is excluded from the simulator build (see
// simulator/CMakeLists.txt), which links this file instead. There is no host
// audio capture in the simulator, so the microphone always reports unavailable
// and callers fall back to their "no microphone" path.

HalMicrophone HalMicrophone::instance;

bool HalMicrophone::begin() {
  LOG_INF("MIC", "Simulator: microphone not available");
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
