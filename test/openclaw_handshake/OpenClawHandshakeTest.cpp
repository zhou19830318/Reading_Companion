#include <StreamingJsonParser.h>
#include <ed25519.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "OpenClawHandshake.h"

using OpenClaw::AUTH_VERSION;
using OpenClaw::base64urlEncode;
using OpenClaw::buildAuthPayload;
using OpenClaw::decodeHexSeed;
using OpenClaw::hexEncode32;
using OpenClaw::Identity;
using OpenClaw::MAX_MSG_ID;
using OpenClaw::Phase;

namespace {

// --- base64url decode (test-only; the firmware only ever encodes) ---
int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

std::vector<uint8_t> b64urlDecode(const std::string& s) {
  std::vector<uint8_t> out;
  int val = 0;
  int bits = -8;
  for (char c : s) {
    if (c == '=') continue;
    const int d = b64Val(c);
    if (d < 0) return {};
    val = (val << 6) + d;
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

// Flattens a JSON document into dotted paths ("params.client.id" -> "cli").
// An array records only its first element under the array's own key, which is
// all these assertions need; the remaining elements are checked on raw text.
class FlatJson : public JsonCallbacks {
 public:
  FlatJson() {
    ctx = this;
    onKey = &FlatJson::keyCb;
    onString = &FlatJson::strCb;
    onNumber = &FlatJson::numCb;
    onObjectStart = &FlatJson::objStartCb;
    onObjectEnd = &FlatJson::objEndCb;
    onArrayStart = &FlatJson::arrStartCb;
    onArrayEnd = &FlatJson::arrEndCb;
    onBool = nullptr;
    onNull = nullptr;
  }

  bool parse(const std::string& json) {
    StreamingJsonParser parser(*this);
    parser.feed(json.data(), json.size());
    return !parser.hasError();
  }

  std::string get(const std::string& path) const {
    const auto it = kv.find(path);
    return it == kv.end() ? std::string() : it->second;
  }
  bool has(const std::string& path) const { return kv.count(path) != 0; }

 private:
  static void keyCb(void* c, const char* k, size_t n) { static_cast<FlatJson*>(c)->pending.assign(k, n); }
  // An object consumes its key (the key names the container, not the value),
  // while an array keeps its key alive so the elements record under it.
  static void objStartCb(void* c) {
    auto* s = static_cast<FlatJson*>(c);
    s->stack.push_back(s->pending);
    s->pending.clear();
  }
  static void objEndCb(void* c) {
    auto* s = static_cast<FlatJson*>(c);
    if (!s->stack.empty()) s->stack.pop_back();
    s->pending.clear();
  }
  static void arrStartCb(void*) {}
  static void arrEndCb(void* c) { static_cast<FlatJson*>(c)->pending.clear(); }
  static void strCb(void* c, const char* v, size_t n) { static_cast<FlatJson*>(c)->put(v, n); }
  static void numCb(void* c, const char* v, size_t n) { static_cast<FlatJson*>(c)->put(v, n); }

  void put(const char* v, size_t n) {
    if (pending.empty()) return;
    std::string path;
    for (const auto& k : stack) {
      if (k.empty()) continue;
      if (!path.empty()) path += '.';
      path += k;
    }
    if (!path.empty()) path += '.';
    path += pending;
    kv[path].assign(v, n);
    pending.clear();
  }

  std::vector<std::string> stack;
  std::string pending;
  std::map<std::string, std::string> kv;
};

// A deterministic keypair plus a plausible 64-hex device id, so every test
// signs with the same material.
Identity makeIdentity() {
  Identity id{};
  uint8_t seed[32];
  for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(i);
  ed25519_create_keypair(id.publicKey, id.privateKey, seed);

  uint8_t did[32];
  for (int i = 0; i < 32; ++i) did[i] = static_cast<uint8_t>(0xA0 + i);
  hexEncode32(id.deviceId, did);
  id.valid = true;
  return id;
}

std::string challenge(const std::string& nonce, long long ts) {
  std::string s = "{\"type\":\"event\",\"event\":\"connect.challenge\",\"payload\":{\"nonce\":\"";
  s += nonce;
  s += "\",\"ts\":";
  s += std::to_string(ts);
  s += "}}";
  return s;
}

// challenge() yields a temporary; bind it first so data() and size() come
// from one object that outlives the call.
bool feed(OpenClaw::Handshake& hs, const std::string& frame, int64_t nowMs) {
  return hs.onServerFrame(frame.data(), frame.size(), nowMs);
}

bool feedLit(OpenClaw::Handshake& hs, const char* frame, int64_t nowMs) {
  return hs.onServerFrame(frame, strlen(frame), nowMs);
}

constexpr const char* kNonce = "f0e1d2c3-b4a5-4678-9abc-def012345678";
constexpr const char* kToken = "shared-token-value";
constexpr int64_t kNowMs = 1700000000123LL;

}  // namespace

TEST(Base64Url, MatchesRfc4648WithoutPadding) {
  char buf[128];

  auto enc = [&](const void* in, size_t n) {
    buf[0] = '\0';
    base64urlEncode(buf, sizeof(buf), static_cast<const uint8_t*>(in), n);
    return std::string(buf);
  };

  EXPECT_EQ(enc("", 0), "");
  EXPECT_EQ(enc("f", 1), "Zg");
  EXPECT_EQ(enc("fo", 2), "Zm8");
  EXPECT_EQ(enc("foo", 3), "Zm9v");
  EXPECT_EQ(enc("foobar", 6), "Zm9vYmFy");

  // 32 zero bytes: 43 characters, no '=' padding.
  const uint8_t zeros[32] = {};
  const std::string z = enc(zeros, 32);
  EXPECT_EQ(z.size(), 43u);
  EXPECT_EQ(z, std::string(43, 'A'));

  // 64 zero bytes -> 86 characters.
  const uint8_t zeros64[64] = {};
  EXPECT_EQ(enc(zeros64, 64).size(), 86u);

  // '-' and '_' must be used instead of '+' and '/'.
  const uint8_t ff[3] = {0xFF, 0xFF, 0xFF};
  EXPECT_EQ(enc(ff, 3), "____");
  const uint8_t dash[3] = {0xFB, 0xF0, 0x00};
  EXPECT_EQ(enc(dash, 3), "-_AA");

  // Too small a buffer must fail rather than truncate.
  char tiny[4] = {};
  EXPECT_EQ(base64urlEncode(tiny, 4, zeros, 32), 0u);
}

TEST(HexCodec, RoundTripsAndRejectsMalformedInput) {
  const char* hex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
  uint8_t bytes[32];
  ASSERT_TRUE(decodeHexSeed(hex, bytes));
  for (int i = 0; i < 32; ++i) ASSERT_EQ(bytes[i], static_cast<uint8_t>(i)) << i;

  char back[65];
  hexEncode32(back, bytes);
  EXPECT_STREQ(back, hex);

  // Uppercase input decodes to the same bytes.
  uint8_t upper[32];
  EXPECT_TRUE(decodeHexSeed("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", upper));
  EXPECT_EQ(memcmp(bytes, upper, 32), 0);

  EXPECT_FALSE(decodeHexSeed("", bytes));
  EXPECT_FALSE(decodeHexSeed("00", bytes));
  EXPECT_FALSE(decodeHexSeed("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1g", bytes));
  EXPECT_FALSE(decodeHexSeed(nullptr, bytes));
}

// The signed payload is the one thing that must not drift from the reference
// implementation: the gateway recomputes this exact string.
TEST(AuthPayload, MatchesAiWatchReferenceFormat) {
  const Identity id = makeIdentity();
  char payload[512];
  const size_t n = buildAuthPayload(payload, sizeof(payload), id.deviceId, kNowMs, kToken, kNonce);
  ASSERT_GT(n, 0u);

  const std::string expected = std::string(AUTH_VERSION) + "|" + id.deviceId + "|cli|cli|operator|" +
                               "operator.read,operator.write,operator.admin|" + std::to_string(kNowMs) + "|" + kToken +
                               "|" + kNonce;
  EXPECT_EQ(std::string(payload), expected);
  EXPECT_EQ(n, expected.size());
}

TEST(AuthPayload, RejectsAnOutgrownBuffer) {
  const Identity id = makeIdentity();
  char small[32];
  EXPECT_EQ(buildAuthPayload(small, sizeof(small), id.deviceId, kNowMs, kToken, kNonce), 0u);
}

TEST(Configure, MissingDeviceKeyFailsImmediately) {
  OpenClaw::Handshake hs;
  Identity bad{};
  hs.configure(bad, kToken, "");
  EXPECT_EQ(hs.phase(), Phase::Failed);
  EXPECT_STREQ(hs.errorCode(), "NO_DEVICE_KEY");
  const std::string in = challenge(kNonce, kNowMs);
  EXPECT_FALSE(feed(hs, in, kNowMs));
}

// End to end: challenge in, signed connect req out, and the signature on the
// wire verifies against the payload reconstructed from that same frame.
TEST(Handshake, ChallengeProducesASignedConnectRequest) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");
  ASSERT_EQ(hs.phase(), Phase::WaitChallenge);

  const std::string in = challenge(kNonce, kNowMs);
  ASSERT_TRUE(feed(hs, in, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::ConnectSent);
  ASSERT_GT(hs.pendingFrameLen(), 0u);
  ASSERT_LE(hs.pendingFrameLen(), OpenClaw::Handshake::FRAME_CAP);

  const std::string frame(hs.pendingFrame(), hs.pendingFrameLen());
  FlatJson f;
  ASSERT_TRUE(f.parse(frame)) << frame;

  EXPECT_EQ(f.get("type"), "req");
  EXPECT_EQ(f.get("id"), "1");
  EXPECT_EQ(f.get("method"), "connect");
  EXPECT_EQ(f.get("params.minProtocol"), "3");
  EXPECT_EQ(f.get("params.maxProtocol"), "4");
  EXPECT_EQ(f.get("params.client.id"), "cli");
  EXPECT_EQ(f.get("params.client.version"), "0.5.0");
  EXPECT_EQ(f.get("params.client.platform"), "esp32");
  EXPECT_EQ(f.get("params.client.mode"), "cli");
  EXPECT_EQ(f.get("params.role"), "operator");
  EXPECT_EQ(f.get("params.locale"), "en-US");
  EXPECT_EQ(f.get("params.scopes"), "operator.read");
  EXPECT_EQ(f.get("params.auth.token"), kToken);
  EXPECT_EQ(f.get("params.device.id"), makeIdentity().deviceId);
  EXPECT_EQ(f.get("params.device.nonce"), kNonce);
  EXPECT_EQ(f.get("params.device.signedAt"), std::to_string(kNowMs));
  EXPECT_NE(frame.find("\"operator.write\""), std::string::npos);
  EXPECT_NE(frame.find("\"operator.admin\""), std::string::npos);
  EXPECT_NE(frame.find("\"tool-events\""), std::string::npos);
  EXPECT_NE(frame.find("\"proactive\""), std::string::npos);

  const std::string pubB64 = f.get("params.device.publicKey");
  const std::string sigB64 = f.get("params.device.signature");
  ASSERT_EQ(pubB64.size(), 43u);
  ASSERT_EQ(sigB64.size(), 86u);

  const std::vector<uint8_t> pub = b64urlDecode(pubB64);
  const std::vector<uint8_t> sig = b64urlDecode(sigB64);
  ASSERT_EQ(pub.size(), 32u);
  ASSERT_EQ(sig.size(), 64u);

  const Identity id = makeIdentity();
  EXPECT_EQ(memcmp(pub.data(), id.publicKey, 32), 0);

  // Rebuild the payload from the frame's own fields: if signing and framing
  // ever disagree, this is where it shows up.
  char payload[512];
  const size_t plen = buildAuthPayload(payload, sizeof(payload), f.get("params.device.id").c_str(), kNowMs,
                                       f.get("params.auth.token").c_str(), f.get("params.device.nonce").c_str());
  ASSERT_GT(plen, 0u);
  EXPECT_EQ(ed25519_verify(sig.data(), reinterpret_cast<const uint8_t*>(payload), plen, pub.data()), 1);

  // Tampering with the payload must break the signature.
  payload[0] = 'X';
  EXPECT_EQ(ed25519_verify(sig.data(), reinterpret_cast<const uint8_t*>(payload), plen, pub.data()), 0);
}

TEST(Handshake, MessageIdsAdvanceAndWrap) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");

  const std::string first = challenge(kNonce, kNowMs);
  ASSERT_TRUE(feed(hs, first, kNowMs));
  EXPECT_EQ(hs.sentMsgId(), 1u);
  EXPECT_NE(std::string(hs.pendingFrame()).find("\"id\":\"1\""), std::string::npos);

  const std::string second = challenge(kNonce, kNowMs);
  ASSERT_TRUE(feed(hs, second, kNowMs));
  EXPECT_EQ(hs.sentMsgId(), 2u);
  EXPECT_NE(std::string(hs.pendingFrame()).find("\"id\":\"2\""), std::string::npos);
}

TEST(Handshake, FallsBackToTheChallengeTimestampWhenTheClockIsUnset) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");
  const std::string in = challenge(kNonce, 1700000999555LL);
  ASSERT_TRUE(hs.onServerFrame(in.data(), in.size(), /*nowMs=*/0));

