#pragma once

#include <VoiceRelayCodec.h>
#include <WebSocketsClient.h>

#include <cstddef>
#include <cstdint>
#include <memory>

// Device-only transport for the Voice Relay protocol: the WebSocket lifecycle,
// the header auth, the audio.end/cancel control flow, and the timers. The wire
// bytes themselves live in VoiceRelayCodec (host-tested); this class exists so
// that test/voice_relay_codec stays free of WebSocketsClient.
//
// Task model: start/poll/sendAudio/finishAudio/close are all called from the
// activity's task; ws_->loop() dispatches callbacks on that same task, so no
// locking is needed. The two rules that keep that true:
//   * never send from inside the WS callback (armed flags flush in poll()),
//   * the event notifier must not touch this object (set a flag, repaint).
//
// Session lifecycle: a finished (Done) session keeps its socket — the next
// start() arms a fresh session on it (the server replaces the finished one),
// which is what keeps the dial out of the capture window. A failed (Failed)
// transport is terminal: recovery is the activity's decision (retry budget or
// the legacy STT + OpenClaw path), so the library's own auto-redial is
// disabled.
namespace VoiceRelay {

class Client {
 public:
  // Device-observed budgets. The server closes with an error event when it
  // hits its own limits first; these are the half-dead-link backstops.
  static constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
  static constexpr uint32_t START_TIMEOUT_MS = 5000;
  // Comfortably past the server's 20 s SESSION_TIMEOUT: a healthy server
  // always answers (session.done or error) before this fires.
  static constexpr uint32_t DONE_TIMEOUT_MS = 30000;
  // Same RFC 6455 heartbeat as OpenClawSession: keeps NAT mappings fed and
  // tears down a pong-silent link instead of letting it swallow the turn.
  static constexpr uint32_t PING_INTERVAL_MS = 25000;
  static constexpr uint32_t PONG_TIMEOUT_MS = 5000;
  static constexpr uint8_t PONG_TIMEOUT_COUNT = 3;
  // WebSocketsClient throttles every dial — the FIRST one included — by this
  // interval (loop() returns while millis() - _lastConnectionFail < interval).
  // A huge value therefore never dials at all; keep it small so a lost SYN is
  // retried inside ConnectTimeout. Teardown stays ours: any drop fails the
  // round and releaseSocket() deletes the client, so no silent redial.
  static constexpr uint32_t REDIAL_INTERVAL_MS = 1000;

  static constexpr size_t TOKEN_CAP = 200;                // "Bearer " + 192 + NUL
  static constexpr size_t HEADERS_CAP = 128;              // X-Device-Id + X-Voice-Protocol
  static constexpr size_t FRAME_CAP = CONTROL_FRAME_CAP;  // largest control frame

  enum class State : uint8_t { Idle, Connecting, Starting, Active, Done, Failed };

  // Why this client gave up, for State::Failed. Server-reported failures
  // arrive as Error events instead (lastEvent()), and leave this None.
  enum class TransportError : uint8_t {
    None,
    BadConfig,  // unparsable URL or too-long token/device id
    NoWifi,
    Oom,
    ConnectTimeout,  // dialed, no WStype_CONNECTED
    SocketLost,      // upgrade refused or the socket dropped mid-turn
    StartTimeout,    // session.start sent, no session.started
    DoneTimeout,     // audio.end sent, no session.done
  };

  using EventFn = void (*)(void* ctx, const Event& event);

  Client() = default;
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Fires from inside the WS dispatch for every parsed relay event. Must only
  // set a flag / repaint — never call back into this object.
  void setEventNotifier(void* ctx, EventFn fn) {
    notifyCtx_ = ctx;
    notifyFn_ = fn;
  }

