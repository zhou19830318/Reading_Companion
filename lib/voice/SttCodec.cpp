#include "SttCodec.h"

#include <Memory.h>

#include <cstring>

namespace SttCodec {

// ── WAV ──────────────────────────────────────────────────────────────────

void buildWavHeader(WavHeader& header, uint32_t pcmBytes, uint32_t sampleRate) {
  memset(&header, 0, sizeof(header));
  memcpy(header.riff, "RIFF", 4);
  header.fileSize = static_cast<uint32_t>(sizeof(WavHeader)) - 8 + pcmBytes;
  memcpy(header.wave, "WAVE", 4);
  memcpy(header.fmtId, "fmt ", 4);
  header.fmtSize = 16;
  header.audioFmt = 1;  // PCM
  header.numChannels = 1;
  header.bitsPerSample = 16;
  // byteRate / blockAlign derive from bitsPerSample, so it must be set first —
  // computing them from the zeroed field produced a header with byteRate = 0
  // and blockAlign = 0, which players and ASR front-ends reject (or divide by).
  header.sampleRate = sampleRate;
  header.byteRate = sampleRate * header.numChannels * header.bitsPerSample / 8;
  header.blockAlign = header.numChannels * header.bitsPerSample / 8;
  memcpy(header.dataId, "data", 4);
  header.dataSize = pcmBytes;
}

// ── base64 ───────────────────────────────────────────────────────────────

bool base64Encode(const uint8_t* in, size_t inLen, char* out, size_t outCap, size_t& outLen) {
  static constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  if (out == nullptr) return false;
  if (inLen > 0 && in == nullptr) return false;

  const size_t need = base64EncodedSize(inLen);
  if (outCap < need + 1) return false;  // room for payload + NUL

  size_t o = 0;
  size_t i = 0;
  for (; i + 3 <= inLen; i += 3) {
    const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8) | in[i + 2];
    out[o++] = ALPHABET[(v >> 18) & 0x3F];
    out[o++] = ALPHABET[(v >> 12) & 0x3F];
    out[o++] = ALPHABET[(v >> 6) & 0x3F];
    out[o++] = ALPHABET[v & 0x3F];
  }

  const size_t rem = inLen - i;
  if (rem == 1) {
    const uint32_t v = static_cast<uint32_t>(in[i]) << 16;
    out[o++] = ALPHABET[(v >> 18) & 0x3F];
    out[o++] = ALPHABET[(v >> 12) & 0x3F];
    out[o++] = '=';
    out[o++] = '=';
  } else if (rem == 2) {
    const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8);
    out[o++] = ALPHABET[(v >> 18) & 0x3F];
    out[o++] = ALPHABET[(v >> 12) & 0x3F];
    out[o++] = ALPHABET[(v >> 6) & 0x3F];
    out[o++] = '=';
  }

  out[o] = '\0';
  outLen = o;
  return true;
}

// ── request body ─────────────────────────────────────────────────────────

namespace {
// Kept as literals (assembled with memcpy, never printf) so the body is built
// without a format-string surface and the exact length is computable up front.
constexpr const char JSON_HEAD_MODEL[] = "{\"model\":\"";
constexpr const char JSON_HEAD_AUDIO[] =
    "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\","
    "\"input_audio\":{\"data\":\"";
constexpr const char JSON_TAIL[] = "\"}}]}],\"asr_options\":{\"language\":\"auto\"},\"stream\":true}";

// Appends src to out at *pos when it fits. Returns false when it does not.
bool append(char* out, size_t cap, size_t* pos, const char* src, size_t len) {
  if (*pos + len + 1 > cap) return false;
  memcpy(out + *pos, src, len);
  *pos += len;
  return true;
}
}  // namespace

size_t requestBodySize(size_t wavBytes, const char* model) {
  if (model == nullptr) return 0;
  const size_t head = strlen(JSON_HEAD_MODEL) + strlen(model) + strlen(JSON_HEAD_AUDIO) + strlen(DATA_URL_PREFIX);
  const size_t tail = strlen(JSON_TAIL);
  return head + base64EncodedSize(wavBytes) + tail;
}

size_t writeBodyHead(char* out, size_t outCap, const char* model) {
  if (out == nullptr || model == nullptr) return 0;
  size_t pos = 0;
  if (!append(out, outCap, &pos, JSON_HEAD_MODEL, strlen(JSON_HEAD_MODEL))) return 0;
  if (!append(out, outCap, &pos, model, strlen(model))) return 0;
  if (!append(out, outCap, &pos, JSON_HEAD_AUDIO, strlen(JSON_HEAD_AUDIO))) return 0;
  if (!append(out, outCap, &pos, DATA_URL_PREFIX, strlen(DATA_URL_PREFIX))) return 0;
  return pos;
}