  FlatJson f;
  ASSERT_TRUE(f.parse(hs.pendingFrame()));
  EXPECT_EQ(f.get("params.device.signedAt"), "1700000999555");
}

TEST(Handshake, PrefersTheSharedTokenAndFallsBackToTheDeviceToken) {
  const std::string in = challenge(kNonce, kNowMs);
  {
    OpenClaw::Handshake hs;
    hs.configure(makeIdentity(), "shared-abc", "device-xyz");
    ASSERT_TRUE(feed(hs, in, kNowMs));
    FlatJson f;
    ASSERT_TRUE(f.parse(hs.pendingFrame()));
    EXPECT_EQ(f.get("params.auth.token"), "shared-abc");
  }
  {
    OpenClaw::Handshake hs;
    hs.configure(makeIdentity(), "", "device-xyz");
    ASSERT_TRUE(feed(hs, in, kNowMs));
    FlatJson f;
    ASSERT_TRUE(f.parse(hs.pendingFrame()));
    EXPECT_EQ(f.get("params.auth.token"), "device-xyz");
  }
}

TEST(Handshake, RefusesToSignWithoutAnyToken) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), "", "");
  const std::string in = challenge(kNonce, kNowMs);
  EXPECT_FALSE(feed(hs, in, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::Failed);
  EXPECT_STREQ(hs.errorCode(), "NO_TOKEN");
}

