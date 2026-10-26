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
constexpr size_t DEVICE_ID_CAP = 40;   // "reading-companion-001" + margin
constexpr size_t IMAGE_ID_CHARS = 16;  // /api/v1/image asset id: 16 lowercase hex
constexpr size_t IMAGE_ID_CAP = IMAGE_ID_CHARS + 1;

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
// strings (this library does not depend on I18n). The T2i* codes are not
// protocol.py members: they are the error tokens of the /api/v1/image
// endpoint (relay t2i.py T2iError.code).
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
  T2iEmptyPrompt,
  T2iRejected,
  T2iNotConfigured,
  T2iUpstream,
  T2iTimeout,
  T2iBadParam,  // T2I_BAD_PARAM — unknown style/size or a bad edit image
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
//
// The optional "strokes" field (a dict card's encoded stroke-order payload,
// see relay tools/build_stroke_data.py) is captured into strokesOut when a
// buffer is passed. The payload is ASCII digits and ',', ';', '|' only. A
// payload that does not fit is dropped in full, never truncated — a partial
// draw would render the wrong character.
bool parseChatResponse(const char* json, size_t len, char* textOut, size_t textCap, bool& truncated, ErrorCode& error,
                       char* strokesOut = nullptr, size_t strokesCap = 0);

// ── learning endpoint (百词斩 word book sync) ─────────────────────────────
//
// POST <origin>/api/v1/learning/words with {"words":["...",...],"source":"..."}
// (relay/relay/main.py learning_add_words): success body
// {"ok":true,"accepted":N,"added":N}; errors as {"error":"UNAUTHORIZED"} or
// {"error":"invalid_words","max":200}. The relay accepts at most
// LEARNING_MAX_WORDS_PER_REQUEST entries per call — the device chunks smaller
// (LEARNING_CHUNK_WORDS) so one request stays near 1 KB on the wire.

constexpr size_t LEARNING_MAX_WORDS_PER_REQUEST = 200;
constexpr size_t LEARNING_CHUNK_WORDS = 16;

struct AddWordsResult {
  bool ok = false;                    // body carried "ok" (true)
  int32_t accepted = -1;              // echoed request size; -1 = absent
  int32_t added = -1;                 // rows newly inserted; -1 = absent
  ErrorCode error = ErrorCode::None;  // {"error":...} / {"code":...} body
};

// Builds the request body; `words` entries are escaped through the shared JSON
// escaper. Returns 0 (and never a partial frame) on a null/empty word, a null
// array, or when the frame does not fit `cap`.
size_t buildAddWordsRequest(char* out, size_t cap, const char* const* words, size_t count,
                            const char* source = "lookup");

// Builds POST /api/v1/learning/words/remove: {"words":["...",...]} (relay
// main.py learning_remove_words; success body {"ok":true,"accepted":N,
// "removed":N}). Validation mirrors the add builder. The response parses with
// parseAddWordsResponse — the device acts on ok/error, not on the count
// (`added` stays -1 for a remove ack).
size_t buildRemoveWordsRequest(char* out, size_t cap, const char* const* words, size_t count);

// True when the body carried "ok" (an empty-batch echo is still a success).
// On false, out.error holds the token from an {"error":...} body so the
// transport can map 401/400 without a second parser.
bool parseAddWordsResponse(const char* json, size_t len, AddWordsResult& out);

// ── review endpoint (今日复习) ────────────────────────────────────────────
//
// GET <origin>/api/v1/learning/review?limit=N returns the words whose last
// review interval expired, oldest due first:
//   {"count":N,"due":TOTAL,"words":[{"word":"...","addedAt":...,
//     "source":"...","lastRating":N|null,"lastReviewedAt":...,"dueAt":...}]}
// POST <origin>/api/v1/learning/review takes
//   {"word":"...","rating":1..4,"clientEventId":"<32 hex>"}
// and answers {"ok":true,"duplicate":bool}; clientEventId is the idempotency
// key, so a retried submission never records the review twice.

