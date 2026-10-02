#include "OpenClawProbe.h"

// clang-format off
// HalStorage.h pulls SdFat macros that collide with lwip's ip4_addr.h unless
// seen first (same ordering constraint as lib/voice/SttClient.cpp and
// src/activities/settings/OpenClawActivity.cpp).
#include <Arduino.h>
#include <HalStorage.h>
// clang-format on
#include <Logging.h>
#include <Memory.h>
#include <NetBootstrap.h>
#include <OpenClawHandshake.h>
#include <OpenClawIdentity.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <cstdio>
#include <cstring>

// The ESP-IDF certificate bundle compiled into this firmware; identical
// linkage story to the one in OpenClawActivity.cpp (see the comment there).
extern "C" {
extern const uint8_t _binary_x509_crt_bundle_start[];
extern const uint8_t _binary_x509_crt_bundle_end[];
}

namespace {

// Mirrors OpenClawActivity::CA_PEM_SIZE.
constexpr size_t CA_PEM_SIZE = 2400;
// Our own ceiling for the TCP reachability probe.
constexpr int32_t TCP_TIMEOUT_MS = 3000;
// WebSocketsClient blocks in connect() for WEBSOCKETS_TCP_TIMEOUT (5000 ms)
// per attempt, so this bounds the whole upgrade exchange to one attempt.
constexpr uint32_t HANDSHAKE_DEADLINE_MS = 5500;
// S1's acceptance criterion is the server speaking first; give a frame a
// bounded wait rather than returning on the 101 alone.
constexpr uint32_t FIRST_FRAME_WAIT_MS = 2500;
// How long after our connect request we wait for the gateway's verdict. Long
// enough for a round trip to 118.25.20.250, short enough that the browser tab
// does not look hung.
constexpr uint32_t AUTH_DEADLINE_MS = 6000;

size_t defaultBundleSize() { return static_cast<size_t>(_binary_x509_crt_bundle_end - _binary_x509_crt_bundle_start); }

// payload is not NUL-terminated; copy at most cap-1 bytes and terminate.
void copyTrunc(char* dst, size_t cap, const uint8_t* src, size_t len) {
  if (cap == 0) return;
  if (len >= cap) len = cap - 1;
  if (len > 0 && src != nullptr) memcpy(dst, src, len);
  dst[len] = '\0';
}

void fail(OpenClawProbe::Result& out, const char* stage, const char* detail) {
  out.ok = false;
  out.stage = stage;
  snprintf(out.detail, sizeof(out.detail), "%s", detail);
}

}  // namespace

