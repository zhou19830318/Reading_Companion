#pragma once

#include <cstddef>
#include <cstdint>

#include "VoiceRelayCodec.h"  // PomodoroSessionWire, POMODORO_MAX_SESSION_BATCH

// /api/v1/pomodoro session sync on the relay: uploads the records the focus
// timer queues when a work/break phase completes. Same origin derivation
// (ws:// URL -> http:// origin), auth headers (bearer + X-Device-Id) and
// blocking contract as LearningClient — the caller paints the alert first.
// Request bytes and response parsing live in the host-tested codec
// (test/pomodoro_codec). The caller owns the queue and drops the uploaded
// entries after Ok (idempotent server-side, so a lost ack only re-sends).
namespace VoiceRelay {

class PomodoroClient {
 public:
  enum class Result : uint8_t {
    Ok,
    BadConfig,  // url/token/device missing, request did not fit, or 400/404
    NoWifi,
    Time,  // timestamps need a sane clock and SNTP did not settle
    Connect,
    Http,      // non-200 with no more specific class
    Auth,      // 401 or an UNAUTHORIZED / DEVICE_NOT_ALLOWED body
    Upstream,  // 5xx or a server-overload class
    Read,
    Oom,
    NoData,  // 200 without an ok field
  };

  struct Stats {
    int httpStatus = 0;
    size_t responseBytes = 0;
  };

  // POST <origin>/api/v1/pomodoro/sessions with one batch (count <=
  // POMODORO_MAX_SESSION_BATCH). Returns Ok when the relay acknowledged the
  // batch, including an idempotent retry (stored may be lower than accepted).
  static Result uploadSessions(const char* url, const char* token, const char* deviceId,
                               const PomodoroSessionWire* sessions, size_t count, Stats* stats = nullptr);

  static const char* resultName(Result result);
};

}  // namespace VoiceRelay