  // Arms a round: validates url/token/deviceId, then either dials or (warm
  // Connecting / Done with an open socket) arms on the socket already on
  // hand — arming on an open socket writes no dial, so no handshake lands in
  // the capture window. False means the round was never attempted or cannot
  // proceed here; transportError() says why (state is Failed on the cold
  // paths, which the activity turns into the legacy fallback). `mode` is the
  // upstream pipeline ("relay" / "dictation"); nullptr keeps "relay".
  bool start(const char* url, const char* token, const char* deviceId, const char* mode = nullptr);
  // Opens the socket (TCP connect + upgrade) without arming a session, so the
  // blocking dial runs before any microphone is live: poll() drives it, the
  // next start() adopts it as warm, and a failure parks quietly back at Idle
  // — a warm-up miss must never poison a round. Rationale: the ~0.9 s dial
  // starves the 8 ms I2S DMA ring of any loop pass that runs inside a
  // capture, and the first second of every relay round never reached the PCM
  // buffer (measured 2026-10-17: server frame counts trailed each capture by
  // exactly the handshake window, droppedFrames=0).
  bool prepare(const char* url, const char* token, const char* deviceId);
  // Upgrade finished: the socket can carry a session right now (warm or
  // mid-session).
  bool connected() const { return wsOpen_; }
  // A dial is in flight (socket opened, upgrade not finished): a poll() pass
  // now blocks for the connect. Callers with a live microphone defer polling
  // until it clears — the PCM flush after the capture already handles a
  // session that only comes up once the mic is off.
  bool dialing() const { return state_ == State::Connecting && !wsOpen_; }
  // Pumps the socket, flushes armed sends, enforces the timers. Call every
  // loop pass between start() and close().
  void poll();
  // Best-effort session.cancel while Active, then disconnect and release.
  // Safe in any state; a no-op after the session already finished.
  void close();

  State state() const { return state_; }
  TransportError transportError() const { return transportError_; }
  // Most recent parsed event; type Unknown until the first frame arrives.
  const Event& lastEvent() const { return lastEvent_; }
  const char* requestId() const { return requestId_; }
  const char* sessionId() const { return sessionId_; }
  // Device-observed stage timings, all from this device's millis(): connect is
  // dial → WS open; the rest are relative to the session.start send. -1 when
  // the stage did not happen. The server's own numbers ride session.done
  // (lastEvent().latency).
  const Latency& localLatency() const { return localLatency_; }

  // Feeds recorded PCM (any length; the framer carries the remainder). Emits
  // one binary WS message per FRAME_BYTES. False unless the server has started
  // the session (dropped frames never split a frame across sends).
  bool sendAudio(const uint8_t* pcm, size_t len);
  // Sends audio.end so the server runs ASR-final + the LLM. False when no
  // session is Active or the frame could not be written.
  bool finishAudio(const char* reason = "vad_silence");

 private:
  void onWsEvent(WStype_t type, uint8_t* payload, size_t length);
  void handleText(const char* json, size_t len);
  void handleEvent(const Event& event);
  void fail(TransportError error);
  // Shared front half of start()/prepare(): validate, reset flags, allocate
  // the socket, begin(). `warm` selects the failure mode: a warm miss sets
  // transportError() and stays Idle, a cold miss fails the round.
  bool setUp(const char* url, const char* token, const char* deviceId, bool warm);
  // Per-session reset + session.start build (fresh request id). Runs on every
  // arm, cold or reused socket.
  bool buildRound(const char* mode);
  void sendStart();
  void sendAudioStart();
  void releaseSocket();
  static void frameSink(const uint8_t* frame, size_t len, void* ctx);
  uint32_t elapsed(uint32_t sinceMs) const { return millis() - sinceMs; }

  State state_ = State::Idle;
  TransportError transportError_ = TransportError::None;
  std::unique_ptr<WebSocketsClient> ws_;
  ParsedUrl url_;
  char deviceId_[DEVICE_ID_CAP] = {};
  char requestId_[REQUEST_ID_CAP] = {};
  char sessionId_[SESSION_ID_CAP] = {};
  char startFrame_[FRAME_CAP] = {};
  size_t startFrameLen_ = 0;
  // Armed by the CONNECTED callback; flushed by the same poll() pass (the WS
  // dispatch must not send).
  bool startPending_ = false;
  // Armed once session.started arrives: audio.start is what makes the server
  // open its ASR upstream, so it must precede every binary audio frame.
  bool audioStartPending_ = false;
  // Upgrade finished (WStype_CONNECTED seen); gates sendStart() and separates
  // "dial never opened" from "dial opened, server never answered".
  bool wsOpen_ = false;
  // Set by prepare(): this socket is a warm-up, not a round. A warm socket
  // that drops parks quietly at Idle instead of failing anything, and it
  // arms no startPending_ of its own (start() does when it adopts it).
  bool warm_ = false;
  // A warm socket closed from inside its own dispatch: the client cannot be
  // freed from within ws_->loop(), so the next poll() pass releases it.
  bool releasePending_ = false;
  // audio.end is out; the only remaining event is session.done (or an error).
  bool ending_ = false;
  PcmFramer framer_;
  Event lastEvent_{};
  Latency localLatency_{};

  uint32_t dialedAtMs_ = 0;
  uint32_t startSentAtMs_ = 0;
  uint32_t endSentAtMs_ = 0;

  void* notifyCtx_ = nullptr;
  EventFn notifyFn_ = nullptr;
};

}  // namespace VoiceRelay
