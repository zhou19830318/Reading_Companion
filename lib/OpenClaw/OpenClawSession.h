#pragma once

#include <CronListFormat.h>
#include <OpenClawChat.h>
#include <OpenClawHandshake.h>
#include <WebSocketsClient.h>

#include <cstddef>
#include <cstdint>
#include <memory>

// The OpenClaw conversation, extracted from OpenClawActivity (P3) so the voice
// screen (P3-S Phase 1) shares one transport instead of a second copy drifting
// (docs/voice-openclaw-port-plan.md §8.2).
//
// What this class owns: config loading and validation (CloudConfig, the same
// URI parsing OpenClawActivity does), the WebSocket lifecycle, the handshake,
// chat.send with the failure-marker retry, the reply accumulation, and the
// device-token persistence flag. What it does not own: any UI — the caller
// renders from phase(), status(), and reply text, and drives it with poll().
//
// Thread/repaint discipline is inherited from the activity: poll() pumps the
// socket on the caller's task; requestUpdate() callbacks fire from inside the
// WebSocket dispatch, so they must not call back into the socket.
//
// Device-only: WebSocketsClient + CloudConfig. The protocol halves it drives
// (Handshake, ChatReply) are the host-tested ones.
namespace OpenClaw {

// CronJob / CronParse and the reply parser live in CronListFormat.h — pure,
// host-tested (test/cron_list), and readable by the workbench without
// pulling this device-only header in.

class Session {
 public:
  static constexpr size_t HOST_SIZE = 64;
  static constexpr size_t PATH_SIZE = 96;
  static constexpr size_t PROTO_SIZE = 40;
  static constexpr size_t CAPATH_SIZE = 96;
  static constexpr size_t CA_PEM_SIZE = 2400;
  // One line of state — data, not translated text.
  static constexpr size_t STATUS_SIZE = 96;
  // Bounded: the workbench card shows at most MAX_CRON_JOBS rows and the
  // parse loop must not grow the heap per job.
  static constexpr size_t MAX_CRON_JOBS = 8;
  static constexpr uint32_t RECONNECT_MS = 15000;
  // RFC 6455 heartbeat, armed via WebSocketsClient::enableHeartbeat(). The
  // gateway closed an idle socket after ~55 s (2026-09-27 session log); a ping
  // every 25 s keeps NAT mappings and the gateway's idle timer fed, and a pong
  // silence longer than PONG_TIMEOUT_MS x PONG_TIMEOUT_COUNT tears the socket
  // down so the reconnect path below takes over instead of a half-dead TCP
  // link swallowing the next chat.send.
  static constexpr uint32_t PING_INTERVAL_MS = 25000;
  static constexpr uint32_t PONG_TIMEOUT_MS = 5000;
  static constexpr uint8_t PONG_TIMEOUT_COUNT = 3;
  // How long a chat.send may stay without a sign of life before poll() reports
  // Timeout so the caller can say so. It is an *inactivity* clock, not a total
  // one: every streamed delta restarts it (Session::handleChatFrame), so a
  // long tool turn — asking for an alarm runs a tool round trip — may exceed
  // this in total as long as it keeps moving. The socket stays open; a slow
  // agent may still answer afterwards. 120 s: a plain turn measured 41 s with
  // the gateway silent for the first 39.7 s of it, and a tool turn adds a
  // second model call on top of that.
  static constexpr uint32_t CHAT_TIMEOUT_MS = 120000;
  // One send plus one re-send when the gateway answers with only its
  // "[assistant turn failed before producing content]" marker.
  static constexpr uint8_t MAX_CHAT_ATTEMPTS = 2;
  // Reconnect-after-loss budget. Each loss re-runs the full handshake, so a
  // flapping gateway must not spin the radio forever; at MAX_RECONNECTS the
  // session settles into Failed and the user restarts it deliberately. The
  // delay grows RECONNECT_MS x attempt (15 s, 30 s, 45 s).
  static constexpr uint8_t MAX_RECONNECTS = 3;

  // Why a chat.send never produced an answer worth showing.
  enum class SendOutcome : uint8_t { Sent, Failed, NoReply, AgentFailed, Aborted };

  // Queues a cron.list. Transmission happens in poll(): the WS callback must
  // not sendTXT (same rule as the reconnect dial and the retry path).
  void requestCronList();
  // Last answer from the gateway, valid for the lifetime of the firmware.
  // Read by the workbench, which owns no Session instance.
  // Out-of-line for the same cppcheck reason as gatewayUptimeMs().
  static const CronJob* cronJobs();
  static size_t cronJobCount();
  // False until the gateway has answered at least once (the card then shows
  // its empty state rather than an out-of-date list).
  static bool cronValid();

