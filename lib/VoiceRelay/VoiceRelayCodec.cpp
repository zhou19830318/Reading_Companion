#include "VoiceRelayCodec.h"

#include <cstdio>
#include <cstring>

namespace VoiceRelay {

// ── names and policy ─────────────────────────────────────────────────────

const char* eventTypeName(EventType type) {
  switch (type) {
    case EventType::SessionStarted:
      return "session.started";
    case EventType::AsrPartial:
      return "asr.partial";
    case EventType::AsrFinal:
      return "asr.final";
    case EventType::LlmStart:
      return "llm.start";
    case EventType::LlmDelta:
      return "llm.delta";
    case EventType::LlmFinal:
      return "llm.final";
    case EventType::SessionDone:
      return "session.done";
    case EventType::Error:
      return "error";
    case EventType::Unknown:
      return "unknown";
  }
  return "unknown";
}

const char* errorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::BadProtocol:
      return "BAD_PROTOCOL";
    case ErrorCode::Unauthorized:
      return "UNAUTHORIZED";
    case ErrorCode::DeviceNotAllowed:
      return "DEVICE_NOT_ALLOWED";
    case ErrorCode::AudioFormatUnsupported:
      return "AUDIO_FORMAT_UNSUPPORTED";
    case ErrorCode::AudioTimeout:
      return "AUDIO_TIMEOUT";
    case ErrorCode::AsrConnectionError:
      return "ASR_CONNECTION_ERROR";
    case ErrorCode::AsrAuthError:
      return "ASR_AUTH_ERROR";
    case ErrorCode::AsrRateLimited:
      return "ASR_RATE_LIMITED";
    case ErrorCode::LlmConnectionError:
      return "LLM_CONNECTION_ERROR";
    case ErrorCode::LlmAuthError:
      return "LLM_AUTH_ERROR";
    case ErrorCode::LlmRateLimited:
      return "LLM_RATE_LIMITED";
    case ErrorCode::SessionTimeout:
      return "SESSION_TIMEOUT";
    case ErrorCode::ServerOverloaded:
      return "SERVER_OVERLOADED";
    case ErrorCode::InternalError:
      return "INTERNAL_ERROR";
    case ErrorCode::T2iEmptyPrompt:
      return "T2I_EMPTY_PROMPT";
    case ErrorCode::T2iRejected:
      return "T2I_REJECTED";
    case ErrorCode::T2iNotConfigured:
      return "T2I_NOT_CONFIGURED";
    case ErrorCode::T2iUpstream:
      return "T2I_UPSTREAM";
    case ErrorCode::T2iTimeout:
      return "T2I_TIMEOUT";
    case ErrorCode::T2iBadParam:
      return "T2I_BAD_PARAM";
    case ErrorCode::None:
    case ErrorCode::Unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

bool errorRetryable(ErrorCode code) {
  // Local fallback for frames without an explicit retryable field; mirrors
  // the retry column of the docs error table. Rate limits are retryable
  // (once, with backoff) — the activity owns the backoff.
  switch (code) {
    case ErrorCode::AsrConnectionError:
    case ErrorCode::LlmConnectionError:
    case ErrorCode::AsrRateLimited:
    case ErrorCode::LlmRateLimited:
    case ErrorCode::ServerOverloaded:
      return true;
    default:
      return false;
  }
}

// ── parsing ──────────────────────────────────────────────────────────────

namespace {

enum class Field : uint8_t {
  None,
  Type,
  RequestId,
  SessionId,
  Text,
  Code,
  Message,
  Retryable,
  Seq,
  // /api/v1/chat error bodies: {"error": "UNAUTHORIZED"} — a second token
  // shape the same name table already covers.
  Error,
  // /api/v1/chat dict cards: the encoded stroke-order payload (digits, ',',
  // ';' and '|' only — see relay tools/build_stroke_data.py).
  Strokes,
  // latencyMs object and its members
  LatencyMs,
  LatConnect,
  LatAsrFirstPartial,
  LatAsrFinal,
  LatLlmFirstToken,
  LatLlmFinal,
  LatVisibleTotal,
  // /api/v1/image success bodies: the "image" object's members. The keys are
  // unique across every body this parser sees (chat replies carry no "id" /
  // "width" / "height" / "bytes"), so capturing them needs no depth tracking.
  ImageId,
  ImageWidth,
  ImageHeight,
  ImageBytes,
};

Field classifyKey(const char* k, size_t n) {
  switch (n) {
    case 2:
      return memcmp(k, "id", 2) == 0 ? Field::ImageId : Field::None;
    case 3:
      return memcmp(k, "seq", 3) == 0 ? Field::Seq : Field::None;
    case 5:
      if (memcmp(k, "error", 5) == 0) return Field::Error;
      if (memcmp(k, "width", 5) == 0) return Field::ImageWidth;
      if (memcmp(k, "bytes", 5) == 0) return Field::ImageBytes;
      return Field::None;
    case 4:
      if (memcmp(k, "type", 4) == 0) return Field::Type;
      if (memcmp(k, "text", 4) == 0) return Field::Text;
      if (memcmp(k, "code", 4) == 0) return Field::Code;
      return Field::None;
    case 6:
      return memcmp(k, "height", 6) == 0 ? Field::ImageHeight : Field::None;
    case 7:
      if (memcmp(k, "message", 7) == 0) return Field::Message;
      if (memcmp(k, "connect", 7) == 0) return Field::LatConnect;
      if (memcmp(k, "strokes", 7) == 0) return Field::Strokes;
      return Field::None;
    case 8:
      if (memcmp(k, "asrFinal", 8) == 0) return Field::LatAsrFinal;
      if (memcmp(k, "llmFinal", 8) == 0) return Field::LatLlmFinal;
      return Field::None;
    case 9:
      if (memcmp(k, "requestId", 9) == 0) return Field::RequestId;
      if (memcmp(k, "sessionId", 9) == 0) return Field::SessionId;
      if (memcmp(k, "retryable", 9) == 0) return Field::Retryable;
      if (memcmp(k, "latencyMs", 9) == 0) return Field::LatencyMs;
      return Field::None;
    case 12:
      return memcmp(k, "visibleTotal", 12) == 0 ? Field::LatVisibleTotal : Field::None;
    case 13:
      return memcmp(k, "llmFirstToken", 13) == 0 ? Field::LatLlmFirstToken : Field::None;
    case 15:
      return memcmp(k, "asrFirstPartial", 15) == 0 ? Field::LatAsrFirstPartial : Field::None;
    default:
      return Field::None;
  }
}

EventType typeFromWire(const char* s, size_t n) {
  switch (n) {
    case 5:
      return memcmp(s, "error", 5) == 0 ? EventType::Error : EventType::Unknown;
    case 9:
      if (memcmp(s, "asr.final", 9) == 0) return EventType::AsrFinal;
      if (memcmp(s, "llm.start", 9) == 0) return EventType::LlmStart;
      if (memcmp(s, "llm.delta", 9) == 0) return EventType::LlmDelta;
      if (memcmp(s, "llm.final", 9) == 0) return EventType::LlmFinal;
      return EventType::Unknown;
    case 11:
      return memcmp(s, "asr.partial", 11) == 0 ? EventType::AsrPartial : EventType::Unknown;
    case 12:
      return memcmp(s, "session.done", 12) == 0 ? EventType::SessionDone : EventType::Unknown;
    case 15:
      return memcmp(s, "session.started", 15) == 0 ? EventType::SessionStarted : EventType::Unknown;
    default:
      return EventType::Unknown;
  }
}

ErrorCode codeFromWire(const char* s, size_t n) {
  struct Entry {
    const char* name;
    ErrorCode code;
  };
  static constexpr Entry TABLE[] = {
      {"BAD_PROTOCOL", ErrorCode::BadProtocol},
      {"UNAUTHORIZED", ErrorCode::Unauthorized},
      {"DEVICE_NOT_ALLOWED", ErrorCode::DeviceNotAllowed},
      {"AUDIO_FORMAT_UNSUPPORTED", ErrorCode::AudioFormatUnsupported},
      {"AUDIO_TIMEOUT", ErrorCode::AudioTimeout},
      {"ASR_CONNECTION_ERROR", ErrorCode::AsrConnectionError},
      {"ASR_AUTH_ERROR", ErrorCode::AsrAuthError},
      {"ASR_RATE_LIMITED", ErrorCode::AsrRateLimited},
      {"LLM_CONNECTION_ERROR", ErrorCode::LlmConnectionError},
      {"LLM_AUTH_ERROR", ErrorCode::LlmAuthError},
      {"LLM_RATE_LIMITED", ErrorCode::LlmRateLimited},
      {"SESSION_TIMEOUT", ErrorCode::SessionTimeout},
      {"SERVER_OVERLOADED", ErrorCode::ServerOverloaded},
      {"INTERNAL_ERROR", ErrorCode::InternalError},
      // /api/v1/image endpoint tokens (relay t2i.py T2iError.code).
      {"T2I_EMPTY_PROMPT", ErrorCode::T2iEmptyPrompt},
      {"T2I_REJECTED", ErrorCode::T2iRejected},
      {"T2I_NOT_CONFIGURED", ErrorCode::T2iNotConfigured},
      {"T2I_UPSTREAM", ErrorCode::T2iUpstream},
      {"T2I_TIMEOUT", ErrorCode::T2iTimeout},
      {"T2I_BAD_PARAM", ErrorCode::T2iBadParam},
  };
  for (const Entry& e : TABLE) {
    if (strlen(e.name) == n && memcmp(s, e.name, n) == 0) return e.code;
  }
  return ErrorCode::Unknown;
}

int32_t parseI32(const char* s, size_t n) {
  bool neg = false;
  size_t i = 0;
  if (i < n && (s[i] == '-' || s[i] == '+')) {
    neg = s[i] == '-';
    ++i;
  }
  int64_t v = 0;
  for (; i < n && s[i] >= '0' && s[i] <= '9'; ++i) {
    v = v * 10 + (s[i] - '0');
    if (v > 2000000000) {  // latencies never exceed this; clamp, don't overflow
      v = 2000000000;
      break;
    }
  }
  return static_cast<int32_t>(neg ? -v : v);
}

// Copies a fixed-length wire id field, ignoring anything past the buffer.
void copyId(char* dst, size_t cap, const char* v, size_t n) {
  const size_t take = n < cap - 1 ? n : cap - 1;
  memcpy(dst, v, take);
  dst[take] = '\0';
}

void appendMessage(Event& out, const char* v, size_t n) {
  const size_t used = strlen(out.message);
  const size_t room = ERROR_MESSAGE_CAP - 1;
  if (used >= room) return;
  const size_t take = n < room - used ? n : room - used;
  memcpy(out.message + used, v, take);
  out.message[used + take] = '\0';
}

// Drops an incomplete UTF-8 sequence from the end of a truncated buffer and
// NUL-terminates at the new end, so the activity never renders a half codepoint.
void trimUtf8Tail(char* buf, size_t& len) {
  while (len > 0) {
    size_t i = len;
    while (i > 0 && (static_cast<uint8_t>(buf[i - 1]) & 0xC0) == 0x80) --i;
    if (i == 0) {  // only continuation bytes: nothing valid remains
      len = 0;
      break;
    }
    const uint8_t lead = static_cast<uint8_t>(buf[i - 1]);
    size_t need;
    if ((lead & 0x80) == 0)
      need = 1;
    else if ((lead & 0xE0) == 0xC0)
      need = 2;
    else if ((lead & 0xF0) == 0xE0)
      need = 3;
    else if ((lead & 0xF8) == 0xF0)
      need = 4;
    else {  // invalid lead byte: drop it and re-check
      len = i - 1;
      continue;
    }
    if (len - (i - 1) >= need) break;  // complete sequence at the end
    len = i - 1;                       // incomplete: drop the whole sequence
  }
  buf[len] = '\0';
}

struct ParseState {
  Event* out = nullptr;
  EventType type = EventType::Unknown;
  Field field = Field::None;
  bool retryableSeen = false;
  size_t textLen = 0;
  bool inLatency = false;
  uint8_t objectDepth = 0;
  uint8_t latencyDepth = 0;

