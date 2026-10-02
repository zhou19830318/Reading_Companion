#include "OpenClawSession.h"

// clang-format off
// HalStorage.h pulls SdFat macros that collide with lwip's ip4_addr.h unless
// seen first (same ordering constraint as OpenClawActivity.cpp).
#include <Arduino.h>
#include <CloudConfig.h>
#include <HalStorage.h>
// clang-format on
#include <ArduinoJson.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <NetBootstrap.h>
#include <OpenClawIdentity.h>
#include <OpenClawRedact.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

// The ESP-IDF certificate bundle compiled into this firmware. It is already on
// the link line because ssl_client.cpp references esp_crt_bundle_attach, so
// naming it here costs no flash. Handing it to beginSslWithBundle() keeps
// chain validation on: with no CA and no fingerprint the library would take
// the setInsecure() fallback instead (WebSocketsClient.cpp:294-295), which is
// not an option for a socket that will later carry credentials.
extern "C" {
extern const uint8_t _binary_x509_crt_bundle_start[];
extern const uint8_t _binary_x509_crt_bundle_end[];
}
namespace {
// Not constexpr: the bundle is an extern link-time symbol, so the span is only
// known after linking.
size_t defaultBundleSize() { return static_cast<size_t>(_binary_x509_crt_bundle_end - _binary_x509_crt_bundle_start); }
}  // namespace

