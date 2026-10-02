#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <memory>

// STT wire-format codec for the MiMo-V2.5-ASR endpoint
// (POST /v1/chat/completions, OpenAI-compatible).
//
// Everything here is pure C++ (no Arduino / ESP-IDF headers) so the exact bytes
// we put on the wire can be exercised by the host test suite in test/stt_codec.
// The device-side transport lives in lib/voice/SttClient.
//
// The request is a single JSON body whose input_audio.data carries the whole
// capture as data:audio/wav;base64,....
namespace SttCodec {

// 16 kHz mono s16le — matches HalMicrophone::SAMPLE_RATE.
constexpr uint32_t DEFAULT_SAMPLE_RATE = 16000;
constexpr const char* DEFAULT_MODEL = "mimo-v2.5-asr";
// Transcription result cap. Mirrors STT_TEXT_MAX upstream; larger values only
// waste the text buffer (a sentence of ASR output fits comfortably).
constexpr size_t TEXT_CAPACITY = 512;

// What input_audio.data carries — a full WAV as a base64 data URL.
constexpr const char* DATA_URL_PREFIX = "data:audio/wav;base64,";

// ── WAV ──────────────────────────────────────────────────────────────────

// Canonical 44-byte PCM WAV header (RIFF/WAVE/fmt /data).
struct __attribute__((packed)) WavHeader {
  char riff[4];
  uint32_t fileSize;  // sizeof(WavHeader) - 8 + pcmBytes
  char wave[4];
  char fmtId[4];
  uint32_t fmtSize;
  uint16_t audioFmt;  // 1 = PCM
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char dataId[4];
  uint32_t dataSize;  // pcmBytes
};

static_assert(sizeof(WavHeader) == 44, "WAV header must be 44 bytes");
static_assert(offsetof(WavHeader, dataSize) == 40, "unexpected WAV field layout");

constexpr size_t WAV_HEADER_SIZE = sizeof(WavHeader);

void buildWavHeader(WavHeader& header, uint32_t pcmBytes, uint32_t sampleRate);

// ── base64 ───────────────────────────────────────────────────────────────

// Exact output length for inLen input bytes (padding included), excluding the
// terminating NUL.
constexpr size_t base64EncodedSize(size_t inLen) { return ((inLen + 2) / 3) * 4; }

// Encodes in[0..inLen) into out. outCap must be >= base64EncodedSize(inLen) + 1
// for the NUL terminator. Returns false when it does not fit (nothing written).
// out is NUL-terminated on success; outLen receives the encoded length.
bool base64Encode(const uint8_t* in, size_t inLen, char* out, size_t outCap, size_t& outLen);

// ── request body ─────────────────────────────────────────────────────────

// Exact byte length buildRequestBody() will produce for this WAV/model,
// excluding the NUL terminator. 0 if the request cannot be built.
size_t requestBodySize(size_t wavBytes, const char* model);

// Serialises { model, messages[0].content[0] = input_audio(data URL), stream }
// into out. The WAV is base64-encoded directly into the output buffer, so the
// peak footprint is the request body alone — never body + base64 copy.
//
// Returns the body length (excluding NUL) or 0 when outCap is too small or an
// argument is null. out is NUL-terminated on success.
size_t buildRequestBody(char* out, size_t outCap, const char* model, const uint8_t* wav, size_t wavBytes);

// ── streaming request body ───────────────────────────────────────────────
//
// buildRequestBody() needs the whole body in RAM (~215 KB for a 5 s capture).
// The streaming writer produces the identical bytes in ~7 KB of working set so
// the transport can push the body straight to the socket:
//
//   writeBodyHead()  { ...,"input_audio":{"data":"data:audio/wav;base64,
//   writer.next()*   base64 payload, in aligned chunks
//   writeBodyTail()  "}}]}],"asr_options":{"language":"auto"},"stream":true}
//
// requestBodySize() is the sum of those three, so it doubles as the
// Content-Length / esp_http_client_open() write length.

// Payload bytes of the JSON opening up to and including DATA_URL_PREFIX.
// Returns 0 when model is null or outCap is too small. out is not
// NUL-terminated (the tail is written straight after it).
size_t writeBodyHead(char* out, size_t outCap, const char* model);

// Payload bytes of the JSON closing. Same buffer contract as writeBodyHead().
size_t writeBodyTail(char* out, size_t outCap);

// Chunked base64 encoder over the WAV byte stream (header then PCM).
//
// Every non-final chunk is a whole number of 3-byte input groups, so the
// chunks concatenate to exactly base64EncodedSize(headerBytes + pcmLen) bytes
// with no carry state between calls. The final chunk carries the remainder
// (and its padding).
//
// Sized for heap placement (~7 KB): create it with makeUniqueNoThrow, never on
// the stack.
class WavBase64Writer {
 public:
  // Input bytes per chunk. Divisible by 3 so every chunk except the last is
  // independently encodable; large enough to keep socket writes efficient.
  //
  // input+output+1 = 28 KB in a single allocation, which is above
  // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB), so the working set lands in
  // PSRAM rather than the ~26 KB of link-time internal-RAM headroom.
  static constexpr size_t CHUNK_IN = 12000;
  static constexpr size_t CHUNK_OUT = base64EncodedSize(CHUNK_IN);

