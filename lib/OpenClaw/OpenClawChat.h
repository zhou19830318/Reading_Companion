#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

// P3 — the text half of the chat round trip: one chat.send request out, the
// chat events that answer it back in.
//
// Frame shapes are ported from AIWatch's openclaw_client.c so the gateway
// sees exactly the request it already accepts:
//
//   out  openclaw_client.c:1550
//        {"type":"req","id":"<n>","method":"chat.send",
//         "params":{"sessionKey":"crosspoint","message":"<msg>",
//                   "idempotencyKey":"<key>"}}
//   in   openclaw_client.c:486
//        {"type":"event","event":"chat",
//         "payload":{"state":"delta"|"final"|"error"|"aborted",
//                    "message":{"content":[{"text":"..."}]},
//                    "errorMessage":"..."}}
//        {"type":"res","id":"<n>","error":{"code":..,"message":..}}
//
// What is deliberately *not* ported: AIWatch prepends a ~1.5 KB instruction
// prefix and an SD card MP3 listing to every message (openclaw_client.c:
// 1457-1547). Neither exists on a reader with no speaker, and 1.5 KB would not
// fit a single frame here either. idempotencyKey keeps its original shape so
// nothing server side has to change.
//
// The one intentional divergence is sessionKey: AIWatch sends "default", a
// conversation every client shares, and that history had absorbed the
// gateway's "[assistant turn failed before producing content]" line — the
// model then copied the marker into later replies (measured: 9 occurrences in
// agent:main:default, 0 in a fresh key). "crosspoint" scopes this device to
// its own history.
//
// Pure C++ — no ESP-IDF, no Arduino — so test/openclaw_chat compiles the same
// translation unit the firmware links.
namespace OpenClaw {

// Fixed scaffolding (ids, method, sessionKey, idempotencyKey) is ~120 bytes,
// and escaping can expand a message several times over, so buildChatSendFrame
// checks the real frame instead of estimating; this is only the size above
// which it is not worth trying.
//
// 1536, not 512: a voice transcript still lands under 512, but the history
// panel's "summarize this day" prompt ships a slice of the day's turns and
// needs room for a few hundred CJK characters. Realistic content (CJK bytes
// pass escaping verbatim, only quotes/backslashes/control bytes expand) frames
// to ~1.7 KB — inside CHAT_BUF_CAP (4096); even every-byte-doubled text fits.
// Pathological all-control input can still overrun 4096 and fails closed:
// buildChatSendFrame returns 0 and the send reports Failed.
static constexpr size_t CHAT_MESSAGE_CAP = 1536;

// One buffer serves both directions: sendTXT() copies the frame onto the
// socket before it returns, so the same memory can start accumulating the
// reply the moment the send completes. Callers allocate it once per activity
// (PSRAM on device — see OpenClawActivity) instead of on the stack.
//
// 4096, not 1024: a story-style reply arrives as ONE text item of 2–3 KB
// (measured on device 2026-09-27 — delta and final frames both carried a
// single content[].text over 2.6 KB). A 1024-byte buffer truncated every such
// reply to nothing usable; the PSRAM this build has (~8 MB) makes the extra
// 3 KB per session the cheap fix. An outbound chat.send frame is bounded by
// CHAT_MESSAGE_CAP (1536) plus escaping expansion, so sends still fit with
// room to spare (buildChatSendFrame enforces the real bound either way).
static constexpr size_t CHAT_BUF_CAP = 4096;

// Writes a complete chat.send frame. Returns the number of bytes written, or
// 0 when the message is empty, longer than CHAT_MESSAGE_CAP, the key is null,
// or the frame does not fit `cap`. Never writes a partial frame.
//
// `idempotencyKey` must differ per logical send: the gateway suppresses a
// repeat of a key it has already seen, so reusing one would silently drop the
// second message.
size_t buildChatSendFrame(char* out, size_t cap, uint32_t msgId, const char* message, const char* idempotencyKey);

// True when a reply carries no usable content — only the gateway's own
// "[assistant turn failed before producing content]" line (possibly repeated)
// and whitespace, or nothing at all. A real answer anywhere in the text makes
// it false, because the gateway often prefixes a good reply with one copy.
//
// The marker is server side, not ours: AIWatch's saved chat logs contain it in
// 18 of 52 assistant turns on this gateway. Callers use this to decide whether
// a turn is worth sending again rather than showing the marker as the answer.
bool isAgentFailureReply(std::string_view text);

// What an inbound frame turned out to be, decided only once the whole document
// has been parsed — the gateway's key order is not something to depend on.
enum class ChatFrame : uint8_t {
  Ignored,  // connect/agent/other frame, or a chat frame without usable text
  Delta,    // a streamed chunk was appended to text()
  Final,    // text() was replaced by the authoritative full reply
  Error,    // error() explains why there will be no reply
  Aborted,  // the gateway cancelled the run
};

// Accumulates one reply across the frames that carry it.
//
// Storage is borrowed, not owned: the caller allocates the buffer (and frees
// it) and hands it over with attach(). That keeps this object trivial to place
// inside a heap-allocated activity without pulling in <memory>.
class ChatReply {
 public:
  ChatReply(char* storage = nullptr, size_t cap = 0) : parser_(makeCallbacks()), storage_(storage), cap_(cap) {
    reset();
  }
  // The embedded parser captures `this` in its callbacks, so a copy would keep
  // pointing back at the original.
  ChatReply(const ChatReply&) = delete;
  ChatReply& operator=(const ChatReply&) = delete;