void OpenClawProbe::run(const Config& cfg, Result& out) {
  out = Result{};
  snprintf(out.url, sizeof(out.url), "%s://%s:%u%s", cfg.tls ? "wss" : "ws", cfg.host, cfg.port, cfg.path);
  const uint32_t t0 = millis();

  if (cfg.host[0] == '\0') return fail(out, "config", "host is empty");
  if (cfg.path[0] != '/') return fail(out, "config", "path must start with /");
  if (WiFi.status() != WL_CONNECTED) return fail(out, "network", "WiFi is not connected");

  // TLS reads the system clock for certificate validity, and mbedTLS needs
  // its allocator in place before the first handshake — same two preconditions
  // the on-device screen enforces.
  NetBootstrap::ensureTlsAllocator();
  if (cfg.tls && !NetBootstrap::ensureSystemTime()) {
    out.elapsedMs = millis() - t0;
    return fail(out, "clock", "system clock not set; SNTP has never synced");
  }

  // Cheap, precise separation between "host/port unreachable" and "reachable
  // but the upgrade was refused": without this a DNS or routing failure and a
  // bad subprotocol both look like one generic timeout.
  {
    WiFiClient tcp;
    if (!tcp.connect(cfg.host, cfg.port, TCP_TIMEOUT_MS)) {
      out.elapsedMs = millis() - t0;
      return fail(out, "tcp", "TCP connect failed (host down, wrong port, or DNS failed)");
    }
    tcp.stop();
  }

  std::unique_ptr<char[]> caPem;
  bool caLoaded = false;
  if (cfg.tls && cfg.caPath[0] != '\0') {
    caPem = makeUniqueNoThrow<char[]>(CA_PEM_SIZE);
    if (!caPem) {
      out.elapsedMs = millis() - t0;
      return fail(out, "ca", "out of memory reading CA file");
    }
    if (Storage.readFileToBuffer(cfg.caPath, caPem.get(), CA_PEM_SIZE) == 0) {
      out.elapsedMs = millis() - t0;
      return fail(out, "ca", "cannot read CA file from SD");
    }
    caLoaded = true;
  }

  auto ws = makeUniqueNoThrow<WebSocketsClient>();
  if (!ws) {
    out.elapsedMs = millis() - t0;
    return fail(out, "connect", "out of memory allocating WebSocket client");
  }

  // The identity and the handshake are built up front so the event callback
  // can push every frame straight into the state machine, which is what lets
  // this stay a single blocking call instead of a pump loop across UI frames.
  char ephemeralSeed[CloudConfig::DEVICE_KEY_SIZE];
  const char* seedHex = cfg.deviceKeyHex;
  if (seedHex[0] == '\0') {
    // The probe promises not to write NVS, so a missing key is derived and
    // thrown away rather than persisted the way OpenClawActivity persists it.
    if (!OpenClaw::generateSeedHex(ephemeralSeed, sizeof(ephemeralSeed))) {
      out.elapsedMs = millis() - t0;
      return fail(out, "auth", "no device key stored and the RNG is unavailable");
    }
    seedHex = ephemeralSeed;
  }
  OpenClaw::Identity identity{};
  if (!OpenClaw::identityFromSeedHex(seedHex, identity)) {
    out.elapsedMs = millis() - t0;
    return fail(out, "auth", "device key is not 64 hex characters");
  }

  auto handshake = makeUniqueNoThrow<OpenClaw::Handshake>();
  if (!handshake) {
    out.elapsedMs = millis() - t0;
    return fail(out, "auth", "out of memory allocating handshake");
  }
  handshake->configure(identity, cfg.token, cfg.deviceToken);
  if (handshake->phase() == OpenClaw::Phase::Failed) {
    out.elapsedMs = millis() - t0;
    return fail(out, "auth", handshake->errorCode());
  }

  bool connected = false;
  bool stopped = false;
  bool sentConnect = false;
  ws->onEvent([&out, &connected, &stopped, &sentConnect, &handshake, ws = ws.get()](WStype_t type, uint8_t* payload,
                                                                                    size_t length) {
    switch (type) {
      case WStype_CONNECTED:
        connected = true;
        // The library hands back the request URL here, not the negotiated
        // subprotocol; it still proves the 101 came back.
        copyTrunc(out.detail, sizeof(out.detail), payload, length);
        break;
      case WStype_TEXT:
      case WStype_FRAGMENT_TEXT_START:
        if (out.firstFrame[0] == '\0') copyTrunc(out.firstFrame, sizeof(out.firstFrame), payload, length);
        if (type == WStype_TEXT &&
            handshake->onServerFrame(reinterpret_cast<const char*>(payload), length, OpenClaw::nowEpochMs())) {
          ws->sendTXT(handshake->pendingFrame(), handshake->pendingFrameLen());
          handshake->clearPendingFrame();
          sentConnect = true;
        }
        break;
      case WStype_DISCONNECTED:
      case WStype_ERROR:
        stopped = true;
        copyTrunc(out.detail, sizeof(out.detail), payload, length);
        break;
      default:
        break;
    }
  });
  // Zero so the very first attempt is not gated on _lastConnectionFail (it is
  // not initialised by the constructor), then raised so a failed upgrade
  // cannot retry inside the deadline and burn another 5 s per pass.
  ws->setReconnectInterval(0);
  // links2004 sends "Origin: file://" by default; the gateway rejects it with
  // INVALID_REQUEST: origin not allowed. A headless client sends no Origin —
  // that is what esp_websocket_client (AIWatch) does.
  ws->setExtraHeaders("");

  if (!cfg.tls) {
    ws->begin(cfg.host, cfg.port, cfg.path, cfg.protocol);
  } else if (caLoaded) {
    ws->beginSslWithCA(cfg.host, cfg.port, cfg.path, caPem.get(), cfg.protocol);
  } else {
    ws->beginSslWithBundle(cfg.host, cfg.port, cfg.path, _binary_x509_crt_bundle_start, defaultBundleSize(),
                           cfg.protocol);
  }

  // First pass performs the blocking TCP connect + TLS handshake + upgrade
  // request; the 101 reply is consumed on a later pass.
  ws->loop();
  ws->setReconnectInterval(HANDSHAKE_DEADLINE_MS);

  const uint32_t handshakeDeadline = millis() + HANDSHAKE_DEADLINE_MS;
  while (millis() < handshakeDeadline && !connected && !stopped) {
    ws->loop();
    delay(10);
  }

  if (!connected) {
    if (out.detail[0] == '\0') {
      snprintf(out.detail, sizeof(out.detail), "no WebSocket 101 within %u ms", (unsigned)HANDSHAKE_DEADLINE_MS);
    }
    ws->disconnect();
    out.elapsedMs = millis() - t0;
    out.ok = false;
    out.stage = "connect";
    return;
  }

  const uint32_t frameDeadline = millis() + FIRST_FRAME_WAIT_MS;
  while (millis() < frameDeadline && out.firstFrame[0] == '\0' && !stopped) {
    ws->loop();
    delay(10);
  }

  if (out.firstFrame[0] == '\0') {
    ws->disconnect();
    out.elapsedMs = millis() - t0;
    out.ok = true;
    out.stage = "ws";
    if (out.detail[0] == '\0') {
      snprintf(out.detail, sizeof(out.detail), "connected, no frame within %u ms", (unsigned)FIRST_FRAME_WAIT_MS);
    }
    return;
  }

  // Reachable and speaking: the S1 result stands on its own. Now find out
  // whether it will let us in.
  const uint32_t authDeadline = millis() + AUTH_DEADLINE_MS;
  while (millis() < authDeadline && !stopped && handshake->phase() != OpenClaw::Phase::Authenticated &&
         handshake->phase() != OpenClaw::Phase::Failed) {
    ws->loop();
    delay(10);
  }
  ws->disconnect();
  out.elapsedMs = millis() - t0;

  switch (handshake->phase()) {
    case OpenClaw::Phase::Authenticated:
      out.ok = true;
      out.stage = "ok";
      snprintf(out.detail, sizeof(out.detail), "authenticated as %s", identity.deviceId);
      return;

    case OpenClaw::Phase::Failed:
      out.ok = false;
      out.stage = "auth";
      snprintf(out.detail, sizeof(out.detail), "%s: %s", handshake->errorCode(), handshake->errorMessage());
      // NOT_PAIRED carries the id needed to run `openclaw devices approve`;
      // it is useless buried in a log line nobody is watching.
      snprintf(out.requestId, sizeof(out.requestId), "%s", handshake->requestId());
      return;

    default:
      out.ok = false;
      out.stage = "auth";
      if (!sentConnect) {
        snprintf(out.detail, sizeof(out.detail), "first frame was not connect.challenge");
      } else if (stopped) {
        snprintf(out.detail, sizeof(out.detail), "server closed the connection during authentication");
      } else {
        snprintf(out.detail, sizeof(out.detail), "no response to connect within %u ms", (unsigned)AUTH_DEADLINE_MS);
      }
      return;
  }
}
