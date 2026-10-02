#include "OpenClawHandshake.h"

#include <StreamingJsonParser.h>
#include <ed25519.h>
#include <sys/time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace OpenClaw {
namespace {

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// payload is not NUL-terminated at every call site; returns true when the
// value was truncated (i.e. the copy is unusable).
bool copyTrunc(char* dst, size_t cap, const char* src, size_t len) {
  if (cap == 0) return true;
  const bool truncated = len >= cap;
  if (truncated) len = cap - 1;
  if (len > 0 && src != nullptr) memcpy(dst, src, len);
  dst[len] = '\0';
  return truncated;
}

bool keyEq(const char* got, const char* want) { return strcmp(got, want) == 0; }

}  // namespace

int64_t nowEpochMs() {
  timeval tv = {};
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + static_cast<int64_t>(tv.tv_usec) / 1000;
}

void Handshake::Out::raw(const char* s, size_t n) {
  if (overflow) return;
  if (len + n + 1 > cap) {
    overflow = true;
    return;
  }
  memcpy(buf + len, s, n);
  len += n;
  buf[len] = '\0';
}

void Handshake::Out::text(const char* s) { raw(s, strlen(s)); }

void Handshake::Out::jstr(const char* s) {
  raw("\"", 1);
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p != '\0'; ++p) {
    char esc[8];
    switch (*p) {
      case '"':
        text("\\\"");
        break;
      case '\\':
        text("\\\\");
        break;
      case '\b':
        text("\\b");
        break;
      case '\f':
        text("\\f");
        break;
      case '\n':
        text("\\n");
        break;
      case '\r':
        text("\\r");
        break;
      case '\t':
        text("\\t");
        break;
      default:
        if (*p < 0x20) {
          snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(*p));
          text(esc);
        } else {
          raw(reinterpret_cast<const char*>(p), 1);
        }
        break;
    }
  }
  raw("\"", 1);
}