  // Points the reply at new storage and drops everything already accumulated.
  // Call with (nullptr, 0) when the buffer is being freed.
  void attach(char* storage, size_t cap);

  // Consumes one complete inbound text frame. Returns how it classified.
  ChatFrame feed(const char* json, size_t len);

  // Drops the reply text and the per-frame staging state.
  void reset();

  // Removes every "[assistant turn failed before producing content]" marker
  // from the accumulated text, in place, together with the newline that
  // surrounded it. Returns how many were removed (0 leaves the text untouched).
  //
  // The marker is gateway noise, not an answer: AIWatch strips its own control
  // tags before drawing (app_state.c:624-640) and simply never noticed this
  // one, so its screens show it. Called before the text is logged, painted or
  // tested for emptiness — a reply that was only markers collapses to "" and
  // then falls out of isAgentFailureReply() as a failure.
  size_t stripFailureMarkers();

  // Strips the e-ink-hostile markdown emphasis the model emits, in place:
  // **bold** and *em* lose their asterisks, ``` fences and `inline code`
  // lose their backticks. Returns the number of bytes removed.
  //
  // The reply is drawn as plain wrapped text on e-ink (no rich rendering
  // until AI-01, see docs/v4.0-development-plan.md M0-3), so emphasis markers
  // reach the screen verbatim — measured 2026-09-28: "搞定！⏰ 明早 **8:00** 会叫你起床。".
  // Deliberately conservative:
  //  - a marker pair must sit on the SAME line and wrap a non-empty run
  //    (typography, bullets, lone asterisks stay);
  //  - '_' is NOT touched: _em_ is rare in this gateway's replies while
  //    snake_case identifiers and file names are common — corrupting those is
  //    worse than showing the odd underscore;
  //  - headings/list markers keep their characters — one leading '#' or '-'
  //    reads fine as plain text.
  size_t stripMarkdownMarkers();

  // Removes the emoji and symbol codepoints no panel font can draw, in place:
  // ChatHistoryFormat::isStrippableSymbolCodepoint decides (the ranges are
  // host-tested in test/chat_history). Without this every such codepoint
  // reaches the screen as a .notdef box — measured 2026-09-29: the reply
  // "搞定！⏰ …好好休息！😴" painted boxes for ⏰ (U+23F0) and 😴 (U+1F634),
  // since neither the builtin Noto/Ubuntu fonts nor the SD LXGW fallback
  // carries emoji. Called on each delta as well as on final, so the early
  // partial repaint never shows a box. Kept bytes are copied verbatim (a
  // sequence truncated by a delta boundary is never split). Returns bytes
  // removed.
  size_t stripStrippableSymbols();

