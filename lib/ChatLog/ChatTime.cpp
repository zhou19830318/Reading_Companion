#include "ChatTime.h"

#include <sys/time.h>

namespace ChatLog {

int64_t nowEpochMs() {
  timeval tv = {};
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + static_cast<int64_t>(tv.tv_usec) / 1000;
}

}  // namespace ChatLog