  Session() = default;
  ~Session();

  // Process-wide instance (L3: the workbench status cards and the voice
  // activity share one gateway connection instead of each owning one).
  static Session& instance();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Repaint hook: called from inside the WebSocket dispatch when something
  // user-visible changed. Function pointer + context rather than
  // std::function (no heap, no template bloat — the activity passes `this`).
  // The callback must only request a repaint, never touch the socket.
  void setNotifier(void* ctx, void (*fn)(void*)) {
    notifyCtx_ = ctx;
    notifyFn_ = fn;
  }

  // ── lifecycle ────────────────────────────────────────────────────────
  // Loads CloudConfig and validates it. False means "never dial" — reason()
  // carries the translated string id the caller should show.
  bool loadConfig();
  // Arms the connect; the blocking TCP/TLS/upgrade runs on the next poll().
  // False when there is no network or the clock cannot be fixed for TLS.
  bool startConnect();
  // Pumps the socket and runs the deferred actions. Call every loop pass.
  // `onUpdate` fires when something user-visible changed (e-ink repaint).
  void poll();
  // Closes the socket and releases the CA buffer. The chat buffer is freed by
  // the destructor so a reply can be read out after close.
  void close();

  // ── state for the UI ─────────────────────────────────────────────────
  enum class State : uint8_t { Idle, Connecting, Handshake, Challenge, Connected, Failed };
  State state() const { return state_; }
  // True while a lost connection is being re-established inside poll()
  // (socket re-dial + fresh handshake). The UI keeps the session visible but
  // shows this instead of declaring the round trip dead.
  bool reconnecting() const { return reconnecting_; }
  // Last-known gateway link state, readable by code that does not own a
  // Session (the workbench status card reads it from the render task while
  // the voice session lives and dies with its activity). A benign flag, the
  // same pattern as WorkbenchActivity::clockValidAtRender: a lost race shows
  // one stale frame until the next socket event. Set on authentication,
  // cleared on disconnect/failure/close.
  static bool linkUp() { return linkUp_; }
  // Gateway-reported uptime, in milliseconds: the value the authenticated
  // response carries in payload.snapshot.uptimeMs — the field AIWatch_Ver2.0
  // renders as its dashboard's 运行时间, and the only place a current gateway
  // reports uptime at all (its periodic `health` event has no uptime key;
  // measured against a live gateway 2026-10-02). The gateway re-reports only
  // on (re)connect, so this getter adds the elapsed local time since that
  // frame, which keeps the workbench's seconds ticking between handshakes.
  // Returns 0 until an authenticated frame with a snapshot has landed.
  // Defined in the .cpp, not inline: a getter that is visible in the header
  // lets cppcheck constant-fold its result to the in-class initialiser and
  // then report every caller's branch as "always true/false".
  static uint64_t gatewayUptimeMs();

  // ── poll-time deferred work, surfaced for the UI ─────────────────────

  // Translated failure line (static table entry), null before any failure.
  const char* reason() const { return reason_; }
  // Raw status: host:port scheme, last frame echo, send/reply progress.
  const char* status() const { return statusLine_; }
  // Last chat error / gateway rejection (already redacted by the frame path).
  const char* lastError() const { return lastError_[0] != '\0' ? lastError_ : nullptr; }
  // `openclaw devices approve <id>` when the gateway answered NOT_PAIRED.
  const char* approveCommand() const { return approveCmd_[0] != '\0' ? approveCmd_ : nullptr; }
  // The accumulated reply. Borrowed pointer, valid until the next send or
  // close(); empty string when nothing has arrived.
  const char* replyText() const { return reply_.text(); }
  size_t replySize() const { return reply_.textSize(); }
  bool replyTruncated() const { return reply_.truncated(); }
  // Set from the moment a chat.send is on the wire until a final/error/abort
  // or the CHAT_TIMEOUT_MS elapses. Repaint hint for the caller.
  bool awaitingReply() const { return awaitingReply_; }
  // Milliseconds since the outstanding send last saw life (the send itself or
  // the last streamed delta); 0 when nothing is outstanding. Drives the
  // "waiting N/120 s" read-out and restarts on a failure-marker re-send.
  uint32_t awaitingMs() const;