TEST(Handshake, RefusesAChallengeWithoutANonce) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");
  const char* in = "{\"type\":\"event\",\"event\":\"connect.challenge\",\"payload\":{\"ts\":1700000000000}}";
  EXPECT_FALSE(feedLit(hs, in, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::Failed);
  EXPECT_STREQ(hs.errorCode(), "NO_NONCE");
}

TEST(Handshake, EscapesJsonSpecialCharactersInTheTokenAndNonce) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), "a\"b\\c", "");
  // challenge() splices the nonce straight into the JSON, so the nonce here is
  // already JSON-escaped; it decodes back to n"q\z.
  const std::string in = challenge("n\\\"q\\\\z", kNowMs);
  ASSERT_TRUE(feed(hs, in, kNowMs));

  const std::string frame(hs.pendingFrame(), hs.pendingFrameLen());
  EXPECT_NE(frame.find("a\\\"b\\\\c"), std::string::npos);

  FlatJson f;
  ASSERT_TRUE(f.parse(frame)) << frame;
  EXPECT_EQ(f.get("params.auth.token"), "a\"b\\c");
  EXPECT_EQ(f.get("params.device.nonce"), "n\"q\\z");

  char payload[512];
  const size_t plen = buildAuthPayload(payload, sizeof(payload), f.get("params.device.id").c_str(), kNowMs,
                                       f.get("params.auth.token").c_str(), f.get("params.device.nonce").c_str());
  ASSERT_GT(plen, 0u);
  const std::vector<uint8_t> sig = b64urlDecode(f.get("params.device.signature"));
  ASSERT_EQ(sig.size(), 64u);
  const std::vector<uint8_t> pub = b64urlDecode(f.get("params.device.publicKey"));
  ASSERT_EQ(pub.size(), 32u);
  EXPECT_EQ(ed25519_verify(sig.data(), reinterpret_cast<const uint8_t*>(payload), plen, pub.data()), 1);
}