size_t writeBodyTail(char* out, size_t outCap) {
  if (out == nullptr) return 0;
  size_t pos = 0;
  if (!append(out, outCap, &pos, JSON_TAIL, strlen(JSON_TAIL))) return 0;
  return pos;
}

size_t buildRequestBody(char* out, size_t outCap, const char* model, const uint8_t* wav, size_t wavBytes) {
  if (out == nullptr || model == nullptr || (wavBytes > 0 && wav == nullptr)) return 0;
  if (outCap == 0) return 0;

  const size_t headLen = writeBodyHead(out, outCap, model);
  if (headLen == 0) return 0;

  // Base64 straight into the body: no intermediate base64 buffer.
  size_t encoded = 0;
  if (!base64Encode(wav, wavBytes, out + headLen, outCap - headLen, encoded)) return 0;

  const size_t tailLen = writeBodyTail(out + headLen + encoded, outCap - headLen - encoded);
  if (tailLen == 0) return 0;

  const size_t pos = headLen + encoded + tailLen;
  out[pos] = '\0';
  return pos;
}

// ── streaming request body ───────────────────────────────────────────────

bool WavBase64Writer::begin(const void* header, size_t headerBytes, const uint8_t* pcmData, size_t pcmBytes) {
  payload.reset();
  // NB: the member is this->headerLen — assigning to a shadowing parameter
  // here would silently zero the header. Reset the member explicitly.
  this->headerLen = 0;
  pcm = nullptr;
  pcmLen = 0;
  inputTotal = 0;
  inputConsumed = 0;

  if (headerBytes > MAX_HEADER) return false;
  if (headerBytes > 0 && header == nullptr) return false;
  if (pcmBytes > 0 && pcmData == nullptr) return false;

  // One allocation big enough that the allocator places it in PSRAM (see the
  // CHUNK_IN comment in the header) instead of scarce internal DRAM.
  payload = makeUniqueNoThrow<uint8_t[]>(CHUNK_IN + CHUNK_OUT + 1);
  if (!payload) {
    // Pure codec: no logging here (Logging.h pulls Arduino). The transport logs
    // the failure when begin() reports false.
    return false;
  }

  if (headerBytes > 0) memcpy(headerBuf, header, headerBytes);
  this->headerLen = headerBytes;
  pcm = pcmData;
  pcmLen = pcmBytes;
  inputTotal = headerBytes + pcmBytes;
  return inputTotal > 0;
}

bool WavBase64Writer::next(const char** out, size_t* outLen) {
  if (out == nullptr || outLen == nullptr) return false;
  if (inputTotal == 0 || inputConsumed >= inputTotal || !payload) return false;

  const size_t remaining = inputTotal - inputConsumed;
  size_t take = remaining < CHUNK_IN ? remaining : CHUNK_IN;
  // Every chunk but the last must hold whole 3-byte groups so the concatenation
  // of chunks equals a single base64Encode() of the whole stream.
  if (take != remaining) take -= take % 3;

  uint8_t* const scratch = payload.get();
  char* const outBuf = reinterpret_cast<char*>(payload.get() + CHUNK_IN);

  // Gather header||PCM into one contiguous scratch block (only the first chunk
  // can straddle the 44-byte header boundary).
  for (size_t i = 0; i < take; ++i) {
    const size_t idx = inputConsumed + i;
    scratch[i] = idx < headerLen ? headerBuf[idx] : pcm[idx - headerLen];
  }

  size_t encoded = 0;
  if (!base64Encode(scratch, take, outBuf, CHUNK_OUT + 1, encoded)) return false;

  inputConsumed += take;
  *out = outBuf;
  *outLen = encoded;
  return true;
}

// ── SSE response ─────────────────────────────────────────────────────────

JsonCallbacks SseTranscriptParser::makeCallbacks() {
  return {this,
          &SseTranscriptParser::onKeyCb,
          &SseTranscriptParser::onStringCb,
          &SseTranscriptParser::onNumberCb,
          &SseTranscriptParser::onBoolCb,
          &SseTranscriptParser::onNullCb,
          &SseTranscriptParser::onObjectStartCb,
          &SseTranscriptParser::onObjectEndCb,
          &SseTranscriptParser::onArrayStartCb,
          &SseTranscriptParser::onArrayEndCb};
}

void SseTranscriptParser::reset() {
  lineLen = 0;
  lineTruncated = false;
  lineOverflowed = false;
  streamDone = false;
  events = 0;
  textLen = 0;
  textBuf[0] = '\0';
  parser.reset();
  clearKey();
  inDelta = false;
  objectDepth = 0;
  deltaDepth = 0;
}

void SseTranscriptParser::clearKey() {
  lastKeyLen = 0;
  lastKey[0] = '\0';
}

