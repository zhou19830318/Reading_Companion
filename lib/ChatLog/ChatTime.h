#pragma once

#include <cstdint>

namespace ChatLog {

// Wall-clock floor below which the system time is treated as unset (TLS
// certificate validation and history day bucketing both need a real clock).
inline constexpr int64_t MIN_SANE_EPOCH_MS = 1600000000000LL;

int64_t nowEpochMs();

}  // namespace ChatLog
