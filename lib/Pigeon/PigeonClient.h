#pragma once

#include <cstddef>
#include <cstdint>

// HTTP transport for the relay's pigeon endpoints. Same origin derivation
// (ws:// URL -> http:// origin) and auth headers (bearer + X-Device-Id) as
// LearningClient/PomodoroClient: every call is blocking and the caller paints
// a waiting frame first.
//
// The relay is the source of truth for a conversation. Phase 1 keeps no
// device-side message archive; fetchHistory() rehydrates the screen on entry
// and fetchInbox() polls the tail while it is open. The relay retains seven
// days / 100 messages per recipient, which is enough for this screen.
namespace Pigeon {

constexpr size_t DEVICE_ID_CAP = 40;
constexpr size_t DEVICE_NAME_CAP = 32;
constexpr size_t TEXT_CAP = 512;
constexpr size_t CREATED_AT_CAP = 32;
constexpr size_t MAX_DEVICES = 8;
constexpr size_t MAX_MESSAGES = 20;

struct Device {
  char id[DEVICE_ID_CAP] = {};
  char name[DEVICE_NAME_CAP] = {};
  bool online = false;
};

struct Message {
  uint32_t id = 0;
  char peerId[DEVICE_ID_CAP] = {};
  char text[TEXT_CAP + 1] = {};
  char createdAt[CREATED_AT_CAP] = {};
  bool outgoing = false;
  bool read = true;
};

class PigeonClient {
 public:
  enum class Result : uint8_t {
    Ok,
    BadConfig,  // url/token/device missing, request did not fit, or 400/404
    NoWifi,
    Time,  // TLS needs a sane clock and SNTP did not settle
    Connect,
    Http,         // non-2xx with no more specific class
    Auth,         // 401/403
    RateLimited,  // 429
    Upstream,     // 5xx
    Read,
    Oom,
    NoData,  // 2xx without the expected fields
  };

  // POST /api/v1/pigeon/heartbeat. Fills at most `cap` peers; `count` is the
  // number written. `unread` is the relay's unread total for this device.
  static Result heartbeat(const char* url, const char* token, const char* deviceId, const char* name, Device* out,
                          size_t cap, size_t& count, int& unread);

  // POST /api/v1/pigeon/send. `clientEventId` is the idempotency key; the
  // caller should reuse it when retrying. Fills `out` on Ok.
  static Result sendText(const char* url, const char* token, const char* deviceId, const char* to, const char* text,
                         const char* clientEventId, Message& out);

  // GET /api/v1/pigeon/inbox?after=<afterId>&limit=<cap>. Messages are in
  // ascending id order. `latestId` is the highest id the relay has for this
  // device, even when the page is empty.
  static Result fetchInbox(const char* url, const char* token, const char* deviceId, uint32_t afterId, Message* out,
                           size_t cap, size_t& count, uint32_t& latestId, int& unread);

  // GET /api/v1/pigeon/history?peer=<peerId>&limit=<cap>. Both directions,
  // oldest first.
  static Result fetchHistory(const char* url, const char* token, const char* deviceId, const char* peerId, Message* out,
                             size_t cap, size_t& count);

  // POST /api/v1/pigeon/ack. Marks the given relay message ids read.
  static Result ack(const char* url, const char* token, const char* deviceId, const uint32_t* ids, size_t count);

  // POST /api/v1/pigeon/clear?peer=<peerId>. Deletes both directions of the
  // conversation from the relay. Idempotent; `ok` is the only response field
  // the device needs.
  static Result clearHistory(const char* url, const char* token, const char* deviceId, const char* peerId);

  static const char* resultName(Result result);
};

}  // namespace Pigeon
