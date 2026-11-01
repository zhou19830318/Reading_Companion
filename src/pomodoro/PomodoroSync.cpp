#include "PomodoroSync.h"

#include <CloudConfig.h>
#include <CrossPointSettings.h>
#include <Logging.h>
#include <NetBootstrap.h>
#include <PomodoroClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>

#include "PomodoroSession.h"

namespace {

constexpr size_t MAX_BATCH = PomodoroSession::QUEUE_CAP;

// One record's string storage. PomodoroSession keeps epochs (compact, clock
// agnostic); the relay wants ISO-8601, which is formatted at flush time.
struct RecordStrings {
  char clientEventId[VoiceRelay::POMODORO_EVENT_ID_CAP] = {};
  char kind[16] = {};
  char startedAt[VoiceRelay::POMODORO_TIMESTAMP_CAP + 1] = {};
  char endedAt[VoiceRelay::POMODORO_TIMESTAMP_CAP + 1] = {};
};

template <typename T>
struct HeapCapsDeleter {
  void operator()(T* p) const { heap_caps_free(p); }
};

template <typename T>
using HeapArray = std::unique_ptr<T[], HeapCapsDeleter<T>>;

// PSRAM first: the batch lives across the TLS handshake, which dips the
// internal heap hard.
template <typename T>
HeapArray<T> allocBatchArray(const size_t count) {
  T* p = static_cast<T*>(heap_caps_malloc(sizeof(T) * count, MALLOC_CAP_SPIRAM));
  if (p == nullptr) p = static_cast<T*>(heap_caps_malloc(sizeof(T) * count, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return HeapArray<T>(p);
}

const char* kindName(const uint8_t kind) {
  switch (kind) {
    case 0:
      return "work";
    case 1:
      return "break";
    case 2:
      return "long_break";
    default:
      return nullptr;
  }
}

void formatIso(const uint32_t epoch, char* out, const size_t cap) {
  const time_t value = static_cast<time_t>(epoch);
  struct tm parts;
  gmtime_r(&value, &parts);
  strftime(out, cap, "%Y-%m-%dT%H:%M:%S", &parts);
}

}  // namespace

namespace PomodoroSync {

bool flushIfOnline() {
  const size_t pending = POMODORO.pendingCount();
  if (pending == 0) return false;
  if (!WiFi.isConnected()) return false;

  CLOUD_CFG.load();
  if (SETTINGS.voiceRelayEnabled == 0 || CLOUD_CFG.relayUrl[0] == '\0' || CLOUD_CFG.relayToken[0] == '\0' ||
      CLOUD_CFG.relayDeviceId[0] == '\0') {
    return false;
  }
  // The relay groups stats by the startedAt date, so a record with no clock
  // cannot be placed. Try once (bounded); failure keeps everything queued.
  if (!NetBootstrap::systemTimeValid() && !NetBootstrap::ensureSystemTime()) {
    LOG_INF("POMO", "sync deferred: clock unset");
    return false;
  }

  size_t batch = pending < MAX_BATCH ? pending : MAX_BATCH;
  auto strings = allocBatchArray<RecordStrings>(batch);
  auto wire = allocBatchArray<VoiceRelay::PomodoroSessionWire>(batch);
  if (!strings || !wire) {
    LOG_ERR("POMO", "sync OOM: %u records", static_cast<unsigned>(batch));
    return false;
  }

  // A record captured before the clock was set has no date of its own; the
  // flush time is the best approximation (the session ended within the last
  // cycle, so the error is at most the queue's age). Both ends are backdated
  // by the recorded duration so the interval stays plausible.
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  size_t count = 0;
  for (size_t i = 0; i < batch; ++i) {
    const auto* record = POMODORO.pendingAt(i);
    if (record == nullptr) break;
    const char* kind = kindName(record->kind);
    if (kind == nullptr) break;
    uint32_t started = record->startedAtEpoch;
    uint32_t ended = record->endedAtEpoch;
    if (started == 0 || ended == 0 || ended < started) {
      ended = now;
      const uint32_t duration = static_cast<uint32_t>(record->minutes) * 60U;
      started = now > duration ? now - duration : now;
    }
    RecordStrings& text = strings[count];
    snprintf(text.clientEventId, sizeof(text.clientEventId), "%s", record->clientEventId);
    snprintf(text.kind, sizeof(text.kind), "%s", kind);
    formatIso(started, text.startedAt, sizeof(text.startedAt));
    formatIso(ended, text.endedAt, sizeof(text.endedAt));
    wire[count] = {text.clientEventId, text.kind, text.startedAt, text.endedAt, record->minutes, record->completed};
    count++;
  }
  if (count == 0) return false;

  const auto result = VoiceRelay::PomodoroClient::uploadSessions(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken,
                                                                 CLOUD_CFG.relayDeviceId, wire.get(), count);
  if (result != VoiceRelay::PomodoroClient::Result::Ok) {
    LOG_INF("POMO", "sync failed (%s), %u records kept", VoiceRelay::PomodoroClient::resultName(result),
            static_cast<unsigned>(count));
    return true;
  }
  POMODORO.clearPending(count);
  LOG_INF("POMO", "synced %u sessions, %u left", static_cast<unsigned>(count),
          static_cast<unsigned>(POMODORO.pendingCount()));
  return true;
}

}  // namespace PomodoroSync
