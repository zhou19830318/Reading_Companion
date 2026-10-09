#include "OpenClawChat.h"

#include <Utf8.h>

#include <cstdio>
#include <cstring>

#include "ChatHistoryFormat.h"

namespace OpenClaw {
namespace {

bool strEq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

bool tokenEq(const char* value, size_t len, const char* expected) {
  return std::strlen(expected) == len && std::memcmp(value, expected, len) == 0;
}

// JSON string escaping for the outbound frame. Only the characters JSON
// requires are touched: quote, backslash, \n \r \t get their short form and
// the remaining control characters get \u00XX. Everything else passes through
// byte for byte — but only when it belongs to a well-formed UTF-8 sequence.
// A text frame that is not valid UTF-8 makes the gateway drop the socket with
// no reply and no close frame (RFC 6455 §8.1), which takes the whole round
// trip down instead of merely garbling this message, so an ill-formed byte is
// dropped here rather than put on the wire.
bool appendEscaped(char* out, size_t cap, size_t& pos, const char* s) {
  static constexpr char HEX[] = "0123456789abcdef";
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  while (*p != '\0') {
    const size_t seqLen = utf8ValidSequenceLen(p);
    if (seqLen == 0) {
      ++p;
      continue;
    }
    char tmp[6];
    size_t n = seqLen;
    if (seqLen == 1) {
      const unsigned char c = *p;
      switch (c) {
        case '"':
          tmp[0] = '\\';
          tmp[1] = '"';
          n = 2;
          break;
        case '\\':
          tmp[0] = '\\';
          tmp[1] = '\\';
          n = 2;
          break;
        case '\n':
          tmp[0] = '\\';
          tmp[1] = 'n';
          n = 2;
          break;
        case '\r':
          tmp[0] = '\\';
          tmp[1] = 'r';
          n = 2;
          break;
        case '\t':
          tmp[0] = '\\';
          tmp[1] = 't';
          n = 2;
          break;
        default:
          if (c < 0x20) {
            tmp[0] = '\\';
            tmp[1] = 'u';
            tmp[2] = '0';
            tmp[3] = '0';
            tmp[4] = HEX[c >> 4];
            tmp[5] = HEX[c & 0x0F];
            n = 6;
          } else {
            tmp[0] = static_cast<char>(c);
            n = 1;
          }
          break;
      }
    } else {
      std::memcpy(tmp, p, seqLen);
    }
    if (pos + n > cap) return false;
    std::memcpy(out + pos, tmp, n);
    pos += n;
    p += seqLen;
  }
  return true;
}

}  // namespace

int firstIllFormedUtf8(const char* message) {
  if (message == nullptr) return -1;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(message);
  int offset = 0;
  while (*p != '\0') {
    const size_t seqLen = utf8ValidSequenceLen(p);
    if (seqLen == 0) return offset;
    p += seqLen;
    offset += static_cast<int>(seqLen);
  }
  return -1;
}

size_t buildChatSendFrame(char* out, size_t cap, uint32_t msgId, const char* message, const char* idempotencyKey) {
  if (out == nullptr || cap == 0 || message == nullptr || message[0] == '\0' || idempotencyKey == nullptr) {
    return 0;
  }
  if (std::strlen(message) > CHAT_MESSAGE_CAP) return 0;

  size_t pos = 0;
  auto put = [&](const char* s) {
    const size_t n = std::strlen(s);
    if (pos + n > cap) return false;
    std::memcpy(out + pos, s, n);
    pos += n;
    return true;
  };

  char id[12];
  const int idLen = std::snprintf(id, sizeof(id), "%lu", static_cast<unsigned long>(msgId));
  if (idLen <= 0 || static_cast<size_t>(idLen) >= sizeof(id)) return 0;

  if (!put("{\"type\":\"req\",\"id\":\"")) return 0;
  if (pos + static_cast<size_t>(idLen) > cap) return 0;
  std::memcpy(out + pos, id, static_cast<size_t>(idLen));
  pos += static_cast<size_t>(idLen);
  if (!put("\",\"method\":\"chat.send\",\"params\":{\"sessionKey\":\"crosspoint\",\"message\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, message)) return 0;
  if (!put("\",\"idempotencyKey\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, idempotencyKey)) return 0;
  if (!put("\"}}")) return 0;
  return pos;
}

// ── isAgentFailureReply ─────────────────────────────────────────────────────

namespace {
// Verbatim from the gateway's agent runner. Repeated copies are stacked with
// newlines when the model is retried inside one turn (measured: five in a
// single reply).
constexpr std::string_view AGENT_FAILURE_MARKER = "[assistant turn failed before producing content]";
constexpr std::string_view WHITESPACE = " \t\r\n";
}  // namespace

bool isAgentFailureReply(std::string_view text) {
  // Scan without building a copy: count every byte that is neither part of the
  // marker nor whitespace. A good answer sitting after the markers therefore
  // still counts, and so does one sitting in front of them.
  size_t i = 0;
  size_t content = 0;
  while (i < text.size()) {
    if (text.compare(i, AGENT_FAILURE_MARKER.size(), AGENT_FAILURE_MARKER) == 0) {
      i += AGENT_FAILURE_MARKER.size();
      continue;
    }
    if (WHITESPACE.find(text[i]) == std::string_view::npos) ++content;
    ++i;
  }
  return content == 0;
}

// ── ChatReply ──────────────────────────────────────────────────────────────

size_t ChatReply::stripFailureMarkers() {
  if (storage_ == nullptr || len_ < AGENT_FAILURE_MARKER.size()) return 0;
  size_t removed = 0;
  size_t r = 0;
  size_t w = 0;
  while (r < len_) {
    if (len_ - r >= AGENT_FAILURE_MARKER.size() &&
        std::memcmp(storage_ + r, AGENT_FAILURE_MARKER.data(), AGENT_FAILURE_MARKER.size()) == 0) {
      // The marker sits on its own line: drop the newline that ended the
      // previous line and the one that ended this one, so the surrounding
      // lines meet where the marker was instead of leaving blank rows behind.
      if (w > 0 && storage_[w - 1] == '\n') --w;
      r += AGENT_FAILURE_MARKER.size();
      if (r < len_ && storage_[r] == '\n') ++r;
      ++removed;
      continue;
    }
    storage_[w++] = storage_[r++];
  }
  if (removed == 0) return 0;
  len_ = w;
  storage_[w] = '\0';
  return removed;
}

size_t ChatReply::markerRunAhead(size_t from, char markerChar, size_t runLen, bool crossLine) const {
  size_t i = from;
  while (i < len_) {
    if (storage_[i] == '\n') {
      // Inline emphasis never spans a line; a ``` fence is DEFINED to span
      // lines, so only its search may continue past the newline.
      if (!crossLine) return NO_MARKER;
      ++i;
      continue;
    }
    if (storage_[i] != markerChar) {
      ++i;
      continue;
    }
    // Width of the marker run starting here.
    size_t runWidth = 1;
    while (i + runWidth < len_ && storage_[i + runWidth] == markerChar) ++runWidth;
    if (runWidth == runLen) return i;
    // A longer run (e.g. "***") can never be the exact close for this opener
    // (its leading bytes would have matched first); skip past it whole so a
    // later single asterisk still closes a single-asterisk opener.
    i += runWidth;
  }
  return NO_MARKER;
}

size_t ChatReply::stripMarkdownMarkers() {
  if (storage_ == nullptr || len_ == 0) return 0;
  size_t removed = 0;
  size_t r = 0;
  size_t w = 0;
  while (r < len_) {
    const char c = storage_[r];
    if (c == '*' || c == '`') {
      // Full width of the contiguous marker run at the read cursor (uncapped:
      // the close search must start beyond ALL of it, or the second asterisk
      // of an unclosed "**" would be taken for a close).
      size_t runWidth = 1;
      while (r + runWidth < len_ && storage_[r + runWidth] == c) ++runWidth;
      bool paired = false;
      // Marker flavours, widest first. A ** opener only closes with ** (never
      // fall back to a single *: "**8:00 and 2*3" would pair across flavours);
      // a ` opener may fall back to a shorter run (fence → inline code).
      const size_t maxRun = runWidth < 3 ? runWidth : 3;
      const size_t minRun = (c == '*' && runWidth >= 2) ? 2 : 1;
      for (size_t run = maxRun; run >= minRun; --run) {
        // Only a ``` fence spans lines; inline emphasis is line-bound.
        const size_t close = markerRunAhead(r + runWidth, c, run, run == 3);
        if (close != NO_MARKER) {
          // Drop the opener, keep the content between the markers byte for
          // byte, drop the closer. The forward copy is safe: w <= r always,
          // so the destination never overtakes the source.
          removed += run;                                   // opener
          r += run;                                         // skip the opener
          while (r < close) storage_[w++] = storage_[r++];  // keep content
          removed += run;                                   // closer
          r = close + run;                                  // skip the closer
          paired = true;
          break;
        }
      }
      if (paired) continue;  // r already advanced past the closer
      // No flavour closed: the whole run is literal text, consumed whole so
      // its second byte is not re-examined as a smaller opener (the second
      // asterisk of an unclosed "**" must not pair with a later single *).
      while (runWidth-- > 0) storage_[w++] = storage_[r++];
      continue;
    }
    storage_[w++] = storage_[r++];
  }
  if (removed == 0) return 0;
  len_ = w;
  storage_[w] = '\0';
  return removed;
}

size_t ChatReply::stripStrippableSymbols() {
  if (storage_ == nullptr || len_ == 0) return 0;
  const size_t removed = ChatHistoryFormat::stripStrippableSymbols(storage_, len_);
  len_ -= removed;
  return removed;
}

JsonCallbacks ChatReply::makeCallbacks() {
  return {this,
          &ChatReply::onKeyCb,
          &ChatReply::onStringCb,
          &ChatReply::onNumberCb,
          &ChatReply::onBoolCb,
          &ChatReply::onNullCb,
          &ChatReply::onObjectStartCb,
          &ChatReply::onObjectEndCb,
          &ChatReply::onArrayStartCb,
          &ChatReply::onArrayEndCb,
          &ChatReply::onStringPartCb};
}

void ChatReply::attach(char* storage, size_t cap) {
  storage_ = storage;
  cap_ = cap;
  reset();
}

void ChatReply::reset() {
  len_ = 0;
  truncated_ = false;
  event_[0] = '\0';
  state_[0] = '\0';
  error_[0] = '\0';
  if (storage_ != nullptr && cap_ > 0) storage_[0] = '\0';
  beginFrame();
}

void ChatReply::beginFrame() {
  frameStartLen_ = len_;
  frameType_ = FrameType::Unknown;
  sawError_ = false;
  lastKey_ = Key::Other;
  depth_ = 0;
  payloadDepth_ = 0;
  messageDepth_ = 0;
  contentDepth_ = 0;
  errorDepth_ = 0;
}

ChatFrame ChatReply::feed(const char* json, size_t len) {
  if (json == nullptr || len == 0 || storage_ == nullptr || cap_ == 0) return ChatFrame::Ignored;
  beginFrame();
  parser_.reset();
  parser_.feed(json, len);
  return endFrame();
}

ChatFrame ChatReply::endFrame() {
  auto revert = [&]() {
    len_ = frameStartLen_;
    storage_[len_] = '\0';
  };

  if (frameType_ == FrameType::Event && strEq(event_, "chat")) {
    if (strEq(state_, "delta")) {
      // text() already holds this frame's chunk in the right place, appended
      // after everything earlier deltas produced.
      return ChatFrame::Delta;
    }
    if (strEq(state_, "final")) {
      // The final message is authoritative: whatever this frame emitted *is*
      // the whole reply, so it replaces the accumulated deltas. When the frame
      // emitted nothing usable — empty content, or a text item too long for
      // StreamingJsonParser to carry — keep the deltas instead of blanking a
      // reply we already have.
      if (len_ > frameStartLen_) {
        const size_t tail = len_ - frameStartLen_;
        std::memmove(storage_, storage_ + frameStartLen_, tail);
        len_ = tail;
        storage_[len_] = '\0';
      }
      return ChatFrame::Final;
    }
    // error / aborted / unknown state carry no message text worth keeping.
    revert();
    if (strEq(state_, "error")) return ChatFrame::Error;
    if (strEq(state_, "aborted")) return ChatFrame::Aborted;
    return ChatFrame::Ignored;
  }

  revert();
  if (frameType_ == FrameType::Res) return sawError_ ? ChatFrame::Error : ChatFrame::Ignored;
  return ChatFrame::Ignored;
}

void ChatReply::appendText(const char* value, size_t len) {
  if (storage_ == nullptr || cap_ == 0 || len == 0) return;
  // One byte is reserved for the terminator, so the usable payload is cap_-1.
  const size_t room = cap_ - 1 - len_;
  const size_t n = len < room ? len : room;
  if (n > 0) {
    std::memcpy(storage_ + len_, value, n);
    len_ += n;
    storage_[len_] = '\0';
  }
  if (n < len) truncated_ = true;
}

void ChatReply::onStringPartCb(void* ctx, const char* chunk, size_t len) {
  auto* self = static_cast<ChatReply*>(ctx);
  // Only reply text takes the chunk path: the parser hands every string over
  // 511 bytes here, and the only such string in a chat frame is the answer
  // itself (a key, event name or error message that long is not dispatchable
  // anyway).
  if (self->lastKey_ == Key::Text && self->inPayload() && self->inContent()) self->appendText(chunk, len);
}

void ChatReply::onKeyCb(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<ChatReply*>(ctx);
  auto is = [&](const char* name) { return tokenEq(key, len, name); };
  if (is("type")) {
    self->lastKey_ = Key::Type;
  } else if (is("event")) {
    self->lastKey_ = Key::Event;
  } else if (is("payload")) {
    self->lastKey_ = Key::Payload;
  } else if (is("state")) {
    self->lastKey_ = Key::State;
  } else if (is("content")) {
    self->lastKey_ = Key::Content;
  } else if (is("text")) {
    self->lastKey_ = Key::Text;
  } else if (is("errorMessage")) {
    self->lastKey_ = Key::ErrorMessage;
  } else if (is("error")) {
    self->lastKey_ = Key::Error;
  } else if (is("message")) {
    self->lastKey_ = Key::Message;
  } else {
    self->lastKey_ = Key::Other;
  }
}

void ChatReply::onStringCb(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ChatReply*>(ctx);
  switch (self->lastKey_) {
    case Key::Type:
      // `type` only appears at the root, and payload is false until we have
      // descended into it, so the guard is about ordering rather than depth.
      if (!self->inPayload()) {
        if (tokenEq(value, len, "event")) {
          self->frameType_ = FrameType::Event;
        } else if (tokenEq(value, len, "res")) {
          self->frameType_ = FrameType::Res;
        }
      }
      break;
    case Key::Event:
      if (!self->inPayload()) {
        const size_t n = len < sizeof(self->event_) - 1 ? len : sizeof(self->event_) - 1;
        std::memcpy(self->event_, value, n);
        self->event_[n] = '\0';
      }
      break;
    case Key::State:
      if (self->inPayload()) {
        const size_t n = len < sizeof(self->state_) - 1 ? len : sizeof(self->state_) - 1;
        std::memcpy(self->state_, value, n);
        self->state_[n] = '\0';
      }
      break;
    case Key::ErrorMessage:
      if (self->inPayload()) {
        const size_t n = len < sizeof(self->error_) - 1 ? len : sizeof(self->error_) - 1;
        std::memcpy(self->error_, value, n);
        self->error_[n] = '\0';
      }
      break;
    case Key::Message:
      // payload.message is an object (never a string); a string under the
      // root-level `error` object is the only other `message` on the wire.
      if (!self->inPayload() && self->inError()) {
        const size_t n = len < sizeof(self->error_) - 1 ? len : sizeof(self->error_) - 1;
        std::memcpy(self->error_, value, n);
        self->error_[n] = '\0';
      }
      break;
    case Key::Text:
      if (self->inPayload() && self->inContent()) self->appendText(value, len);
      break;
    default:
      break;
  }
}

void ChatReply::onNumberCb(void*, const char*, size_t) {}
void ChatReply::onBoolCb(void*, bool) {}
void ChatReply::onNullCb(void*) {}

void ChatReply::onObjectStartCb(void* ctx) {
  auto* self = static_cast<ChatReply*>(ctx);
  ++self->depth_;
  switch (self->lastKey_) {
    case Key::Payload:
      if (self->payloadDepth_ == 0) self->payloadDepth_ = self->depth_;
      break;
    case Key::Message:
      if (self->inPayload() && self->messageDepth_ == 0) self->messageDepth_ = self->depth_;
      break;
    case Key::Error:
      if (!self->inPayload() && self->errorDepth_ == 0) {
        self->errorDepth_ = self->depth_;
        self->sawError_ = true;
      }
      break;
    default:
      break;
  }
}

void ChatReply::onObjectEndCb(void* ctx) {
  auto* self = static_cast<ChatReply*>(ctx);
  if (self->payloadDepth_ == self->depth_) self->payloadDepth_ = 0;
  if (self->messageDepth_ == self->depth_) self->messageDepth_ = 0;
  if (self->errorDepth_ == self->depth_) self->errorDepth_ = 0;
  if (self->depth_ > 0) --self->depth_;
}

void ChatReply::onArrayStartCb(void* ctx) {
  auto* self = static_cast<ChatReply*>(ctx);
  ++self->depth_;
  if (self->lastKey_ == Key::Content && self->inPayload() && self->messageDepth_ != 0 && self->contentDepth_ == 0) {
    self->contentDepth_ = self->depth_;
  }
}

void ChatReply::onArrayEndCb(void* ctx) {
  auto* self = static_cast<ChatReply*>(ctx);
  if (self->contentDepth_ == self->depth_) self->contentDepth_ = 0;
  if (self->depth_ > 0) --self->depth_;
}

}  // namespace OpenClaw
