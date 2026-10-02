#pragma once

#include <cstddef>
#include <cstdint>

// OpenClaw v2 connect handshake.
//
// The wire protocol is a direct port of AIWatch_Ver2.0's
// components/openclaw/openclaw_client.c (send_connect / handle_message):
//
//   server -> {"type":"event","event":"connect.challenge",
//              "payload":{"nonce":"...","ts":<ms>}}
//   client -> {"type":"req","id":"<n>","method":"connect","params":{
//                "minProtocol":3,"maxProtocol":4,
//                "client":{"id":"cli",...},"role":"operator",
//                "scopes":["operator.read","operator.write","operator.admin"],
//                "auth":{"token":"<shared or device token>"},
//                "device":{"id":..,"publicKey":..,"signature":..,
//                          "signedAt":..,"nonce":..}}}
//   server -> {"type":"res","id":"<n>","payload":{...}}        authenticated
//          -> {"type":"res","error":{"code":..,"message":..}}  refused
//
// The authenticated payload carries a server snapshot — measured against a
// live gateway 2026-10-02, its keys are type/protocol/server/features/
// snapshot/pluginSurfaceUrls/auth/policy, and snapshot.uptimeMs is the ONLY
// place the gateway reports its uptime (the periodic `health` event has no
// uptime field at all, contrary to what older clients parse). That value is
// the one AIWatch_Ver2.0 renders as its web dashboard's 运行时间, so the
// workbench card reads the same number through here.
//
// The signature covers a pipe-joined string, not the JSON:
//
//   v2|<deviceId>|<clientId>|<clientMode>|<role>|<scopes>|
//      <signedAtMs>|<token>|<nonce>
//
// with clientId/clientMode/role/scopes fixed to the values sent in params, so
// those four fields must stay in sync between the JSON and the signed string —
// that coupling is why the builder owns both.
//
// Nothing here includes ESP-IDF or Arduino headers: the same translation unit
// compiles into the firmware and into the host test suite.
namespace OpenClaw {

// Mirrors openclaw_client.c:366-397. CLIENT_ID and CLIENT_MODE double as the
// "cli|cli" segment of the signed payload.
inline constexpr const char* CLIENT_ID = "cli";
inline constexpr const char* CLIENT_MODE = "cli";
inline constexpr const char* ROLE = "operator";
inline constexpr const char* CLIENT_VERSION = "0.5.0";
inline constexpr const char* PLATFORM = "esp32";
inline constexpr const char* LOCALE = "en-US";
// Mirrors AIWatch during the S2 spike so a server-side check on the user
// agent cannot be the reason authentication fails. Differentiate this once
// the connect round trip is proven, then re-run the probe.
inline constexpr const char* USER_AGENT = "AIWatch/0.5.0";
inline constexpr int MIN_PROTOCOL = 3;
inline constexpr int MAX_PROTOCOL = 4;

inline constexpr const char* SCOPES[] = {"operator.read", "operator.write", "operator.admin"};
inline constexpr size_t SCOPE_COUNT = sizeof(SCOPES) / sizeof(SCOPES[0]);

// Prefix of the signed payload (openclaw_client.c:415).
inline constexpr const char* AUTH_VERSION = "v2";

// msg ids are a decimal counter modulo 99999 (openclaw_client.c:36,332).
inline constexpr uint32_t MAX_MSG_ID = 99999;

// Any wall clock before this is an unsynced RTC (ESP32 boots in 1970).
inline constexpr int64_t MIN_SANE_EPOCH_MS = 1600000000000LL;

// Local wall-clock milliseconds. An ESP32 that has never run SNTP reports
// 1970, which is below MIN_SANE_EPOCH_MS — Handshake then falls back to the
// server's own challenge timestamp rather than signing a stale date.
int64_t nowEpochMs();

// Device identity: ed25519 seed -> keypair, and the device id the server sees.
// deviceId is SHA-256(publicKey) as lowercase hex (openclaw_client.c:1307-1310).
struct Identity {
  char deviceId[65] = {};
  uint8_t publicKey[32] = {};
  uint8_t privateKey[64] = {};
  bool valid = false;
};

enum class Phase : uint8_t {
  Idle,           // not fed a frame yet
  WaitChallenge,  // connected, waiting for connect.challenge
  ConnectSent,    // our connect request is on the wire
  Authenticated,  // res arrived with no error while authenticating
  Failed,         // res carried an error, or the frame was unusable
};

// base64url without padding — the alphabet and the missing '=' are what the
// server expects for publicKey/signature (openclaw_client.c:54-71).
size_t base64urlEncode(char* out, size_t cap, const uint8_t* in, size_t len);

// Decode 64 hex chars (either case) into 32 bytes. Returns false on a
// non-hex character or a null input.
bool decodeHexSeed(const char* seedHex64, uint8_t out[32]);

// Lowercase hex of exactly 32 bytes into a NUL-terminated 65-char buffer.
void hexEncode32(char out[65], const uint8_t in[32]);

// The pipe-joined string that gets signed. Returns 0 if it did not fit.
size_t buildAuthPayload(char* out, size_t cap, const char* deviceId, int64_t signedAtMs, const char* token,
                        const char* nonce);

class Handshake {
 public:
  // The connect frame lives inside the object rather than on the caller's
  // stack: worst case is ~1 KB with a 160-char token fully escaped, and both
  // callers run on the 8 KB ESP32-C3 loop task with a WebSocket client already
  // allocated.
  static constexpr size_t FRAME_CAP = 1152;
  static constexpr size_t PAYLOAD_CAP = 512;