bool decodeHexSeed(const char* seedHex64, uint8_t out[32]) {
  if (seedHex64 == nullptr || out == nullptr) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int hi = hexNibble(seedHex64[i * 2]);
    const int lo = hexNibble(seedHex64[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

void hexEncode32(char out[65], const uint8_t in[32]) {
  static constexpr char HEX[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    out[i * 2] = HEX[(in[i] >> 4) & 0x0F];
    out[i * 2 + 1] = HEX[in[i] & 0x0F];
  }
  out[64] = '\0';
}

size_t base64urlEncode(char* out, size_t cap, const uint8_t* in, size_t len) {
  static constexpr char TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  if (out == nullptr || cap == 0) return 0;
  out[0] = '\0';
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = static_cast<uint32_t>(in[i]) << 16;
    if (i + 1 < len) v |= static_cast<uint32_t>(in[i + 1]) << 8;
    if (i + 2 < len) v |= static_cast<uint32_t>(in[i + 2]);
    if (o + 4 >= cap) {
      out[0] = '\0';
      return 0;
    }
    out[o++] = TABLE[(v >> 18) & 0x3F];
    out[o++] = TABLE[(v >> 12) & 0x3F];
    if (i + 1 < len) out[o++] = TABLE[(v >> 6) & 0x3F];
    if (i + 2 < len) out[o++] = TABLE[v & 0x3F];
  }
  out[o] = '\0';
  return o;
}

size_t buildAuthPayload(char* out, size_t cap, const char* deviceId, int64_t signedAtMs, const char* token,
                        const char* nonce) {
  if (out == nullptr || cap == 0) return 0;

  // Same comma-joined scope list that goes into params.scopes — the server
  // recomputes the string from the scopes it granted, so the two must match
  // byte for byte (openclaw_client.c:409-416).
  char scopes[128] = {};
  size_t scopesLen = 0;
  for (size_t i = 0; i < SCOPE_COUNT; ++i) {
    const size_t n = strlen(SCOPES[i]);
    if (i > 0) {
      if (scopesLen + 1 >= sizeof(scopes)) return 0;
      scopes[scopesLen++] = ',';
    }
    if (scopesLen + n >= sizeof(scopes)) return 0;
    memcpy(scopes + scopesLen, SCOPES[i], n);
    scopesLen += n;
  }
  scopes[scopesLen] = '\0';

  const int n = snprintf(out, cap, "%s|%s|%s|%s|%s|%s|%lld|%s|%s", AUTH_VERSION, deviceId, CLIENT_ID, CLIENT_MODE, ROLE,
                         scopes, static_cast<long long>(signedAtMs), token, nonce);
  if (n < 0 || static_cast<size_t>(n) >= cap) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n);
}

void Handshake::configure(const Identity& id, const char* token, const char* deviceToken) {
  id_ = id;
  snprintf(token_, sizeof(token_), "%s", token != nullptr ? token : "");
  snprintf(deviceToken_, sizeof(deviceToken_), "%s", deviceToken != nullptr ? deviceToken : "");
  msgId_ = 0;
  sentMsgId_ = 0;
  clearPendingFrame();
  resetFrameFields();
  if (!id_.valid) {
    phase_ = Phase::Failed;
    failPhase("NO_DEVICE_KEY", "device key missing or not 64 hex characters");
    return;
  }
  phase_ = Phase::WaitChallenge;
}

void Handshake::resetFrameFields() {
  objDepth_ = 0;
  objStack_[0] = Obj::Root;
  lastKey_[0] = '\0';
  type_[0] = '\0';
  event_[0] = '\0';
  nonce_[0] = '\0';
  serverTs_ = 0;
  snapshotUptimeMs_ = 0;
  errorCode_[0] = '\0';
  errorMessage_[0] = '\0';
  requestId_[0] = '\0';
  issued_[0] = '\0';
  resHadError_ = false;
  frameTruncated_ = false;
}

void Handshake::failPhase(const char* code, const char* message) {
  phase_ = Phase::Failed;
  snprintf(errorCode_, sizeof(errorCode_), "%s", code != nullptr ? code : "");
  snprintf(errorMessage_, sizeof(errorMessage_), "%s", message != nullptr ? message : "");
}

// A separate stack frame so the parser's 512-byte token buffer is released
// before buildConnectFrame() runs ed25519_sign (~1.3 KB of its own stack).
bool Handshake::parseFrame(const char* json, size_t len) {
  resetFrameFields();

  JsonCallbacks cb = {};
  cb.ctx = this;
  cb.onKey = &Handshake::onKeyCb;
  cb.onString = &Handshake::onStringCb;
  cb.onNumber = &Handshake::onNumberCb;
  cb.onObjectStart = &Handshake::onObjectStartCb;
  cb.onObjectEnd = &Handshake::onObjectEndCb;

  StreamingJsonParser parser(cb);
  parser.feed(json, len);
  // The parser reports syntax errors but not truncation: a frame cut short
  // mid-object leaves no error flag. Requiring the nesting depth to have
  // returned to zero is what tells a complete document from a partial one.
  return !parser.hasError() && objDepth_ == 0;
}

bool Handshake::onServerFrame(const char* json, size_t len, int64_t nowMs) {
  clearPendingFrame();
  if (phase_ == Phase::Authenticated || phase_ == Phase::Failed || !id_.valid) return false;
  if (json == nullptr || len == 0) return false;

  if (!parseFrame(json, len)) return false;
  if (frameTruncated_) return false;

  if (keyEq(type_, "event") && keyEq(event_, "connect.challenge")) {
    if (phase_ != Phase::WaitChallenge && phase_ != Phase::ConnectSent) return false;
    if (nonce_[0] == '\0') {
      failPhase("NO_NONCE", "connect.challenge carried no nonce");
      return false;
    }
    if (activeToken()[0] == '\0') {
      failPhase("NO_TOKEN", "no shared token and no cached device token");
      return false;
    }
    // Prefer the local clock, fall back to the server's own timestamp: a
    // plaintext ws:// session may never have run SNTP, and signing 1970 would
    // be rejected as stale while the server's ts is by construction fresh.
    const int64_t signedAtMs = nowMs >= MIN_SANE_EPOCH_MS ? nowMs : serverTs_;
    if (!buildConnectFrame(signedAtMs)) {
      failPhase("FRAME_TOO_LONG", "connect frame did not fit");
      return false;
    }
    phase_ = Phase::ConnectSent;
    return true;
  }

  if (keyEq(type_, "res") && phase_ == Phase::ConnectSent) {
    if (resHadError_) {
      // Fields already hold what the server said; just move to Failed.
      phase_ = Phase::Failed;
      if (errorCode_[0] == '\0') snprintf(errorCode_, sizeof(errorCode_), "%s", "UNKNOWN");
      if (errorMessage_[0] == '\0') snprintf(errorMessage_, sizeof(errorMessage_), "%s", "server returned an error");
      return false;
    }
    phase_ = Phase::Authenticated;
    return false;
  }

  return false;
}

bool Handshake::buildConnectFrame(int64_t signedAtMs) {
  const size_t payloadLen =
      buildAuthPayload(payload_, sizeof(payload_), id_.deviceId, signedAtMs, activeToken(), nonce_);
  if (payloadLen == 0) return false;

  uint8_t signature[64];
  ed25519_sign(signature, reinterpret_cast<const uint8_t*>(payload_), payloadLen, id_.publicKey, id_.privateKey);

  char pubB64[48];
  char sigB64[96];
  if (base64urlEncode(pubB64, sizeof(pubB64), id_.publicKey, sizeof(id_.publicKey)) == 0) return false;
  if (base64urlEncode(sigB64, sizeof(sigB64), signature, sizeof(signature)) == 0) return false;

  msgId_ = (msgId_ + 1) % MAX_MSG_ID;
  sentMsgId_ = msgId_;
  char idStr[12];
  snprintf(idStr, sizeof(idStr), "%u", sentMsgId_);

  char signedAtStr[24];
  snprintf(signedAtStr, sizeof(signedAtStr), "%lld", static_cast<long long>(signedAtMs));

  Out o{out_, sizeof(out_)};
  o.text("{\"type\":\"req\",\"id\":");
  o.jstr(idStr);
  o.text(",\"method\":\"connect\",\"params\":{\"minProtocol\":3,\"maxProtocol\":4,\"client\":{\"id\":");
  o.jstr(CLIENT_ID);
  o.text(",\"version\":");
  o.jstr(CLIENT_VERSION);
  o.text(",\"platform\":");
  o.jstr(PLATFORM);
  o.text(",\"mode\":");
  o.jstr(CLIENT_MODE);
  o.text("},\"role\":");
  o.jstr(ROLE);
  o.text(",\"scopes\":[");
  for (size_t i = 0; i < SCOPE_COUNT; ++i) {
    if (i > 0) o.text(",");
    o.jstr(SCOPES[i]);
  }
  o.text("],\"caps\":[\"tool-events\",\"proactive\"],\"locale\":");
  o.jstr(LOCALE);
  o.text(",\"userAgent\":");
  o.jstr(USER_AGENT);
  o.text(",\"auth\":{\"token\":");
  o.jstr(activeToken());
  o.text("},\"device\":{\"id\":");
  o.jstr(id_.deviceId);
  o.text(",\"publicKey\":");
  o.jstr(pubB64);
  o.text(",\"signature\":");
  o.jstr(sigB64);
  o.text(",\"signedAt\":");
  o.text(signedAtStr);
  o.text(",\"nonce\":");
  o.jstr(nonce_);
  o.text("}}}");

  if (o.overflow) {
    out_[0] = '\0';
    outLen_ = 0;
    return false;
  }
  outLen_ = o.len;
  return true;
}

void Handshake::handleObjectStart() {
  Obj role = Obj::Other;
  if (objDepth_ == 0) {
    role = Obj::Root;
  } else {
    const Obj parent = objStack_[objDepth_ - 1];
    if (parent == Obj::Root && keyEq(lastKey_, "payload")) {
      role = Obj::Payload;
    } else if (parent == Obj::Root && keyEq(lastKey_, "error")) {
      role = Obj::Error;
    } else if (parent == Obj::Error && keyEq(lastKey_, "details")) {
      role = Obj::Details;
    } else if (parent == Obj::Payload && keyEq(lastKey_, "auth")) {
      role = Obj::Auth;
    } else if (parent == Obj::Payload && keyEq(lastKey_, "snapshot")) {
      role = Obj::Snapshot;  // carries the gateway's uptimeMs (see header)
    }
  }
  if (objDepth_ < OBJ_DEPTH) objStack_[objDepth_++] = role;
  if (role == Obj::Error) resHadError_ = true;
  lastKey_[0] = '\0';
}

void Handshake::handleObjectEnd() {
  if (objDepth_ > 0) --objDepth_;
  lastKey_[0] = '\0';
}

void Handshake::handleKey(const char* key, size_t len) {
  // An oversized key is unknown, not a prefix of a known one — drop it rather
  // than let "type<...>" truncate into a false "type" match.
  if (copyTrunc(lastKey_, sizeof(lastKey_), key, len)) lastKey_[0] = '\0';
}

void Handshake::handleString(const char* value, size_t len) {
  const Obj top = objDepth_ > 0 ? objStack_[objDepth_ - 1] : Obj::Root;
  // Only fields the state machine dispatches on invalidate a frame when they
  // overflow; an over-long error message is still worth reporting.
  if (top == Obj::Root) {
    if (keyEq(lastKey_, "type")) {
      if (copyTrunc(type_, sizeof(type_), value, len)) frameTruncated_ = true;
    } else if (keyEq(lastKey_, "event")) {
      if (copyTrunc(event_, sizeof(event_), value, len)) frameTruncated_ = true;
    } else if (keyEq(lastKey_, "error")) {
      resHadError_ = true;
      copyTrunc(errorMessage_, sizeof(errorMessage_), value, len);
    }
  } else if (top == Obj::Payload) {
    if (keyEq(lastKey_, "nonce")) {
      if (copyTrunc(nonce_, sizeof(nonce_), value, len)) frameTruncated_ = true;
    }
  } else if (top == Obj::Error) {
    resHadError_ = true;
    if (keyEq(lastKey_, "code")) {
      if (copyTrunc(errorCode_, sizeof(errorCode_), value, len)) frameTruncated_ = true;
    } else if (keyEq(lastKey_, "message")) {
      copyTrunc(errorMessage_, sizeof(errorMessage_), value, len);
    }
  } else if (top == Obj::Details) {
    if (keyEq(lastKey_, "requestId")) copyTrunc(requestId_, sizeof(requestId_), value, len);
  } else if (top == Obj::Auth) {
    if (keyEq(lastKey_, "deviceToken")) copyTrunc(issued_, sizeof(issued_), value, len);
  }
}

void Handshake::handleNumber(const char* value, size_t len) {
  if (objDepth_ == 0) return;
  const Obj top = objStack_[objDepth_ - 1];
  if (top == Obj::Payload && keyEq(lastKey_, "ts")) {
    char buf[32];
    if (copyTrunc(buf, sizeof(buf), value, len)) return;
    serverTs_ = strtoll(buf, nullptr, 10);
  } else if (top == Obj::Snapshot && keyEq(lastKey_, "uptimeMs")) {
    char buf[32];
    if (copyTrunc(buf, sizeof(buf), value, len)) return;
    const int64_t ms = strtoll(buf, nullptr, 10);
    // Sanity bound of 10 years: a parsed-but-nonsensical value must not end
    // up on the card as a service that has been up for millennia.
    if (ms > 0 && ms <= 315360000000LL) snapshotUptimeMs_ = static_cast<uint64_t>(ms);
  }
}

void Handshake::onObjectStartCb(void* ctx) { static_cast<Handshake*>(ctx)->handleObjectStart(); }
void Handshake::onObjectEndCb(void* ctx) { static_cast<Handshake*>(ctx)->handleObjectEnd(); }
void Handshake::onKeyCb(void* ctx, const char* key, size_t len) { static_cast<Handshake*>(ctx)->handleKey(key, len); }
void Handshake::onStringCb(void* ctx, const char* value, size_t len) {
  static_cast<Handshake*>(ctx)->handleString(value, len);
}
void Handshake::onNumberCb(void* ctx, const char* value, size_t len) {
  static_cast<Handshake*>(ctx)->handleNumber(value, len);
}

}  // namespace OpenClaw