  void begin(Event& event) {
    out = &event;
    type = EventType::Unknown;
    field = Field::None;
    retryableSeen = false;
    textLen = 0;
    inLatency = false;
    objectDepth = 0;
    latencyDepth = 0;
  }

  void appendText(const char* v, size_t n) {
    if (textLen >= TEXT_CAP) {
      out->truncated = true;
      return;
    }
    const size_t take = n < TEXT_CAP - textLen ? n : TEXT_CAP - textLen;
    memcpy(out->text + textLen, v, take);
    textLen += take;
    out->text[textLen] = '\0';
    if (take < n) out->truncated = true;
  }

  void setLatency(Field f, int32_t value) {
    switch (f) {
      case Field::LatConnect:
        out->latency.connect = value;
        break;
      case Field::LatAsrFirstPartial:
        out->latency.asrFirstPartial = value;
        break;
      case Field::LatAsrFinal:
        out->latency.asrFinal = value;
        break;
      case Field::LatLlmFirstToken:
        out->latency.llmFirstToken = value;
        break;
      case Field::LatLlmFinal:
        out->latency.llmFinal = value;
        break;
      case Field::LatVisibleTotal:
        out->latency.visibleTotal = value;
        break;
      default:
        break;
    }
  }

  static void onKeyCb(void* ctx, const char* key, size_t len) {
    static_cast<ParseState*>(ctx)->field = classifyKey(key, len);
  }