  // ── chat ─────────────────────────────────────────────────────────────
  // Sends one chat.send. Returns the immediate verdict; an outcome of Sent
  // means reply accumulation has started and poll() will finish the round
  // trip. The failure-marker retry is internal and reported via outcome().
  SendOutcome sendMessage(const char* message);
  // Verdict of the most recent send, final once awaitingReply() is false.
  SendOutcome outcome() const { return outcome_; }

 private:
  void fail(const char* reason);
  void notify() {
    if (notifyFn_ != nullptr) notifyFn_(notifyCtx_);
  }
  bool allocChatBuffer();
  void freeChatBuffer();
  bool loadCaBuffer();
  void onWsEvent(WStype_t type, uint8_t* payload, size_t length);
  void handleChatFrame(const char* json, size_t len);
  // Steals non-chat traffic (gateway health/uptime, cron.list replies) off
  // the shared socket before the chat parser sees it. Returns true when the
  // frame was consumed; never triggers a repaint.
  bool sniffSystemFrame(const char* json, size_t len);
  // Parses a cron.list reply frame into the static cache above via
  // CronListFormat::parseCronList (the host-tested half).
  void storeCronJobs(const char* json, size_t len);
  void finishSend(SendOutcome outcome, const char* logLine);
  // Re-arm the same socket object after a loss: the library re-dials on its
  // own loop(), the handshake starts over on the next WStype_CONNECTED.
  bool redial();
  // True when the socket dropped and poll() should bring it back.
  void scheduleReconnect();

  State state_ = State::Idle;
  inline static bool linkUp_ = false;
  // Uptime as of the last frame that carried it (connect snapshot, or a
  // health event on a gateway that sends one) and the local millis() stamp of
  // that receipt. gatewayUptimeMs() extrapolates between the two.
  inline static uint64_t gatewayUptimeBaseMs_ = 0;
  inline static uint32_t gatewayUptimeSeenMs_ = 0;
  inline static CronJob cronJobs_[MAX_CRON_JOBS] = {};
  inline static size_t cronJobCount_ = 0;
  inline static bool cronValid_ = false;
  const char* reason_ = nullptr;
  char statusLine_[STATUS_SIZE] = {};
  char lastError_[160] = {};
  char approveCmd_[96] = {};

  char host_[HOST_SIZE] = {};
  char path_[PATH_SIZE] = "/";
  char protocol_[PROTO_SIZE] = "v2.openclaw.io";
  char caPath_[CAPATH_SIZE] = {};
  std::unique_ptr<char[]> caPem_;
  bool caLoaded_ = false;
  uint16_t port_ = 0;
  bool tls_ = true;

  bool pendingConnect_ = false;
  bool persistDeviceToken_ = false;
  // Armed inside the WS callback; executed in poll() — no sendTXT under the
  // library's event dispatch.
  bool retryPending_ = false;
  bool pendingPersistToken_ = false;
  // Set by requestCronList(), flushed by poll(). Never sent from the WS
  // callback: sendTXT under the library's event dispatch re-enters the socket.
  bool cronReqPending_ = false;

  // Reconnect state, driven from poll(). reconnectPending_ is set by the WS
  // disconnect callback; the wait makes the retry rate quadratic instead of
  // tight-looping the radio while the gateway is down. A successful
  // (re)authentication resets reconnectAttempts_.
  bool reconnectPending_ = false;
  uint32_t reconnectAtMs_ = 0;
  uint8_t reconnectAttempts_ = 0;
  bool reconnecting_ = false;

  // Borrowed by reply — one buffer serves the outbound frame and the inbound
  // reply, exactly as in OpenClawActivity.
  char* chatBuf_ = nullptr;
  // Text of the current send, kept for the failure-marker retry. Bounded by
  // CHAT_MESSAGE_CAP, which sendMessage() already enforces.
  char lastMessage_[CHAT_MESSAGE_CAP + 1] = {};
  ChatReply reply_{};
  uint32_t nextMsgId_ = 1;
  bool awaitingReply_ = false;
  uint8_t chatAttempts_ = 0;
  bool paintedPartial_ = false;
  uint32_t sentAtMs_ = 0;
  SendOutcome outcome_ = SendOutcome::Failed;

  std::unique_ptr<WebSocketsClient> ws_;
  Identity identity_{};
  std::unique_ptr<Handshake> handshake_;

  void* notifyCtx_ = nullptr;
  void (*notifyFn_)(void*) = nullptr;
};

}  // namespace OpenClaw