namespace OpenClaw {

Session::~Session() { close(); }

Session& Session::instance() {
  static Session s;
  return s;
}

void Session::fail(const char* reason) {
  LOG_ERR("OCS", "failed: %s", reason != nullptr ? reason : "?");
  reason_ = reason;
  state_ = State::Failed;
  linkUp_ = false;
}

bool Session::allocChatBuffer() {
  if (chatBuf_ != nullptr) return true;
  // PSRAM first: with WiFi up the internal heap's largest block is ~4 KB and
  // this buffer only ever holds a frame or a reply — no DMA, no ISR. Internal
  // is the fallback so a PSRAM-less build still works.
  chatBuf_ = static_cast<char*>(heap_caps_malloc(CHAT_BUF_CAP, MALLOC_CAP_SPIRAM));
  const bool inPsram = chatBuf_ != nullptr;
  if (chatBuf_ == nullptr) {
    chatBuf_ = static_cast<char*>(heap_caps_malloc(CHAT_BUF_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (chatBuf_ == nullptr) {
    LOG_ERR("OCS", "OOM: chat buffer %u bytes", static_cast<unsigned>(CHAT_BUF_CAP));
    return false;
  }
  reply_.attach(chatBuf_, CHAT_BUF_CAP);
  LOG_INF("OCS", "chat buf %u B -> %s", static_cast<unsigned>(CHAT_BUF_CAP), inPsram ? "PSRAM" : "internal");
  return true;
}

void Session::freeChatBuffer() {
  if (chatBuf_ == nullptr) return;
  reply_.attach(nullptr, 0);  // before the free, so reply never points at dead memory
  heap_caps_free(chatBuf_);
  chatBuf_ = nullptr;
}

bool Session::loadCaBuffer() {
  caPem_ = makeUniqueNoThrow<char[]>(CA_PEM_SIZE);
  if (!caPem_) {
    LOG_ERR("OCS", "OOM: CA buffer %u bytes", static_cast<unsigned>(CA_PEM_SIZE));
    return false;
  }
  if (Storage.readFileToBuffer(caPath_, caPem_.get(), CA_PEM_SIZE) == 0) {
    LOG_ERR("OCS", "cannot read CA file %s", caPath_);
    caPem_.reset();
    return false;
  }
  return true;
}

bool Session::loadConfig() {
  CloudConfig& cloud = CLOUD_CFG;
  cloud.load();

  snprintf(host_, sizeof(host_), "%s", cloud.host);
  port_ = cloud.port;
  snprintf(path_, sizeof(path_), "%s", cloud.path);
  snprintf(protocol_, sizeof(protocol_), "%s", cloud.protocol);
  snprintf(caPath_, sizeof(caPath_), "%s", cloud.caPath);
  tls_ = cloud.tls;

  // Accept a full URI in the host field the same way AIWatch does
  // (openclaw_client.c:1377-1395): a scheme decides TLS, an explicit :port
  // inside the host overrides the stored port.
  if (strncmp(host_, "wss://", 6) == 0) {
    tls_ = true;
    memmove(host_, host_ + 6, strlen(host_ + 6) + 1);
  } else if (strncmp(host_, "ws://", 5) == 0) {
    tls_ = false;
    memmove(host_, host_ + 5, strlen(host_ + 5) + 1);
  }

  char* colon = strrchr(host_, ':');
  if (colon != nullptr && colon[1] != '\0' && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
    const long parsed = strtol(colon + 1, nullptr, 10);
    if (parsed > 0 && parsed <= 65535) {
      port_ = static_cast<uint16_t>(parsed);
      *colon = '\0';
    }
  }

  if (host_[0] == '\0') {
    LOG_ERR("OCS", "no OpenClaw host in NVS");
    fail(tr(STR_OPENCLAW_NO_CONFIG));
    return false;
  }
  if (path_[0] != '/') {
    LOG_ERR("OCS", "\"path\" must start with '/', got %s", path_);
    fail(tr(STR_OPENCLAW_BAD_CONFIG));
    return false;
  }
  // AIWatch refuses to dial without a credential (openclaw_client.c:346-349).
  if (cloud.token[0] == '\0' && cloud.deviceToken[0] == '\0') {
    LOG_ERR("OCS", "no shared token and no cached device token in NVS");
    fail(tr(STR_OPENCLAW_NO_TOKEN));
    return false;
  }

  // The ed25519 seed is self-generated on first use and then stays put.
  if (cloud.deviceKeyHex[0] == '\0') {
    char seed[CloudConfig::DEVICE_KEY_SIZE];
    if (!OpenClaw::generateSeedHex(seed, sizeof(seed)) || !cloud.setDeviceKey(seed)) {
      LOG_ERR("OCS", "could not generate a device key");
      fail(tr(STR_OPENCLAW_BAD_CONFIG));
      return false;
    }
    LOG_INF("OCS", "generated a new device key (first run)");
  }
  if (!OpenClaw::identityFromSeedHex(cloud.deviceKeyHex, identity_)) {
    LOG_ERR("OCS", "device key in NVS is not 64 hex characters");
    fail(tr(STR_OPENCLAW_BAD_CONFIG));
    return false;
  }
  LOG_INF("OCS", "config: %s://%s:%u%s", tls_ ? "wss" : "ws", host_, port_, path_);
  return true;
}

bool Session::startConnect() {
  if (WiFi.status() != WL_CONNECTED) {
    fail(tr(STR_STT_NO_NETWORK));
    return false;
  }
  NetBootstrap::ensureTlsAllocator();

  if (tls_) {
    if (!NetBootstrap::ensureSystemTime()) {
      fail(tr(STR_STT_NO_CLOCK));
      return false;
    }
    if (caPath_[0] != '\0' && !loadCaBuffer()) {
      fail(tr(STR_OPENCLAW_BAD_CONFIG));
      return false;
    }
    caLoaded_ = caPath_[0] != '\0';
  }

  if (!allocChatBuffer()) {
    fail(tr(STR_MIC_OOM));
    return false;
  }

  ws_ = makeUniqueNoThrow<WebSocketsClient>();
  handshake_ = makeUniqueNoThrow<Handshake>();
  if (!ws_ || !handshake_) {
    LOG_ERR("OCS", "OOM: socket/handshake");
    fail(tr(STR_MIC_OOM));
    return false;
  }
  const CloudConfig& cloud = CLOUD_CFG;
  handshake_->configure(identity_, cloud.token, cloud.deviceToken);
  if (handshake_->phase() == OpenClaw::Phase::Failed) {
    LOG_ERR("OCS", "handshake setup refused: %s", handshake_->errorCode());
    fail(tr(STR_OPENCLAW_BAD_CONFIG));
    return false;
  }

  ws_->onEvent([this](WStype_t type, uint8_t* payload, size_t length) { onWsEvent(type, payload, length); });
  ws_->setReconnectInterval(RECONNECT_MS);
  // RFC 6455 ping/pong: the gateway closed an idle socket after ~55 s, and
  // without application traffic nothing noticed until the next chat.send
  // failed. Armed once here — the library keeps the settings across its own
  // internal re-dials (they live on the client struct that begin() only
  // zeroes the connection fields of).
  ws_->enableHeartbeat(PING_INTERVAL_MS, PONG_TIMEOUT_MS, PONG_TIMEOUT_COUNT);
  // links2004 sends "Origin: file://" by default; the gateway rejects it with
  // INVALID_REQUEST: origin not allowed. A headless client sends no Origin.
  ws_->setExtraHeaders("");

  if (!tls_) {
    ws_->begin(host_, port_, path_, protocol_);
  } else if (caLoaded_) {
    ws_->beginSslWithCA(host_, port_, path_, caPem_.get(), protocol_);
  } else {
    LOG_INF("OCS", "TLS: IDF cert bundle, %u bytes", static_cast<unsigned>(defaultBundleSize()));
    ws_->beginSslWithBundle(host_, port_, path_, _binary_x509_crt_bundle_start, defaultBundleSize(), protocol_);
  }

  // First poll() pass runs the blocking TCP connect + TLS handshake (bounded
  // by the library's 5 s timeout) plus the HTTP upgrade request; the 101 reply
  // is read on the following pass.
  state_ = State::Connecting;
  pendingConnect_ = true;
  reconnectPending_ = false;
  reconnecting_ = false;
  reconnectAttempts_ = 0;
  snprintf(statusLine_, sizeof(statusLine_), "%s:%u %s", host_, port_, tls_ ? "wss" : "ws");
  return true;
}

bool Session::redial() {
  if (!ws_) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  // Same dial as startConnect(), minus the one-time allocations (chat buffer,
  // CA, handshake object) that a mid-session reconnect must not repeat.
  if (!tls_) {
    ws_->begin(host_, port_, path_, protocol_);
  } else if (caLoaded_) {
    ws_->beginSslWithCA(host_, port_, path_, caPem_.get(), protocol_);
  } else {
    ws_->beginSslWithBundle(host_, port_, path_, _binary_x509_crt_bundle_start, defaultBundleSize(), protocol_);
  }
  ws_->enableHeartbeat(PING_INTERVAL_MS, PONG_TIMEOUT_MS, PONG_TIMEOUT_COUNT);
  return true;
}

void Session::close() {
  if (ws_) {
    ws_->disconnect();
    ws_.reset();
  }
  handshake_.reset();
  caPem_.reset();
  freeChatBuffer();
  state_ = State::Idle;
  linkUp_ = false;
  // A deliberate teardown cancels any reconnect the socket had scheduled:
  // otherwise the next poll() on a live page would redial a null ws_, burn
  // the retry budget on it and end in Failed for a link nobody asked for.
  reconnectPending_ = false;
  reconnecting_ = false;
  reconnectAttempts_ = 0;
}

void Session::scheduleReconnect() {
  if (reconnectAttempts_ >= MAX_RECONNECTS) {
    LOG_ERR("OCS", "giving up after %u reconnects", static_cast<unsigned>(reconnectAttempts_));
    reconnecting_ = false;
    fail(tr(STR_OPENCLAW_DISCONNECTED));
    return;
  }
  const uint32_t delay = RECONNECT_MS * (reconnectAttempts_ + 1);
  reconnectPending_ = true;
  reconnecting_ = true;
  reconnectAtMs_ = millis() + delay;
  LOG_INF("OCS", "reconnect %u of %u in %lu ms", static_cast<unsigned>(reconnectAttempts_ + 1),
          static_cast<unsigned>(MAX_RECONNECTS), static_cast<unsigned long>(delay));
  snprintf(statusLine_, sizeof(statusLine_), "reconnecting in %lus", static_cast<unsigned long>(delay / 1000));
  notify();
}

void Session::poll() {
  if (pendingPersistToken_) {
    pendingPersistToken_ = false;
    const char* issued = handshake_ != nullptr ? handshake_->issuedDeviceToken() : nullptr;
    if (issued != nullptr) CLOUD_CFG.setDeviceToken(issued);
  }
  // Reconnect: the socket dropped (or a chat.send found it half-dead). The
  // wait happens here, not in the callback, so the WS dispatch never blocks.
  if (reconnectPending_ && static_cast<uint32_t>(millis() - reconnectAtMs_) < 0x80000000UL) {
    reconnectPending_ = false;
    if (redial()) {
      state_ = State::Connecting;
      pendingConnect_ = true;
    } else {
      // No Wi-Fi (or no socket): retry on the next pass, the clock keeps
      // running and the budget still bounds this loop.
      scheduleReconnect();
      return;
    }
  }
  // cron.list: queued by requestCronList(), sent here — same reason as the
  // retry above. Held back while a chat reply is outstanding so the two reply
  // paths can never interleave on one socket; it stays pending and goes out
  // on a later pass.
  if (cronReqPending_ && state_ == State::Connected && !awaitingReply_) {
    cronReqPending_ = false;
    char frame[192];
    // `id` is a STRING, like every other req on this socket: the gateway
    // validates `at /id: must be string` and answered a numeric id with
    // INVALID_REQUEST on device (2026-10-01, the frame below was never
    // exercised before the F4b bring-up).
    const size_t n = snprintf(frame, sizeof(frame),
                              "{\"type\":\"req\",\"id\":\"%lu\",\"method\":\"cron.list\","
                              "\"params\":{\"includeDisabled\":true}}",
                              static_cast<unsigned long>(nextMsgId_));
    const unsigned long reqId = static_cast<unsigned long>(nextMsgId_);
    nextMsgId_ = nextMsgId_ % MAX_MSG_ID + 1;
    if (ws_ != nullptr && ws_->sendTXT(frame, n)) {
      LOG_INF("OCS", "cron.list sent (id %lu, %u bytes)", reqId, static_cast<unsigned>(n));
    } else {
      LOG_ERR("OCS", "cron.list: socket write failed");
    }
  }
  // Re-send scheduled by a failure-only final. Not done in the callback:
  // handleChatFrame runs inside the WebSocket library's event dispatch, and
  // sendTXT() under it would re-enter the socket.
  if (retryPending_ && state_ == State::Connected) {
    retryPending_ = false;
    sendMessage(lastMessage_);
  }
  // Only the caller's notice changes once; fire the timeout at most once per
  // send. The deadline counts silence, not total time (sentAtMs_ is pushed
  // forward by every delta), so a slow but moving agent stays within it.
  // The socket stays open — a slow agent may still answer afterwards.
  if (awaitingReply_ && (millis() - sentAtMs_) >= CHAT_TIMEOUT_MS) {
    awaitingReply_ = false;
    outcome_ = SendOutcome::NoReply;
    LOG_ERR("OCS", "no chat reply after %lu ms", static_cast<unsigned long>(CHAT_TIMEOUT_MS));
    notify();
  }
  if (ws_) ws_->loop();
}

void Session::onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  const char* text = reinterpret_cast<const char*>(payload);

  switch (type) {
    case WStype_CONNECTED: {
      const Echo echo = safeEcho(text, length);
      LOG_INF("OCS", "WS connected: %.*s", echo.len, echo.text);
      snprintf(statusLine_, sizeof(statusLine_), "%.*s", echo.len, echo.text);
      state_ = State::Handshake;
      // The TCP-level re-dial after a loss lands here too; the OpenClaw
      // handshake is stateful, so it starts over from the challenge.
      if (handshake_ != nullptr) {
        handshake_->configure(identity_, CLOUD_CFG.token, CLOUD_CFG.deviceToken);
      }
      reconnecting_ = true;  // cleared on authentication, below
      break;
    }

    case WStype_TEXT: {
      [[maybe_unused]] const Echo echo = safeEcho(text, length);
      LOG_DBG("OCS", "WS text (%u bytes): %.*s", static_cast<unsigned>(length), echo.len, echo.text);

      bool paint = false;
      if (state_ != State::Connected && handshake_ != nullptr &&
          handshake_->onServerFrame(text, length, nowEpochMs())) {
        // Seed an unset system clock from the challenge's server "ts" —
        // must run after onServerFrame(), which is what parses `ts` into
        // handshake_->serverTs() (reading it before returns 0 and the MIN
        // epoch guard rejects it silently). The plaintext ws:// path never
        // runs ensureSystemTime(), so without this the clock stays at 1970
        // until some other sync (e.g. KOReader SNTP) happens to run.
        if (!NetBootstrap::systemTimeValid()) {
          NetBootstrap::setSystemTimeFromEpochMs(handshake_->serverTs());
        }
        // The gateway asked for a nonce and we have one: put the signed
        // connect request on the wire before it closes the window.
        const size_t sentLen = handshake_->pendingFrameLen();
        ws_->sendTXT(handshake_->pendingFrame(), sentLen);
        handshake_->clearPendingFrame();
        LOG_INF("OCS", "connect req sent (id %u, %u bytes)", handshake_->sentMsgId(), static_cast<unsigned>(sentLen));
        snprintf(statusLine_, sizeof(statusLine_), "connect req sent (id %u)", handshake_->sentMsgId());
        state_ = State::Challenge;
        paint = true;
      } else if (state_ != State::Connected && handshake_ != nullptr && handshake_->phase() == Phase::Authenticated) {
        state_ = State::Connected;
        linkUp_ = true;
        // Gateway uptime from this very frame's payload.snapshot.uptimeMs —
        // the only report a current gateway makes, and the one AIWatch's
        // dashboard renders. Captured here because the handshake object is
        // reset after authentication (close()/reconnect paths).
        if (handshake_->snapshotUptimeMs() > 0) {
          gatewayUptimeBaseMs_ = handshake_->snapshotUptimeMs();
          gatewayUptimeSeenMs_ = millis();
          LOG_DBG("OCS", "gateway uptime %llu ms (connect snapshot)",
                  static_cast<unsigned long long>(gatewayUptimeBaseMs_));
        }
        // Pull the workbench's to-do list on every (re)authentication. The
        // workbench owns no Session, so this is the only moment it can fetch;
        // the answer lands in the static cache and outlives this activity.
        // Flag only — poll() does the actual sendTXT.
        cronReqPending_ = true;
        snprintf(statusLine_, sizeof(statusLine_), "device %.12s", identity_.deviceId);
        if (handshake_->issuedDeviceToken() != nullptr) {
          pendingPersistToken_ = true;
        }
        LOG_INF("OCS", "authenticated as %s", identity_.deviceId);
        // L3 feasibility probe: what a gateway session costs to keep resident.
        // ESP.getFreeHeap() is DRAM-only, so this shows the TLS in/out buffers
        // (MBEDTLS_SSL_MAX_CONTENT_LEN=16384 -> ~32 KB) against the reader's
        // page-turn peak.
        LOG_INF("MEM", "session resident: free=%d min=%d maxalloc=%d", ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                ESP.getMaxAllocHeap());
        // A fresh authentication ends the reconnect cycle and restores the
        // full budget for the next loss.
        reconnecting_ = false;
        reconnectAttempts_ = 0;
        paint = true;
      } else if (state_ != State::Connected && handshake_ != nullptr && handshake_->phase() == Phase::Failed) {
        const Echo msg = safeEcho(handshake_->errorMessage());
        snprintf(lastError_, sizeof(lastError_), "%s: %s", handshake_->errorCode(), msg.text);
        LOG_ERR("OCS", "connect refused [%s]: %s", handshake_->errorCode(), msg.text);
        if (handshake_->requestId()[0] != '\0') {
          snprintf(approveCmd_, sizeof(approveCmd_), "openclaw devices approve %s", handshake_->requestId());
        }
        fail(tr(STR_OPENCLAW_AUTH_FAILED));
        paint = true;
      } else if (state_ == State::Connected) {
        // Everything after authentication is chat/agent traffic, handed to the
        // reply parser only while one of our own sends is outstanding.
        //
        // No blanket paint here. The gateway keeps a frame flowing when the
        // device is idle — measured 2026-09-28: `tick` every 30 s and `health`
        // every 30 s out of phase, each one triggering a full fast refresh
        // (~120 repaints per idle half hour: battery, ghosting, and it buried
        // the MEM heartbeat). handleChatFrame() notifies on exactly the frames
        // that change what the screen shows: the first visible delta, the
        // final, and errors.
        if (sniffSystemFrame(text, length)) {
          // Consumed (gateway uptime, later structured replies). Deliberately
          // no notify(): health/tick arrive every ~30 s and must not repaint.
        } else if (awaitingReply_) {
          handleChatFrame(text, length);
        }
      }

      if (paint) notify();
      break;
    }

    case WStype_DISCONNECTED:
    case WStype_ERROR: {
      const Echo echo = safeEcho(text, length);
      LOG_ERR("OCS", "WS %s: %.*s", type == WStype_ERROR ? "error" : "disconnected", echo.len, echo.text);
      // A lost socket no longer kills the session: schedule a re-dial (see
      // scheduleReconnect). Only an auth rejection (Phase::Failed above) or
      // the reconnect budget running out turns this into Session::Failed.
      linkUp_ = false;
      if (state_ != State::Failed) {
        if (awaitingReply_) {
          awaitingReply_ = false;
          outcome_ = SendOutcome::Failed;
        }
        scheduleReconnect();
      }
      break;
    }

    default:
      LOG_DBG("OCS", "WS event %d (%u bytes)", static_cast<int>(type), static_cast<unsigned>(length));
      break;
  }
}

// Non-chat traffic riding the same socket. The gateway emits `health` and
// `tick` about every 30 s while idle — frames the chat parser must never see
// (it would treat them as reply text) and the UI must never repaint for.
//
// Two-stage: a byte scan for a token only the interesting frames carry, so a
// chat delta never pays for a JSON parse; ArduinoJson only on a hit.
bool Session::sniffSystemFrame(const char* json, const size_t len) {
  const std::string_view frame(json, len);
  // Refresh path for gateways that DO put uptimeMs in a pushed event. A
  // current gateway does not — its `health` payload is ok/ts/durationMs/
  // eventLoop/plugins/configReload/channels/.../sessions and `tick` carries
  // only `ts` — which is why the connect snapshot (Handshake::snapshotUptimeMs,
  // copied at authentication) is the source that actually populates this.
  // Keeping the branch costs one byte scan per frame and self-heals if the
  // gateway grows the field again.
  if (frame.find("\"jobs\"") != std::string_view::npos) {
    storeCronJobs(json, len);
    return true;
  }
  if (frame.find("\"uptimeMs\"") == std::string_view::npos) {
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
    LOG_DBG("OCS", "system frame parse failed (%u bytes)", static_cast<unsigned>(len));
    return true;  // ours to drop, even though it was unreadable
  }

  const int64_t uptimeMs = doc["payload"]["uptimeMs"] | int64_t(0);
  // 10-year sanity bound — a field long enough to be nonsense must not reach
  // the card. (uint64 storage also means a service up past the 49.7-day
  // uint32 limit still renders correctly.)
  if (uptimeMs > 0 && uptimeMs <= 315360000000LL) {
    if (gatewayUptimeBaseMs_ != static_cast<uint64_t>(uptimeMs)) {
      gatewayUptimeBaseMs_ = static_cast<uint64_t>(uptimeMs);
      gatewayUptimeSeenMs_ = millis();
      LOG_DBG("OCS", "gateway uptime %llu ms", static_cast<unsigned long long>(gatewayUptimeBaseMs_));
    }
    return true;
  }
  return true;
}

void Session::finishSend(SendOutcome outcome, const char* logLine) {
  awaitingReply_ = false;
  outcome_ = outcome;
  if (logLine != nullptr) LOG_INF("OCS", "%s", logLine);
  notify();
}

void Session::handleChatFrame(const char* json, size_t len) {
  switch (reply_.feed(json, len)) {
    case ChatFrame::Delta: {
      // The agent is talking: restart the inactivity clock so a long tool
      // turn (alarm, cron, …) that streams a preamble is not cut off in the
      // middle of working — see CHAT_TIMEOUT_MS.
      sentAtMs_ = millis();
      const size_t stripped = reply_.stripFailureMarkers();
      // Emoji in a streamed reply would hit the panel as .notdef boxes the
      // moment the first partial repaint lands — strip them at delta speed
      // too, not only at final.
      const size_t symbolStripped = reply_.stripStrippableSymbols();
      LOG_DBG("OCS", "chat delta: frame %u bytes, reply %u bytes", static_cast<unsigned>(len),
              static_cast<unsigned>(reply_.textSize()));
      if (stripped > 0) LOG_DBG("OCS", "stripped %u markers while streaming", static_cast<unsigned>(stripped));
      if (symbolStripped > 0) {
        LOG_DBG("OCS", "stripped %u symbol bytes while streaming", static_cast<unsigned>(symbolStripped));
      }
      // One repaint when the answer first becomes visible; the rest of the
      // stream accumulates silently until it settles.
      if (!paintedPartial_ && reply_.textSize() > 0) {
        paintedPartial_ = true;
        notify();
      }
      break;
    }

    case ChatFrame::Final: {
      size_t stripped = reply_.stripFailureMarkers();
      if (stripped > 0) {
        LOG_INF("OCS", "stripped %u gateway failure markers", static_cast<unsigned>(stripped));
      }
      // E-ink draws plain text: the model's **bold**/*em*/`code` markers would
      // reach the screen verbatim (measured 2026-09-28: "明早 **8:00** 会叫你起床").
      // Done here so every consumer — the panel, the history line, the JSONL
      // log — sees the same cleaned text.
      stripped = reply_.stripMarkdownMarkers();
      if (stripped > 0) LOG_DBG("OCS", "stripped %u markdown bytes", static_cast<unsigned>(stripped));
      // Last: emoji/symbols no font can draw (the panel and the history line
      // both read replyText() after this point — one strip covers both).
      stripped = reply_.stripStrippableSymbols();
      if (stripped > 0) LOG_DBG("OCS", "stripped %u symbol bytes", static_cast<unsigned>(stripped));
      snprintf(statusLine_, sizeof(statusLine_), "reply %u bytes%s", static_cast<unsigned>(reply_.textSize()),
               reply_.truncated() ? " (truncated)" : "");
      LOG_INF("OCS", "chat final: %u bytes", static_cast<unsigned>(reply_.textSize()));
      LOG_INF("OCS", "reply: %s", reply_.text());
      // A healthy round trip can still come back as the gateway's own failure
      // marker with no content. One re-send is worth trying; after that the
      // caller states the failure rather than showing the marker.
      if (isAgentFailureReply(reply_.text())) {
        if (chatAttempts_ < MAX_CHAT_ATTEMPTS) {
          retryPending_ = true;
          LOG_INF("OCS", "reply was only the failure marker, re-sending (%u of %u)",
                  static_cast<unsigned>(chatAttempts_) + 1, static_cast<unsigned>(MAX_CHAT_ATTEMPTS));
        } else {
          finishSend(SendOutcome::AgentFailed, "agent failure after retries");
          reply_.reset();  // logged above; nothing usable to draw
        }
      } else {
        finishSend(SendOutcome::Sent, "chat round trip complete");
      }
      break;
    }

    case ChatFrame::Error: {
      const Echo msg = safeEcho(reply_.error());
      snprintf(lastError_, sizeof(lastError_), "%s", msg.text);
      snprintf(statusLine_, sizeof(statusLine_), "chat error");
      finishSend(SendOutcome::Failed, nullptr);
      break;
    }

    case ChatFrame::Aborted:
      finishSend(SendOutcome::Aborted, "chat aborted by the gateway");
      break;

    case ChatFrame::Ignored:
      break;
  }
}

uint32_t Session::awaitingMs() const { return awaitingReply_ ? (millis() - sentAtMs_) : 0; }

Session::SendOutcome Session::sendMessage(const char* message) {
  if (ws_ == nullptr || state_ != State::Connected) return SendOutcome::Failed;
  if (chatBuf_ == nullptr) return SendOutcome::Failed;
  if (message != nullptr) {
    snprintf(lastMessage_, sizeof(lastMessage_), "%s", message);
  }
  ++chatAttempts_;

  // Unique per send: the gateway suppresses a repeat of an idempotency key it
  // has already seen.
  char key[40];
  snprintf(key, sizeof(key), "cp-%08lx-%lu", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(nextMsgId_));
  const uint32_t sentId = nextMsgId_;

  const size_t n = buildChatSendFrame(chatBuf_, CHAT_BUF_CAP, nextMsgId_, message, key);
  nextMsgId_ = nextMsgId_ % MAX_MSG_ID + 1;
  if (n == 0) {
    LOG_ERR("OCS", "chat.send frame did not fit in %u bytes", static_cast<unsigned>(CHAT_BUF_CAP));
    return SendOutcome::Failed;
  }

  // sendTXT() copies the frame to the socket before returning, so the same
  // memory can start collecting the answer immediately afterwards. attach()
  // zeroes the first byte, which is why it runs after the send.
  const bool ok = ws_->sendTXT(chatBuf_, n);
  reply_.attach(chatBuf_, CHAT_BUF_CAP);
  paintedPartial_ = false;
  sentAtMs_ = millis();
  awaitingReply_ = ok;
  outcome_ = ok ? SendOutcome::Sent : SendOutcome::Failed;
  snprintf(statusLine_, sizeof(statusLine_), "%s id %lu (%u bytes)", ok ? "sent chat" : "chat send failed",
           static_cast<unsigned long>(sentId), static_cast<unsigned>(n));
  if (ok) {
    LOG_INF("OCS", "chat.send id %lu, %u bytes", static_cast<unsigned long>(sentId), static_cast<unsigned>(n));
  } else {
    LOG_ERR("OCS", "chat.send id %lu: socket write failed", static_cast<unsigned long>(sentId));
  }
  return outcome_;
}

uint64_t Session::gatewayUptimeMs() {
  if (gatewayUptimeSeenMs_ == 0) return 0;
  // The gateway reports its uptime only on (re)connect — its pushed health
  // event carries none — so the value is extrapolated with local time. A
  // gateway restart drops this socket, and the next handshake re-seeds the
  // base, so the correction arrives by itself. The subtraction is uint32
  // (millis() wraparound-safe) before widening: elapsed must not be computed
  // in 64-bit against a wrapped clock.
  return gatewayUptimeBaseMs_ + static_cast<uint64_t>(static_cast<uint32_t>(millis() - gatewayUptimeSeenMs_));
}

const CronJob* Session::cronJobs() { return cronJobs_; }

size_t Session::cronJobCount() { return cronJobCount_; }

bool Session::cronValid() { return cronValid_; }

void Session::requestCronList() {
  if (state_ != State::Connected) {
    LOG_DBG("OCS", "cron.list deferred (state %d)", static_cast<int>(state_));
    cronReqPending_ = true;  // flushed by poll() once the link is back up
    return;
  }
  cronReqPending_ = true;
}

// cron.list reply -> static cache. The workbench reads this without owning a
// Session, so the cache outlives the activity that fetched it; it is
// invalidated only by a parse failure, never by a socket drop (a stale list
// is more useful than an empty one).
void Session::storeCronJobs(const char* json, const size_t len) {
  size_t stored = 0;
  // userJobsOnly: the card lists what the user set, so the gateway's own
  // routines (heartbeat, skill review, memory dreaming) are dropped here
  // rather than at paint time — every consumer of the cache then agrees.
  const CronParse verdict = parseCronList(json, len, cronJobs_, MAX_CRON_JOBS, &stored, /*userJobsOnly=*/true);
  switch (verdict) {
    case CronParse::Error:
      LOG_ERR("OCS", "cron.list reply parse failed (%u bytes)", static_cast<unsigned>(len));
      cronValid_ = false;
      cronJobCount_ = 0;
      return;
    case CronParse::NoJobs:
      // Well-formed frame, wrong shape for us — keep the previous list (a
      // transient unrelated "jobs" frame must not blank the card).
      LOG_ERR("OCS", "cron.list reply has no payload.jobs array");
      return;
    case CronParse::Ok:
      break;
  }

  size_t active = 0;
  size_t running = 0;
  for (size_t i = 0; i < stored; ++i) {
    if (cronJobs_[i].enabled) ++active;
    if (cronJobs_[i].running) ++running;
  }
  cronJobCount_ = stored;
  cronValid_ = true;
  LOG_INF("OCS", "cron.list: %u jobs (%u enabled, %u running)", static_cast<unsigned>(stored),
          static_cast<unsigned>(active), static_cast<unsigned>(running));
  for (size_t i = 0; i < stored; ++i) {
    LOG_DBG("OCS", "cron shown: %s (key %s)", cronJobs_[i].name,
            cronJobs_[i].declarationKey[0] != '\0' ? cronJobs_[i].declarationKey : "-");
  }
  // Same diagnostic for the text the card shortens rows against: payload.text
  // is the only human wording the gateway keeps, and its shape (emoji, full
  // width punctuation, spacing) decides whether a row prints as words or as
  // the raw sentence. One line per sync, never per frame; compiled out at
  // LOG_LEVEL 0.
  for (size_t i = 0; i < stored; ++i) {
    const char* t = cronJobs_[i].text;
    const size_t tl = strlen(t);
    char head[25] = {};
    for (size_t k = 0; k < 8; ++k) {
      const size_t used = strlen(head);
      snprintf(head + used, sizeof(head) - used, "%02X", k < tl ? static_cast<uint8_t>(t[k]) : 0u);
    }
    LOG_DBG("OCS", "cron text[%u] len=%u head=%s <%s>", static_cast<unsigned>(i), static_cast<unsigned>(tl), head, t);
  }
  // Unlike health/tick (silently consumed), a cron.list reply is data the
  // user asked for — one bounded repaint when it lands. It can only arrive
  // after requestCronList(), so this is never a periodic event.
  notify();
}

}  // namespace OpenClaw