  // NUL-terminated, and always safe to read (empty string when nothing has
  // arrived or there is no storage yet).
  const char* text() const { return storage_ != nullptr ? storage_ : ""; }
  size_t textSize() const { return len_; }
  // payload.errorMessage / error.message of the last Error frame.
  const char* error() const { return error_; }
  // `event` of the frame just fed — "chat", "agent", … for diagnostics.
  const char* eventName() const { return event_; }
  // True when text had to be dropped because the storage ran out. A reply
  // longer than CHAT_BUF_CAP fits only partially; the activity says so on the
  // panel ("(truncated)") instead of showing a reply that silently ends
  // mid-sentence.
  bool truncated() const { return truncated_; }

 private:
  // Non-static: the callback table carries `this` as its context, exactly as
  // SseTranscriptParser does, so each instance parses into its own storage.
  JsonCallbacks makeCallbacks();

  // SAX callbacks (free functions taking `this` as ctx). onStringPartCb is
  // the StreamingJsonParser chunk path: a single text item larger than the
  // parser's 512-byte token buffer arrives here in consecutive pieces whose
  // concatenation is the exact string (a story reply measured 2.6 KB in one
  // item — without it the item was dropped whole and the reply was empty).
  static void onKeyCb(void* ctx, const char* key, size_t len);
  static void onStringCb(void* ctx, const char* value, size_t len);
  static void onStringPartCb(void* ctx, const char* chunk, size_t len);
  static void onNumberCb(void* ctx, const char* value, size_t len);
  static void onBoolCb(void* ctx, bool value);
  static void onNullCb(void* ctx);
  static void onObjectStartCb(void* ctx);
  static void onObjectEndCb(void* ctx);
  static void onArrayStartCb(void* ctx);
  static void onArrayEndCb(void* ctx);

  void beginFrame();
  ChatFrame endFrame();
  void appendText(const char* value, size_t len);
  // Index of the first run of exactly runLen markerChar bytes at/after `from`.
  // Stops at the end of the current line unless crossLine — inline emphasis
  // never spans a '\n', while a ``` fence is defined by spanning lines.
  // NO_MARKER when there is none.
  static constexpr size_t NO_MARKER = static_cast<size_t>(-1);
  size_t markerRunAhead(size_t from, char markerChar, size_t runLen, bool crossLine) const;

  bool inPayload() const { return payloadDepth_ != 0; }
  bool inContent() const { return contentDepth_ != 0; }
  bool inError() const { return errorDepth_ != 0; }

  // Structural position. depth_ counts objects *and* arrays so each flag can be
  // closed by the matching end callback; the "… == 0 means absent" test is what
  // keeps a stray `message` key inside some other object from being mistaken
  // for payload.message.
  enum class FrameType : uint8_t { Unknown, Event, Res };
  enum class Key : uint8_t { Other, Type, Event, Payload, State, Message, Content, Text, ErrorMessage, Error };

  StreamingJsonParser parser_;

  char* storage_;
  size_t cap_;
  size_t len_ = 0;

  // Per frame, reset by beginFrame().
  size_t frameStartLen_ = 0;
  FrameType frameType_ = FrameType::Unknown;
  bool sawError_ = false;
  Key lastKey_ = Key::Other;
  uint8_t depth_ = 0;
  uint8_t payloadDepth_ = 0;
  uint8_t messageDepth_ = 0;
  uint8_t contentDepth_ = 0;
  uint8_t errorDepth_ = 0;

  // Sticky across frames: one trip flag is enough to say "this reply is not
  // complete", and the caller prints it once at the end.
  bool truncated_ = false;

  char event_[16] = {};
  char state_[16] = {};
  char error_[160] = {};
};

}  // namespace OpenClaw