  static void onStringCb(void* ctx, const char* value, size_t len) {
    auto* st = static_cast<ParseState*>(ctx);
    Event& out = *st->out;
    switch (st->field) {
      case Field::Type:
        st->type = typeFromWire(value, len);
        break;
      case Field::RequestId:
        copyId(out.requestId, REQUEST_ID_CAP, value, len);
        break;
      case Field::SessionId:
        copyId(out.sessionId, SESSION_ID_CAP, value, len);
        break;
      case Field::Text:
        st->appendText(value, len);
        break;
      case Field::Code:
        out.code = codeFromWire(value, len);
        break;
      case Field::Message:
        appendMessage(out, value, len);
        break;
      default:
        break;
    }
  }

  static void onStringPartCb(void* ctx, const char* chunk, size_t len) {
    auto* st = static_cast<ParseState*>(ctx);
    if (st->field == Field::Text) st->appendText(chunk, len);
  }

  static void onNumberCb(void* ctx, const char* value, size_t len) {
    auto* st = static_cast<ParseState*>(ctx);
    if (st->field == Field::Seq) {
      const int32_t v = parseI32(value, len);
      st->out->seq = v < 0 ? 0 : (v > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(v));
    } else if (st->inLatency) {
      st->setLatency(st->field, parseI32(value, len));
    }
  }