  void configure(const Identity& id, const char* token, const char* deviceToken);

  // Feed one server text frame. nowMs is the local epoch-millisecond clock; the
  // challenge's own "ts" is used instead when the local clock is still 1970
  // (a plaintext ws:// session may never have run SNTP).
  // Returns true when pendingFrame() holds a reply the caller must send.
  bool onServerFrame(const char* json, size_t len, int64_t nowMs);

  const char* pendingFrame() const { return out_; }
  size_t pendingFrameLen() const { return outLen_; }
  void clearPendingFrame() {
    outLen_ = 0;
    out_[0] = '\0';
  }

  Phase phase() const { return phase_; }
  const char* errorCode() const { return errorCode_; }
  const char* errorMessage() const { return errorMessage_; }
  const char* requestId() const { return requestId_; }
  // Device token issued alongside an authenticated response, when the server
  // hands one out; used as the fallback credential on later reconnects.
  const char* issuedDeviceToken() const { return issued_[0] != '\0' ? issued_ : nullptr; }
  uint32_t sentMsgId() const { return sentMsgId_; }
  // Gateway wall-clock ms from the challenge payload ("ts"), 0 until a
  // challenge has been parsed. Lets the session seed an unset system clock
  // without an extra round trip (plaintext ws:// never runs SNTP).
  int64_t serverTs() const { return serverTs_; }
  // Gateway uptime (ms) from the authenticated response's
  // payload.snapshot.uptimeMs, 0 until such a frame has been parsed. This is
  // the same field AIWatch_Ver2.0's dashboard uses; the session copies it at
  // authentication because the handshake object does not outlive that moment.
  uint64_t snapshotUptimeMs() const { return snapshotUptimeMs_; }

 private:
  struct Out {
    char* buf;
    size_t cap;
    size_t len = 0;
    bool overflow = false;
    void raw(const char* s, size_t n);
    void text(const char* s);
    // Copies s as a complete JSON string literal, quotes included.
    void jstr(const char* s);
  };

  static void onObjectStartCb(void* ctx);
  static void onObjectEndCb(void* ctx);
  static void onKeyCb(void* ctx, const char* key, size_t len);
  static void onStringCb(void* ctx, const char* value, size_t len);
  static void onNumberCb(void* ctx, const char* value, size_t len);

  // Feeds one frame through the streaming parser. Returns false when the
  // frame was not valid JSON (caller should ignore it rather than fail).
  bool parseFrame(const char* json, size_t len);
  void resetFrameFields();
  void handleObjectStart();
  void handleObjectEnd();
  void handleKey(const char* key, size_t len);
  void handleString(const char* value, size_t len);
  void handleNumber(const char* value, size_t len);

  bool buildConnectFrame(int64_t signedAtMs);
  void failPhase(const char* code, const char* message);
  const char* activeToken() const { return token_[0] != '\0' ? token_ : deviceToken_; }

  // Parser state. The frame shapes we care about nest at most three deep
  // (root -> payload/error -> auth/details/snapshot), so a fixed stack beats a
  // tree.
  enum class Obj : uint8_t { Root, Payload, Error, Auth, Details, Snapshot, Other };
  static constexpr uint8_t OBJ_DEPTH = 8;
  Obj objStack_[OBJ_DEPTH] = {};
  uint8_t objDepth_ = 0;
  char lastKey_[32] = {};
  char type_[16] = {};
  char event_[32] = {};
  char nonce_[128] = {};
  int64_t serverTs_ = 0;
  uint64_t snapshotUptimeMs_ = 0;
  char errorCode_[48] = {};
  char errorMessage_[160] = {};
  char requestId_[48] = {};
  char issued_[161] = {};
  bool resHadError_ = false;
  // Set when a value we dispatch on did not fit its buffer; such a frame is
  // unusable and must not be acted on (a truncated nonce signs wrong).
  bool frameTruncated_ = false;

  Identity id_{};
  char token_[161] = {};
  char deviceToken_[161] = {};
  uint32_t msgId_ = 0;
  uint32_t sentMsgId_ = 0;
  Phase phase_ = Phase::Idle;
  char out_[FRAME_CAP] = {};
  size_t outLen_ = 0;
  // Scratch for the signed string. Kept as a member rather than a local
  // because buildConnectFrame() runs directly under an ed25519_sign() that
  // already wants ~1.3 KB of stack, and both callers sit on the 8 KB loop task.
  char payload_[PAYLOAD_CAP] = {};
};

}  // namespace OpenClaw