bool SseTranscriptParser::feed(const uint8_t* data, size_t len) {
  if (data == nullptr) return streamDone;

  for (size_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c == '\n') {
      if (lineTruncated) {
        lineTruncated = false;  // discard the whole over-long line
      } else {
        handleLine();
      }
      lineLen = 0;
      if (streamDone) return true;
      continue;
    }
    if (c == '\r') continue;  // tolerate CRLF
    if (lineLen < LINE_CAPACITY - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineTruncated = true;
      lineOverflowed = true;
    }
  }
  return streamDone;
}

bool SseTranscriptParser::finish() {
  // Flush a trailing line that never got its newline (stream EOF). Keep
  // lineOverflowed sticky so a dropped line stays observable after the flush.
  if (!lineTruncated && lineLen > 0) handleLine();
  lineLen = 0;
  lineTruncated = false;
  return streamDone;
}

void SseTranscriptParser::handleLine() {
  // Callers skip an over-long line (feed()/finish() check lineTruncated) so
  // nothing is parsed from a truncated buffer. lineOverflowed stays set as a
  // sticky report to the caller, but must not poison later, well-formed lines.
  if (lineLen == 0) return;  // blank line = event separator
  lineBuf[lineLen] = '\0';

  const char* json = nullptr;
  if (lineLen >= 6 && memcmp(lineBuf, "data: ", 6) == 0) {
    json = lineBuf + 6;
  } else if (lineLen >= 5 && memcmp(lineBuf, "data:", 5) == 0) {
    json = lineBuf + 5;
  } else {
    return;  // `event:` lines, `: ping` comments, anything else
  }

  while (*json == ' ') ++json;
  if (*json == '\0') return;

  if (strcmp(json, "[DONE]") == 0) {
    streamDone = true;
    return;
  }
  handleEvent(json, strlen(json));
}

void SseTranscriptParser::handleEvent(const char* json, size_t len) {
  parser.reset();
  clearKey();
  inDelta = false;
  objectDepth = 0;
  deltaDepth = 0;
  parser.feed(json, len);
}

bool SseTranscriptParser::appendText(const char* value, size_t len) {
  if (len == 0) return true;
  ++events;

  const size_t room = TEXT_CAPACITY - textLen;
  const size_t copyLen = len < room ? len : room;
  if (copyLen > 0) {
    memcpy(textBuf + textLen, value, copyLen);
    textLen += copyLen;
    textBuf[textLen] = '\0';
  }
  return copyLen == len;
}

void SseTranscriptParser::onKeyCb(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<SseTranscriptParser*>(ctx);
  const size_t n = len < sizeof(self->lastKey) - 1 ? len : sizeof(self->lastKey) - 1;
  memcpy(self->lastKey, key, n);
  self->lastKey[n] = '\0';
  self->lastKeyLen = n;
}

void SseTranscriptParser::onStringCb(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<SseTranscriptParser*>(ctx);
  const bool isContent = self->inDelta && self->lastKeyLen == 7 && memcmp(self->lastKey, "content", 7) == 0;
  if (isContent) {
    self->appendText(value, len);
  } else if (self->lastKeyLen == 13 && memcmp(self->lastKey, "finish_reason", 13) == 0) {
    if ((len == 4 && memcmp(value, "stop", 4) == 0) || (len == 6 && memcmp(value, "length", 6) == 0)) {
      self->streamDone = true;
    }
  }
  self->clearKey();
}

void SseTranscriptParser::onNullCb(void* ctx) { static_cast<SseTranscriptParser*>(ctx)->clearKey(); }

void SseTranscriptParser::onNumberCb(void* ctx, const char*, size_t) {
  static_cast<SseTranscriptParser*>(ctx)->clearKey();
}

void SseTranscriptParser::onBoolCb(void* ctx, bool) { static_cast<SseTranscriptParser*>(ctx)->clearKey(); }

void SseTranscriptParser::onObjectStartCb(void* ctx) {
  auto* self = static_cast<SseTranscriptParser*>(ctx);
  if (self->objectDepth < 255) ++self->objectDepth;
  if (self->lastKeyLen == 5 && memcmp(self->lastKey, "delta", 5) == 0) {
    self->inDelta = true;
    self->deltaDepth = self->objectDepth;
  }
  self->clearKey();
}

void SseTranscriptParser::onObjectEndCb(void* ctx) {
  auto* self = static_cast<SseTranscriptParser*>(ctx);
  if (self->inDelta && self->objectDepth == self->deltaDepth) self->inDelta = false;
  if (self->objectDepth > 0) --self->objectDepth;
  self->clearKey();
}

void SseTranscriptParser::onArrayStartCb(void* ctx) { static_cast<SseTranscriptParser*>(ctx)->clearKey(); }

void SseTranscriptParser::onArrayEndCb(void* ctx) { static_cast<SseTranscriptParser*>(ctx)->clearKey(); }

}  // namespace SttCodec