// Matches WordBookFormat::WORD_CAP; the codec keeps its own copy so it stays
// independent of the storage layer.
constexpr size_t LEARNING_WORD_CAP = 48;
// One page is a whole review session on a 480-px panel; the relay clamps larger
// limits to 50.
constexpr size_t LEARNING_REVIEW_LIMIT = 20;

struct ReviewItem {
  char word[LEARNING_WORD_CAP] = {};
  int32_t lastRating = -1;  // 1..4; -1 when the word was never reviewed
};

struct ReviewQueueResult {
  int32_t count = -1;  // items on this page; -1 = absent
  int32_t due = -1;    // total due; -1 = absent
};

// Parses a review queue body. Items fill `out` in server order up to `cap`;
// `stored` is the number written. Returns true when the body parsed and the
// "words" array was present (an empty queue is still a valid response).
bool parseReviewQueueResponse(const char* json, size_t len, ReviewItem* out, size_t cap, size_t& stored,
                              ReviewQueueResult& result);

// Builds the review POST body. Returns 0 on a null/empty word or event id, a
// rating outside 1..4, or when the frame does not fit `cap`.
size_t buildReviewRequest(char* out, size_t cap, const char* word, int rating, const char* clientEventId);

struct ReviewSubmitResult {
  bool ok = false;                    // body carried "ok" (true)
  bool duplicate = false;             // the relay already had this event id
  ErrorCode error = ErrorCode::None;  // {"error":...} / {"code":...} body
};

// True when the body carried "ok". A duplicate is a success: the event is
// already stored server-side.
bool parseReviewResponse(const char* json, size_t len, ReviewSubmitResult& out);

// ── image endpoint (AI 生图) ─────────────────────────────────────────────
//
// POST <origin>/api/v1/image on the same relay (relay/relay/main.py
// ImageRequest): bearer token + X-Device-Id, JSON body {requestId, deviceId,
// text[, style, size]} for generation, or {requestId, deviceId, text, image}
// for edit mode (image = {"data": <base64 BMP>, "format", "width",
// "height"}). Success body: {"requestId", "text" (waiting phrase), "image":
// {"id", "url", "width", "height", "bytes", "format"}}. The asset itself is
// fetched separately with GET /api/v1/image/<id>.

// Server cap for an image prompt. One line of typing, not a paragraph; the
// relay never returns a prompt-length error, it just hands the text to the
// image backend as-is.
constexpr size_t IMAGE_TEXT_CAP = 512;

struct ImageMeta {
  char id[IMAGE_ID_CAP] = {};  // asset id; empty until parseImageResponse returns true
  int32_t width = 0;
  int32_t height = 0;
  int32_t bytes = 0;
};

size_t buildImageRequest(char* out, size_t cap, const char* requestId, const char* deviceId, const char* text,
                         const char* style = nullptr, const char* size = nullptr);

// Edit-mode request (device 修改 flow): text + the source image as base64.
// imageB64 is the already-encoded payload (SttCodec::base64Encode of the
// panel BMP); width/height are the BMP header's pixel dimensions. The frame
// is large (~65 KB for a 48 KB BMP) — the caller supplies a PSRAM buffer.
size_t buildImageEditRequest(char* out, size_t cap, const char* requestId, const char* deviceId, const char* text,
                             const char* imageB64, int32_t width, int32_t height);

// Parses a /api/v1/image body. Returns true when the "image.id" field was
// present — a complete ImageMeta; `textOut` (optional) receives the waiting
// phrase when a buffer is passed. On failure `error` is filled from a
// {"code": "T2I_..."} body (ErrorCode::None when the body carried no error
// token, e.g. a transport-level failure).
bool parseImageResponse(const char* json, size_t len, ImageMeta& out, char* textOut, size_t textCap, ErrorCode& error);

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
