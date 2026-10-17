#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>

// Voice Relay wire codec — the pure-C++ half of the realtime voice path.
//
// One WebSocket carries JSON control frames both ways plus raw binary PCM
// frames from the device. This header owns the exact bytes of the control
// protocol (docs/Reading_Companion_realtime_agent .md section 4), so the host
// test suite in test/voice_relay_codec can exercise them without an ESP32.
// The device-only transport (WebSocketsClient) lives in VoiceRelayClient.
//
//   device -> relay: session.start, audio.start, <binary PCM>, audio.end,
//                    session.cancel
//   relay -> device: session.started, asr.partial, asr.final, llm.start,
//                    llm.delta, llm.final, session.done, error
//
// PCM contract: 16000 Hz, mono, signed 16-bit little-endian, 40 ms frames.
namespace VoiceRelay {

// ── audio contract ───────────────────────────────────────────────────────

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint8_t CHANNELS = 1;
constexpr const char* AUDIO_FORMAT = "pcm_s16le";
constexpr size_t FRAME_MS = 40;
// 16000 samples/s x 0.04 s x 2 bytes = 1280.
constexpr size_t FRAME_BYTES = SAMPLE_RATE * FRAME_MS / 1000 * 2;
static_assert(FRAME_BYTES == 1280, "frame contract drifted from the server");

// The relay caps one session's audio at 30 s (MAX_AUDIO_SECONDS); the device
// must end the capture first so the server never sees an over-long stream.
constexpr uint32_t MAX_AUDIO_MS = 30000;

constexpr size_t REQUEST_ID_CHARS = 32;  // 32 lowercase hex digits
constexpr size_t REQUEST_ID_CAP = REQUEST_ID_CHARS + 1;
constexpr size_t SESSION_ID_CAP = REQUEST_ID_CAP;
constexpr size_t DEVICE_ID_CAP = 40;  // "reading-companion-001" + margin

// Largest control frame the device builds. Worst case is session.start with a
// full 32-char requestId and a DEVICE_ID_CAP-1 device id: 215 bytes + NUL.
// The client's FRAME_CAP must be at least this (at 192 every relay round failed
// the frame build and silently fell back to the legacy pipeline).
constexpr size_t CONTROL_FRAME_CAP = 256;

// ASR partial/final and LLM delta/final text. Mirrors SttCodec::TEXT_CAPACITY;
// the relay caps replies at MAX_REPLY_CHARS (500), so a longer value means the
// relay is misconfigured and the text is flagged truncated instead of dropped.
constexpr size_t TEXT_CAP = 512;
// Error events carry a short fixed Chinese line, never upstream detail.
constexpr size_t ERROR_MESSAGE_CAP = 64;

// ── relay -> device events ───────────────────────────────────────────────

enum class EventType : uint8_t {
  Unknown,
  SessionStarted,
  AsrPartial,
  AsrFinal,
  LlmStart,
  LlmDelta,
  LlmFinal,
  SessionDone,
  Error,
};

// Mirrors relay/protocol.py ErrorCode. The activity maps these to translated
// strings (this library does not depend on I18n).
enum class ErrorCode : uint8_t {
  None,
  BadProtocol,
  Unauthorized,
  DeviceNotAllowed,
  AudioFormatUnsupported,
  AudioTimeout,
  AsrConnectionError,
  AsrAuthError,
  AsrRateLimited,
  LlmConnectionError,
  LlmAuthError,
  LlmRateLimited,
  SessionTimeout,
  ServerOverloaded,
  InternalError,
  Unknown,
};

const char* eventTypeName(EventType type);
const char* errorCodeName(ErrorCode code);
// Retry policy from docs section 8: false for auth/config errors, true for
// the network-class failures the server marks retryable.
bool errorRetryable(ErrorCode code);

// Server-side stage timings from session.done ("latencyMs"); -1 = not present.
struct Latency {
  int32_t connect = -1;
  int32_t asrFirstPartial = -1;
  int32_t asrFinal = -1;
  int32_t llmFirstToken = -1;
  int32_t llmFinal = -1;
  int32_t visibleTotal = -1;
};

struct Event {
  EventType type = EventType::Unknown;
  ErrorCode code = ErrorCode::None;
  bool retryable = false;
  // text did not fit TEXT_CAP and was cut at a codepoint-safe boundary.
  bool truncated = false;
  uint16_t seq = 0;
  char requestId[REQUEST_ID_CAP] = {};
  char sessionId[SESSION_ID_CAP] = {};
  char text[TEXT_CAP + 1] = {};
  char message[ERROR_MESSAGE_CAP] = {};
  Latency latency;
};

// One complete inbound text frame from the WebSocket → one Event.
//
// Frames are whole WS text messages, so this is a single-shot parse: reset,
// feed the frame, read the result. The parser is reused across frames, so a
// parse never allocates. Returns false (and leaves `out` as Unknown) when the
// frame is not JSON or carries no recognized `type`.
bool parseEvent(const char* json, size_t len, Event& out);

// ── device -> relay frames ───────────────────────────────────────────────
//
// Every builder writes into a caller buffer and returns the byte length
// (excluding the NUL) or 0 when any argument is invalid or the frame does not
// fit. A partial frame is never written.

// `mode` selects the server pipeline: "relay" (STT + LLM chat) or "dictation"
// (STT only; the capture ends at session.done with the transcript). nullptr
// keeps the historical "relay" bytes unchanged.
size_t buildSessionStart(char* out, size_t cap, const char* requestId, const char* deviceId,
                         const char* mode = nullptr);
size_t buildAudioStart(char* out, size_t cap, const char* requestId, const char* sessionId);
size_t buildAudioEnd(char* out, size_t cap, const char* requestId, const char* sessionId,
                     const char* reason = "vad_silence");
size_t buildSessionCancel(char* out, size_t cap, const char* requestId, const char* sessionId,
                          const char* reason = "user_cancel");

// Fills `out` (>= REQUEST_ID_CAP bytes) with 32 lowercase hex chars + NUL.
// Pure formatting: the caller supplies the entropy (esp_random() on device,
// fixtures in tests), which keeps this file host-compilable.
void formatRequestId(char* out, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

// ── chat endpoint (Ask-AI text round trip) ───────────────────────────────
//
// The same relay serves POST <origin>/api/v1/chat for one-shot text rounds
// (relay/relay/main.py ChatRequest): bearer token + X-Device-Id, JSON body,
// non-streaming reply {"requestId", "text", "usage"}. Built and parsed here
// so the bytes stay host-testable next to the WebSocket frames.

// Server cap for a chat round; matches OpenClaw's CHAT_MESSAGE_CAP so the
// Ask-AI answer path keeps one bound whichever backend produced the reply.
constexpr size_t CHAT_TEXT_CAP = 1536;

// `mode` selects a server-side preset: "dict" switches the relay to the
// dictionary system prompt and token budget (relay config LLM_DICT_*). nullptr
// omits the field entirely and keeps the historical "ask" bytes unchanged.
size_t buildChatRequest(char* out, size_t cap, const char* requestId, const char* deviceId, const char* text,
                        const char* mode = nullptr);

// Parses a non-stream /api/v1/chat body. Returns true when a "text" field was
// present (an empty answer is still a valid reply). `error` is filled from
// {"code": ...} / {"error": ...} bodies so the caller can map 401/502
// failures; ErrorCode::None when the body carried no error token. `truncated`
// is set when textOut was too small and the tail was cut at a codepoint
// boundary.
bool parseChatResponse(const char* json, size_t len, char* textOut, size_t textCap, bool& truncated, ErrorCode& error);

// ── PCM framing ──────────────────────────────────────────────────────────
//
// The recorder produces 100 ms chunks (1600 samples); the wire wants 40 ms
// frames. The framer carries the odd 640 bytes across feed() calls, so every
// emitted frame is exactly FRAME_BYTES.
class PcmFramer {
 public:
  using FrameFn = void (*)(const uint8_t* frame, size_t len, void* ctx);

  void reset() { carryLen = 0; }
  size_t pending() const { return carryLen; }

  // Consumes `pcm` bytes (arbitrary length) and invokes fn once per complete
  // frame. Frames are emitted from a caller-supplied view: the data pointer
  // passed to fn is either internal carry storage or `pcm` itself, so fn must
  // not retain it.
  void feed(const uint8_t* pcm, size_t len, FrameFn fn, void* ctx);

 private:
  uint8_t carry[FRAME_BYTES] = {};
  size_t carryLen = 0;
};

// ── relay URL ────────────────────────────────────────────────────────────

struct ParsedUrl {
  bool valid = false;
  bool tls = false;  // wss://
  char host[64] = {};
  uint16_t port = 0;
  char path[96] = "/";
};

// Parses "ws://host[:port][/path]" or "wss://host[:port][/path]". Defaults:
// port 80/443, path "/". IPv4 literals and DNS names both fit host. Returns
// valid=false for anything else; `host` and `path` stay empty in that case.
ParsedUrl parseRelayUrl(const char* url);

}  // namespace VoiceRelay