TEST(Handshake, AnOverlongNonceIsDiscardedRatherThanSigned) {
  const std::string in = challenge(std::string(200, 'n'), kNowMs);
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");
  EXPECT_FALSE(feed(hs, in, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::WaitChallenge);
  EXPECT_EQ(hs.pendingFrameLen(), 0u);
}

TEST(Handshake, SuccessfulResponseAuthenticatesAndCapturesTheDeviceToken) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");
  const std::string in = challenge(kNonce, kNowMs);
  ASSERT_TRUE(feed(hs, in, kNowMs));
  hs.clearPendingFrame();

  const char* ok = R"({"type":"res","id":"1","payload":{"hello":"ok","auth":{"deviceToken":"issued-tok"}}})";
  EXPECT_FALSE(feedLit(hs, ok, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::Authenticated);
  EXPECT_STREQ(hs.issuedDeviceToken(), "issued-tok");
  EXPECT_EQ(hs.pendingFrameLen(), 0u);
}

// Shape captured from a live gateway 2026-10-02: the connect response carries
// a snapshot whose uptimeMs is the ONLY uptime the gateway reports anywhere
// (its pushed health event has none) — the field AIWatch_Ver2.0's dashboard
// renders as 运行时间, and what the workbench card now shows.
TEST(Handshake, CapturesGatewayUptimeFromTheConnectSnapshot) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");
  const std::string in = challenge(kNonce, kNowMs);
  ASSERT_TRUE(feed(hs, in, kNowMs));
  hs.clearPendingFrame();

  const char* ok =
      R"({"type":"res","id":"1","payload":{"type":"connect","protocol":4,"server":{"version":"1.2.3"},"features":[],"snapshot":{"presence":{"state":"online"},"health":{"ok":true},"stateVersion":7,"uptimeMs":493109719,"appliedConfigHash":"abc","sessionDefaults":{},"updateAvailable":false},"pluginSurfaceUrls":{},"auth":{"deviceToken":"issued-tok"},"policy":{}}})";
  EXPECT_FALSE(feedLit(hs, ok, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::Authenticated);
  EXPECT_EQ(hs.snapshotUptimeMs(), 493109719ull);
}

// A reply without a snapshot (older gateway, or a refusal carrying none) must
// leave the value at 0 so the caller can tell "unknown" from "just booted".
TEST(Handshake, UptimeStaysZeroWhenTheReplyHasNoSnapshot) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");
  const std::string in = challenge(kNonce, kNowMs);
  ASSERT_TRUE(feed(hs, in, kNowMs));
  hs.clearPendingFrame();

  const char* ok = R"({"type":"res","id":"1","payload":{"hello":"ok","auth":{"deviceToken":"issued-tok"}}})";
  EXPECT_FALSE(feedLit(hs, ok, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::Authenticated);
  EXPECT_EQ(hs.snapshotUptimeMs(), 0u);
}

// The sanity bound: a parsed number that cannot be a real uptime is dropped
// rather than rendered as a service measured in centuries.
TEST(Handshake, AnOutlandishUptimeIsRejected) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");
  const std::string in = challenge(kNonce, kNowMs);
  ASSERT_TRUE(feed(hs, in, kNowMs));
  hs.clearPendingFrame();

  const char* ok = R"({"type":"res","id":"1","payload":{"snapshot":{"uptimeMs":999999999999999}}})";
  EXPECT_FALSE(feedLit(hs, ok, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::Authenticated);
  EXPECT_EQ(hs.snapshotUptimeMs(), 0u);
}

TEST(Handshake, ErrorResponseReportsCodeMessageAndPendingPairingId) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");
  const std::string in = challenge(kNonce, kNowMs);
  ASSERT_TRUE(feed(hs, in, kNowMs));
  hs.clearPendingFrame();

  const char* err =
      R"({"type":"res","id":"1","error":{"code":"NOT_PAIRED","message":"device not paired","details":{"requestId":"req-9"}}})";
  EXPECT_FALSE(feedLit(hs, err, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::Failed);
  EXPECT_STREQ(hs.errorCode(), "NOT_PAIRED");
  EXPECT_STREQ(hs.errorMessage(), "device not paired");
  EXPECT_STREQ(hs.requestId(), "req-9");
}

TEST(Handshake, TokenMismatchIsReportedVerbatim) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");
  const std::string in = challenge(kNonce, kNowMs);
  ASSERT_TRUE(feed(hs, in, kNowMs));

  const char* err = R"({"type":"res","error":{"code":"AUTH_TOKEN_MISMATCH","message":"bad token"}})";
  EXPECT_FALSE(feedLit(hs, err, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::Failed);
  EXPECT_STREQ(hs.errorCode(), "AUTH_TOKEN_MISMATCH");
  EXPECT_STREQ(hs.errorMessage(), "bad token");
}

TEST(Handshake, IgnoresUnrelatedFramesAndStaysWaiting) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");

  const char* agent = R"({"type":"event","event":"agent.update","payload":{"agentId":"a1"}})";
  EXPECT_FALSE(feedLit(hs, agent, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::WaitChallenge);
  EXPECT_EQ(hs.pendingFrameLen(), 0u);

  // An unrelated response must not be mistaken for our own.
  const char* stray = R"({"type":"res","id":"42","payload":{"ok":true}})";
  EXPECT_FALSE(feedLit(hs, stray, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::WaitChallenge);
}

TEST(Handshake, MalformedJsonIsIgnored) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");

  // Truncated mid-object: the parser has no error flag for this, so the
  // nesting depth is what has to catch it.
  const char* junk = "{\"type\":\"event\",\"event\":\"connect.challenge\"";
  EXPECT_FALSE(feedLit(hs, junk, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::WaitChallenge);
  EXPECT_EQ(hs.pendingFrameLen(), 0u);

  // Valid JSON that is not an object at all.
  const char* scalar = "\"just a string\"";
  EXPECT_FALSE(feedLit(hs, scalar, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::WaitChallenge);

  EXPECT_FALSE(hs.onServerFrame(nullptr, 0, kNowMs));
}

TEST(Handshake, AlreadyAuthenticatedFramesAreDropped) {
  OpenClaw::Handshake hs;
  hs.configure(makeIdentity(), kToken, "");
  const std::string in = challenge(kNonce, kNowMs);
  ASSERT_TRUE(feed(hs, in, kNowMs));
  const char* ok = R"({"type":"res","payload":{}})";
  ASSERT_FALSE(feedLit(hs, ok, kNowMs));
  ASSERT_EQ(hs.phase(), Phase::Authenticated);

  // A later challenge must not restart the handshake.
  EXPECT_FALSE(feed(hs, in, kNowMs));
  EXPECT_EQ(hs.phase(), Phase::Authenticated);
  EXPECT_EQ(hs.pendingFrameLen(), 0u);
}

TEST(Handshake, ReconfigureResetsSessionState) {
  OpenClaw::Handshake hs;
  const std::string in = challenge(kNonce, kNowMs);
  hs.configure(makeIdentity(), kToken, "");
  ASSERT_TRUE(feed(hs, in, kNowMs));
  ASSERT_EQ(hs.sentMsgId(), 1u);

  hs.configure(makeIdentity(), kToken, "");
  EXPECT_EQ(hs.phase(), Phase::WaitChallenge);
  EXPECT_EQ(hs.sentMsgId(), 0u);
  EXPECT_EQ(hs.pendingFrameLen(), 0u);
  ASSERT_TRUE(feed(hs, in, kNowMs));
  EXPECT_EQ(hs.sentMsgId(), 1u);
}

TEST(Constants, MessageIdWrapsAtTheReferenceCeiling) { EXPECT_EQ(MAX_MSG_ID, 99999u); }