  static void onBoolCb(void* ctx, bool value) {
    auto* st = static_cast<ParseState*>(ctx);
    if (st->field == Field::Retryable) {
      st->retryableSeen = true;
      st->out->retryable = value;
    }
  }

  static void onNullCb(void*) {}

  static void onObjectStartCb(void* ctx) {
    auto* st = static_cast<ParseState*>(ctx);
    if (st->objectDepth < 255) ++st->objectDepth;
    if (st->field == Field::LatencyMs) {
      st->inLatency = true;
      st->latencyDepth = st->objectDepth;
    }
  }

  static void onObjectEndCb(void* ctx) {
    auto* st = static_cast<ParseState*>(ctx);
    if (st->inLatency && st->objectDepth == st->latencyDepth) st->inLatency = false;
    if (st->objectDepth > 0) --st->objectDepth;
  }

  static void onArrayStartCb(void*) {}
  static void onArrayEndCb(void*) {}
};

ParseState g_state;

JsonCallbacks makeCallbacks() {
  return {&g_state,
          &ParseState::onKeyCb,
          &ParseState::onStringCb,
          &ParseState::onNumberCb,
          &ParseState::onBoolCb,
          &ParseState::onNullCb,
          &ParseState::onObjectStartCb,
          &ParseState::onObjectEndCb,
          &ParseState::onArrayStartCb,
          &ParseState::onArrayEndCb,
          &ParseState::onStringPartCb};
}

// One parser for the whole process: statically allocated (no heap), reused
// across frames. The voice path is a single task on the device, so the
// shared state is never concurrently in use.
StreamingJsonParser g_parser(makeCallbacks());

}  // namespace

bool parseEvent(const char* json, size_t len, Event& out) {
  out = Event{};
  if (json == nullptr || len == 0) return false;

  g_state.begin(out);
  g_parser.reset();
  g_parser.feed(json, len);
  if (g_parser.hasError()) {
    out = Event{};
    return false;
  }

  out.type = g_state.type;
  if (out.type == EventType::Unknown) return false;
  if (out.truncated) trimUtf8Tail(out.text, g_state.textLen);
  if (!g_state.retryableSeen) out.retryable = errorRetryable(out.code);
  return true;
}

// ── /api/v1/chat response ────────────────────────────────────────────────

namespace {

struct ChatParseState {
  char* text = nullptr;
  size_t cap = 0;
  size_t textLen = 0;
  char* strokes = nullptr;
  size_t strokesCap = 0;
  size_t strokesLen = 0;
  // /api/v1/image bodies: the asset id plus its geometry (see parseImageResponse).
  char* imageId = nullptr;
  size_t imageIdCap = 0;
  size_t imageIdLen = 0;
  int32_t imageWidth = 0;
  int32_t imageHeight = 0;
  int32_t imageBytes = 0;
  bool sawText = false;
  bool sawImageId = false;
  bool strokesOverflowed = false;
  bool truncated = false;
  ErrorCode error = ErrorCode::None;
  Field field = Field::None;

