#pragma once

#include <cstddef>
#include <cstdint>

// One-shot text round trip on the relay's /api/v1/chat endpoint: the Ask-AI
// screen's transport when the voice-relay setting is on. The device
// transforms its configured realtime URL (ws/wss://host[:port]/...) into the
// same origin's http(s) chat endpoint, so one NVS configuration serves both
// paths (relay/relay/main.py ChatRequest).
//
// Blocking by design: the Ask-AI screen runs one question at a time from its
// loop and paints a static waiting frame before the call — the same pattern
// as SttClient::transcribe on the voice path. The host-testable halves (request
// bytes, response parsing) live in VoiceRelayCodec.
namespace VoiceRelay {

class ChatClient {
 public:
  enum class Result : uint8_t {
    Ok,
    BadConfig,  // url/token/device missing, request did not fit, or 404 (endpoint disabled)
    NoWifi,
    Time,  // TLS needs a sane clock and SNTP did not settle
    Connect,
    Http,      // non-200 with no more specific class
    Auth,      // 401 or an UNAUTHORIZED / DEVICE_NOT_ALLOWED body
    Upstream,  // 502 / provider-class failure (LLM_*, overload, rate limit)
    Read,
    Oom,
    NoText,  // 200 without a text field
  };

  struct Stats {
    int httpStatus = 0;
    size_t responseBytes = 0;
  };

  // `url` is the configured relay URL (ws:// or wss://). On Ok, `reply` holds
  // the NUL-terminated reply text; `truncated` is set when it did not fit
  // replyCap (cut at a codepoint boundary) or the body outgrew the read
  // buffer. `mode` selects a server-side preset (nullptr = plain ask,
  // "dict" = dictionary entry). `stats` is optional.
  static Result ask(const char* url, const char* token, const char* deviceId, const char* text, char* reply,
                    size_t replyCap, bool& truncated, const char* mode = nullptr, Stats* stats = nullptr);

  static const char* resultName(Result result);
};

}  // namespace VoiceRelay