  WavBase64Writer() = default;
  WavBase64Writer(const WavBase64Writer&) = delete;
  WavBase64Writer& operator=(const WavBase64Writer&) = delete;

  // Points the writer at header||pcm. Returns false (and releases the scratch
  // buffers) when the scratch allocation fails. header must stay valid for the
  // lifetime of the writer; pcm too, and must outlive the capture's use of it.
  bool begin(const void* header, size_t headerBytes, const uint8_t* pcm, size_t pcmLen);

  // Fills *out/*outLen with the next base64 chunk. *out points into the
  // writer's own buffer and stays valid until the next next() call. Returns
  // false when the stream is finished (or begin() never succeeded).
  bool next(const char** out, size_t* outLen);

  bool finished() const { return inputTotal > 0 && inputConsumed >= inputTotal; }
  // Total input bytes (header + PCM) and how many have been encoded so far.
  size_t total() const { return inputTotal; }
  size_t consumed() const { return inputConsumed; }

 private:
  static constexpr size_t MAX_HEADER = 64;  // > WAV_HEADER_SIZE (44)

  uint8_t headerBuf[MAX_HEADER] = {};
  size_t headerLen = 0;
  const uint8_t* pcm = nullptr;
  size_t pcmLen = 0;
  size_t inputTotal = 0;
  size_t inputConsumed = 0;
  // [0, CHUNK_IN) holds input, [CHUNK_IN, CHUNK_IN + CHUNK_OUT + 1) holds the
  // encoded output. One allocation, reused for every chunk — no per-chunk heap
  // traffic, no manual free on early returns.
  std::unique_ptr<uint8_t[]> payload;
};

// ── SSE response ─────────────────────────────────────────────────────────

// Incremental parser for `data: {...}` Server-Sent-Events carrying
// choices[0].delta.content tokens plus the terminating `data: [DONE]`.
//
// Sized for heap placement (≈6 KB): create it with makeUniqueNoThrow, never on
// the stack.
class SseTranscriptParser {
 public:
  // Longest single SSE line we keep. A `data:` line beyond this is dropped and
  // flagged; delta tokens are far smaller, so this only guards against a
  // server that ignores "stream":true and answers with one giant line.
  static constexpr size_t LINE_CAPACITY = 4096;

  SseTranscriptParser() : parser(makeCallbacks()) {}
  // The embedded parser captures `this` in its callbacks, so copying would
  // leave the copy pointing back at the original.
  SseTranscriptParser(const SseTranscriptParser&) = delete;
  SseTranscriptParser& operator=(const SseTranscriptParser&) = delete;

  // Discards all state (line buffer, text, counters).
  void reset();

  // Consumes raw bytes of the SSE stream. Returns true once the stream is
  // finished (`data: [DONE]` or finish_reason stop/length) — the caller can
  // stop reading, but further feed() calls are harmless.
  bool feed(const uint8_t* data, size_t len);
  bool feed(const char* data, size_t len) { return feed(reinterpret_cast<const uint8_t*>(data), len); }

  // Flushes a trailing line that arrived without its newline (stream EOF).
  // Call once after the transport reports end-of-body. Returns done().
  bool finish();

  bool done() const { return streamDone; }
  const char* text() const { return textBuf; }
  size_t textSize() const { return textLen; }
  int eventCount() const { return events; }
  // True if some line exceeded LINE_CAPACITY and was discarded.
  bool lineOverflow() const { return lineOverflowed; }

 private:
  JsonCallbacks makeCallbacks();

  // SAX callbacks (free functions taking `this` as ctx).
  static void onKeyCb(void* ctx, const char* key, size_t len);
  static void onStringCb(void* ctx, const char* value, size_t len);
  static void onNullCb(void* ctx);
  static void onObjectStartCb(void* ctx);
  static void onObjectEndCb(void* ctx);
  static void onNumberCb(void* ctx, const char* value, size_t len);
  static void onBoolCb(void* ctx, bool value);
  static void onArrayStartCb(void* ctx);
  static void onArrayEndCb(void* ctx);

  void handleLine();
  void handleEvent(const char* json, size_t len);
  bool appendText(const char* value, size_t len);
  void clearKey();

  // SSE assembly. lineTruncated tracks the line currently being accumulated so
  // it can be discarded whole; lineOverflowed is the sticky public flag.
  char lineBuf[LINE_CAPACITY] = {};
  size_t lineLen = 0;
  bool lineTruncated = false;
  bool lineOverflowed = false;
  bool streamDone = false;
  int events = 0;

  // Accumulated transcript
  char textBuf[TEXT_CAPACITY + 1] = {};
  size_t textLen = 0;

  // Per-event JSON extraction state
  char lastKey[64] = {};
  size_t lastKeyLen = 0;
  bool inDelta = false;
  uint8_t objectDepth = 0;
  uint8_t deltaDepth = 0;

  StreamingJsonParser parser;
};

}  // namespace SttCodec