  void begin(char* out, size_t outCap, char* strokesOut, size_t strokesOutCap, char* imageIdOut = nullptr,
             size_t imageIdOutCap = 0) {
    text = out;
    cap = outCap;
    textLen = 0;
    strokes = strokesOut;
    strokesCap = strokesOutCap;
    strokesLen = 0;
    imageId = imageIdOut;
    imageIdCap = imageIdOutCap;
    imageIdLen = 0;
    imageWidth = 0;
    imageHeight = 0;
    imageBytes = 0;
    sawText = false;
    sawImageId = false;
    strokesOverflowed = false;
    truncated = false;
    error = ErrorCode::None;
    field = Field::None;
    if (text != nullptr && cap > 0) text[0] = '\0';
    if (strokes != nullptr && strokesCap > 0) strokes[0] = '\0';
    if (imageId != nullptr && imageIdCap > 0) imageId[0] = '\0';
  }

  void append(const char* v, size_t n) {
    sawText = true;
    if (text == nullptr || cap == 0) return;
    if (textLen >= cap - 1) {
      truncated = true;
      return;
    }
    const size_t take = n < cap - 1 - textLen ? n : cap - 1 - textLen;
    memcpy(text + textLen, v, take);
    textLen += take;
    text[textLen] = '\0';
    if (take < n) truncated = true;
  }

  // Stroke payloads are all-or-nothing: a truncated payload draws the wrong
  // character, so on overflow the buffer is left to be cleared at the end.
  void appendStrokes(const char* v, size_t n) {
    if (strokes == nullptr || strokesCap == 0 || strokesOverflowed) return;
    if (strokesLen + n > strokesCap - 1) {
      strokesOverflowed = true;
      return;
    }
    memcpy(strokes + strokesLen, v, n);
    strokesLen += n;
    strokes[strokesLen] = '\0';
  }

  static void onKeyCb(void* ctx, const char* key, size_t len) {
    static_cast<ChatParseState*>(ctx)->field = classifyKey(key, len);
  }
  static void onStringCb(void* ctx, const char* value, size_t len) {
    auto* st = static_cast<ChatParseState*>(ctx);
    switch (st->field) {
      case Field::Text:
        st->append(value, len);
        break;
      case Field::Strokes:
        st->appendStrokes(value, len);
        break;
      case Field::Code:
      case Field::Error:
        st->error = codeFromWire(value, len);
        break;
      case Field::ImageId: {
        // All-or-nothing like the stroke payload: an id cut to fit would name
        // a different asset than the one the server just minted.
        if (st->imageId == nullptr || st->imageIdCap == 0 || len >= st->imageIdCap) break;
        memcpy(st->imageId, value, len);
        st->imageId[len] = '\0';
        st->imageIdLen = len;
        st->sawImageId = true;
        break;
      }
      default:
        break;
    }
  }
  static void onStringPartCb(void* ctx, const char* chunk, size_t len) {
    auto* st = static_cast<ChatParseState*>(ctx);
    if (st->field == Field::Text) {
      st->append(chunk, len);
    } else if (st->field == Field::Strokes) {
      st->appendStrokes(chunk, len);
    }
  }
  static void onNumberCb(void* ctx, const char* value, size_t len) {
    auto* st = static_cast<ChatParseState*>(ctx);
    switch (st->field) {
      case Field::ImageWidth:
        st->imageWidth = parseI32(value, len);
        break;
      case Field::ImageHeight:
        st->imageHeight = parseI32(value, len);
        break;
      case Field::ImageBytes:
        st->imageBytes = parseI32(value, len);
        break;
      default:
        break;
    }
  }
  static void onBoolCb(void*, bool) {}
  static void onNullCb(void*) {}
  static void onObjectStartCb(void*) {}
  static void onObjectEndCb(void*) {}
  static void onArrayStartCb(void*) {}
  static void onArrayEndCb(void*) {}
};

ChatParseState g_chatState;

JsonCallbacks makeChatCallbacks() {
  return {&g_chatState,
          &ChatParseState::onKeyCb,
          &ChatParseState::onStringCb,
          &ChatParseState::onNumberCb,
          &ChatParseState::onBoolCb,
          &ChatParseState::onNullCb,
          &ChatParseState::onObjectStartCb,
          &ChatParseState::onObjectEndCb,
          &ChatParseState::onArrayStartCb,
          &ChatParseState::onArrayEndCb,
          &ChatParseState::onStringPartCb};
}

// A second statically allocated parser (~570 B DRAM): the realtime frames use
// g_parser, whose callbacks are bound at construction. The two paths never
// run concurrently (both are driven from the foreground activity loop), and
// sharing the tested tokenizer beats a second escape-decoding implementation.
StreamingJsonParser g_chatParser(makeChatCallbacks());

}  // namespace

bool parseChatResponse(const char* json, size_t len, char* textOut, size_t textCap, bool& truncated, ErrorCode& error,
                       char* strokesOut, size_t strokesCap) {
  g_chatState.begin(textOut, textCap, strokesOut, strokesCap);
  truncated = false;
  error = ErrorCode::None;
  if (json == nullptr || len == 0 || textOut == nullptr || textCap == 0) return false;

  g_chatParser.reset();
  g_chatParser.feed(json, len);
  if (g_chatParser.hasError()) return false;

  if (g_chatState.truncated) trimUtf8Tail(textOut, g_chatState.textLen);
  // Overflowed payload: empty beats partial (a partial draw is a wrong glyph).
  if (g_chatState.strokesOverflowed && strokesOut != nullptr && strokesCap > 0) strokesOut[0] = '\0';
  truncated = g_chatState.truncated;
  error = g_chatState.error;
  return g_chatState.sawText;
}

bool parseImageResponse(const char* json, size_t len, ImageMeta& out, char* textOut, size_t textCap, ErrorCode& error) {
  out = ImageMeta{};
  g_chatState.begin(textOut, textCap, nullptr, 0, out.id, sizeof(out.id));
  error = ErrorCode::None;
  if (json == nullptr || len == 0) return false;

  g_chatParser.reset();
  g_chatParser.feed(json, len);
  if (g_chatParser.hasError()) return false;

  if (textOut != nullptr && textCap > 0 && g_chatState.truncated) trimUtf8Tail(textOut, g_chatState.textLen);
  error = g_chatState.error;
  if (!g_chatState.sawImageId || g_chatState.imageIdLen == 0) return false;
  out.width = g_chatState.imageWidth;
  out.height = g_chatState.imageHeight;
  out.bytes = g_chatState.imageBytes;
  return true;
}

// ── device -> relay frames ───────────────────────────────────────────────

namespace {

bool appendRaw(char* out, size_t cap, size_t& pos, const char* s, size_t n) {
  if (pos + n + 1 > cap) return false;
  memcpy(out + pos, s, n);
  pos += n;
  return true;
}

bool appendLiteral(char* out, size_t cap, size_t& pos, const char* s) { return appendRaw(out, cap, pos, s, strlen(s)); }

// JSON string content: escapes what the config UI could ever put in a
// device id, so a hostile NVS value cannot break the frame.
bool appendEscaped(char* out, size_t cap, size_t& pos, const char* s, size_t n) {
  static constexpr char HEX[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    if (c == '"' || c == '\\') {
      if (!appendRaw(out, cap, pos, "\\", 1)) return false;
      if (!appendRaw(out, cap, pos, reinterpret_cast<const char*>(&c), 1)) return false;
    } else if (c < 0x20) {
      char esc[6] = {'\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 0xF]};
      if (!appendRaw(out, cap, pos, esc, sizeof(esc))) return false;
    } else {
      if (!appendRaw(out, cap, pos, reinterpret_cast<const char*>(&c), 1)) return false;
    }
  }
  return true;
}

bool finishFrame(char* out, size_t cap, size_t& pos) {
  if (pos + 1 > cap) return false;
  out[pos] = '\0';
  return true;
}

}  // namespace

size_t buildSessionStart(char* out, size_t cap, const char* requestId, const char* deviceId, const char* mode) {
  if (out == nullptr || cap == 0 || requestId == nullptr || deviceId == nullptr) return 0;
  if (mode == nullptr) mode = "relay";  // historical default; bytes unchanged
  size_t pos = 0;
  if (!appendLiteral(out, cap, pos, "{\"type\":\"session.start\",\"requestId\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, requestId, strlen(requestId))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"deviceId\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, deviceId, strlen(deviceId))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"audio\":{\"sampleRate\":16000,\"channels\":1,\"format\":\"pcm_s16le\""))
    return 0;
  if (!appendLiteral(out, cap, pos, ",\"frameMs\":40},\"mode\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, mode, strlen(mode))) return 0;
  if (!appendLiteral(out, cap, pos, "\"}")) return 0;
  if (!finishFrame(out, cap, pos)) return 0;
  return pos;
}

size_t buildChatRequest(char* out, size_t cap, const char* requestId, const char* deviceId, const char* text,
                        const char* mode) {
  if (out == nullptr || cap == 0 || requestId == nullptr || deviceId == nullptr || text == nullptr) return 0;
  size_t pos = 0;
  if (!appendLiteral(out, cap, pos, "{\"requestId\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, requestId, strlen(requestId))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"deviceId\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, deviceId, strlen(deviceId))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"text\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, text, strlen(text))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"stream\":false")) return 0;
  if (mode != nullptr) {
    if (!appendLiteral(out, cap, pos, ",\"mode\":\"")) return 0;
    if (!appendEscaped(out, cap, pos, mode, strlen(mode))) return 0;
    if (!appendLiteral(out, cap, pos, "\"")) return 0;
  }
  if (!appendLiteral(out, cap, pos, "}")) return 0;
  if (!finishFrame(out, cap, pos)) return 0;
  return pos;
}

size_t buildImageRequest(char* out, size_t cap, const char* requestId, const char* deviceId, const char* text,
                         const char* style, const char* size) {
  if (out == nullptr || cap == 0 || requestId == nullptr || deviceId == nullptr || text == nullptr) return 0;
  size_t pos = 0;
  if (!appendLiteral(out, cap, pos, "{\"requestId\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, requestId, strlen(requestId))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"deviceId\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, deviceId, strlen(deviceId))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"text\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, text, strlen(text))) return 0;
  if (!appendLiteral(out, cap, pos, "\"")) return 0;
  // Optional generation parameters: absent fields keep the server defaults
  // (mock/dashscope style auto, T2I_SIZE), so the historical frames for
  // callers that pass nothing stay byte-identical.
  if (style != nullptr && style[0] != '\0') {
    if (!appendLiteral(out, cap, pos, ",\"style\":\"")) return 0;
    if (!appendEscaped(out, cap, pos, style, strlen(style))) return 0;
    if (!appendLiteral(out, cap, pos, "\"")) return 0;
  }
  if (size != nullptr && size[0] != '\0') {
    if (!appendLiteral(out, cap, pos, ",\"size\":\"")) return 0;
    if (!appendEscaped(out, cap, pos, size, strlen(size))) return 0;
    if (!appendLiteral(out, cap, pos, "\"")) return 0;
  }
  if (!appendLiteral(out, cap, pos, "}")) return 0;
  if (!finishFrame(out, cap, pos)) return 0;
  return pos;
}

size_t buildImageEditRequest(char* out, size_t cap, const char* requestId, const char* deviceId, const char* text,
                             const char* imageB64, int32_t width, int32_t height) {
  if (out == nullptr || cap == 0 || requestId == nullptr || deviceId == nullptr || text == nullptr || text[0] == '\0' ||
      imageB64 == nullptr || imageB64[0] == '\0' || width <= 0 || height <= 0) {
    return 0;
  }
  char wbuf[12];
  char hbuf[12];
  snprintf(wbuf, sizeof(wbuf), "%d", static_cast<int>(width));
  snprintf(hbuf, sizeof(hbuf), "%d", static_cast<int>(height));
  size_t pos = 0;
  if (!appendLiteral(out, cap, pos, "{\"requestId\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, requestId, strlen(requestId))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"deviceId\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, deviceId, strlen(deviceId))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"text\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, text, strlen(text))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"image\":{\"data\":\"")) return 0;
  // base64 output is ASCII [A-Za-z0-9+/=] — no escaping needed, appended raw.
  if (!appendLiteral(out, cap, pos, imageB64)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"format\":\"bmp\",\"width\":")) return 0;
  if (!appendLiteral(out, cap, pos, wbuf)) return 0;
  if (!appendLiteral(out, cap, pos, ",\"height\":")) return 0;
  if (!appendLiteral(out, cap, pos, hbuf)) return 0;
  if (!appendLiteral(out, cap, pos, "}}")) return 0;
  if (!finishFrame(out, cap, pos)) return 0;
  return pos;
}

namespace {

size_t buildScopedFrame(char* out, size_t cap, const char* type, const char* requestId, const char* sessionId,
                        const char* reason) {
  if (out == nullptr || cap == 0 || type == nullptr || requestId == nullptr || sessionId == nullptr) return 0;
  size_t pos = 0;
  if (!appendLiteral(out, cap, pos, "{\"type\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, type, strlen(type))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"requestId\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, requestId, strlen(requestId))) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"sessionId\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, sessionId, strlen(sessionId))) return 0;
  if (!appendLiteral(out, cap, pos, "\"")) return 0;
  if (reason != nullptr) {
    if (!appendLiteral(out, cap, pos, ",\"reason\":\"")) return 0;
    if (!appendEscaped(out, cap, pos, reason, strlen(reason))) return 0;
    if (!appendLiteral(out, cap, pos, "\"")) return 0;
  }
  if (!appendLiteral(out, cap, pos, "}")) return 0;
  if (!finishFrame(out, cap, pos)) return 0;
  return pos;
}

}  // namespace

size_t buildAudioStart(char* out, size_t cap, const char* requestId, const char* sessionId) {
  return buildScopedFrame(out, cap, "audio.start", requestId, sessionId, nullptr);
}

size_t buildAudioEnd(char* out, size_t cap, const char* requestId, const char* sessionId, const char* reason) {
  return buildScopedFrame(out, cap, "audio.end", requestId, sessionId, reason);
}

size_t buildSessionCancel(char* out, size_t cap, const char* requestId, const char* sessionId, const char* reason) {
  return buildScopedFrame(out, cap, "session.cancel", requestId, sessionId, reason);
}

void formatRequestId(char* out, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  if (out == nullptr) return;
  snprintf(out, REQUEST_ID_CAP, "%08x%08x%08x%08x", static_cast<unsigned>(a), static_cast<unsigned>(b),
           static_cast<unsigned>(c), static_cast<unsigned>(d));
}

// ── PCM framing ──────────────────────────────────────────────────────────

void PcmFramer::feed(const uint8_t* pcm, size_t len, FrameFn fn, void* ctx) {
  if (pcm == nullptr || len == 0 || fn == nullptr) return;

  size_t off = 0;
  if (carryLen > 0) {
    const size_t need = FRAME_BYTES - carryLen;
    const size_t take = len < need ? len : need;
    memcpy(carry + carryLen, pcm, take);
    carryLen += take;
    off += take;
    if (carryLen == FRAME_BYTES) {
      fn(carry, FRAME_BYTES, ctx);
      carryLen = 0;
    }
  }

  while (off + FRAME_BYTES <= len) {
    fn(pcm + off, FRAME_BYTES, ctx);
    off += FRAME_BYTES;
  }

  if (off < len) {
    carryLen = len - off;
    memcpy(carry, pcm + off, carryLen);
  }
}

// ── relay URL ────────────────────────────────────────────────────────────

ParsedUrl parseRelayUrl(const char* url) {
  ParsedUrl out;
  if (url == nullptr) return out;

  const char* p = url;
  if (strncmp(p, "ws://", 5) == 0) {
    out.tls = false;
    p += 5;
  } else if (strncmp(p, "wss://", 6) == 0) {
    out.tls = true;
    p += 6;
  } else {
    return out;
  }

  const char* hostEnd = p;
  while (*hostEnd != '\0' && *hostEnd != ':' && *hostEnd != '/') ++hostEnd;
  const size_t hostLen = static_cast<size_t>(hostEnd - p);
  if (hostLen == 0 || hostLen >= sizeof(out.host)) return out;
  memcpy(out.host, p, hostLen);
  out.host[hostLen] = '\0';
  p = hostEnd;

  uint16_t port = out.tls ? 443 : 80;
  if (*p == ':') {
    ++p;
    if (*p < '0' || *p > '9') return out;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + static_cast<uint32_t>(*p - '0');
      if (v > 65535) return out;
      ++p;
    }
    if (v == 0) return out;
    port = static_cast<uint16_t>(v);
  }

  if (*p == '/') {
    const size_t pathLen = strlen(p);
    if (pathLen >= sizeof(out.path)) return out;
    memcpy(out.path, p, pathLen + 1);
  } else if (*p != '\0') {
    return out;
  } else {
    strcpy(out.path, "/");
  }

  out.port = port;
  out.valid = true;
  return out;
}

}  // namespace VoiceRelay
