#include "VoiceRelayClient.h"

// clang-format off
// Arduino.h first (WebSockets pulls lwip; the same ordering constraint as
// OpenClawSession.cpp documents for its SDK headers).
#include <Arduino.h>
// clang-format on
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>

// The ESP-IDF certificate bundle is already on the link line (ssl_client.cpp
// references it), so naming it here costs no flash and keeps wss:// dials on
// chain validation instead of the library's setInsecure() fallback.
extern "C" {
extern const uint8_t _binary_x509_crt_bundle_start[];
extern const uint8_t _binary_x509_crt_bundle_end[];
}
namespace {
// Not constexpr: the bundle is a link-time symbol, so the span only exists
// after linking (same helper as OpenClawSession.cpp).
size_t defaultBundleSize() { return static_cast<size_t>(_binary_x509_crt_bundle_end - _binary_x509_crt_bundle_start); }
}  // namespace

namespace VoiceRelay {

Client::~Client() { close(); }

bool Client::start(const char* url, const char* token, const char* deviceId, const char* mode) {
  if (state_ == State::Connecting && warm_) {
    // Adopt the socket prepare() opened: the dial already happened, so the
    // capture window sees only a queued session.start, flushed by poll().
    warm_ = false;
    if (!buildRound(mode)) return false;
    startPending_ = wsOpen_;
    return true;
  }
  if (state_ == State::Done) {
    // The finished session's socket is still open: the server replaces the
    // session on a fresh session.start. A socket the server has since closed
    // (its 60 s idle) reads wsOpen_ false — kept that way by the upkeep poll —
    // and dials again here; a socket that died unnoticed just fails this
    // round into the activity's legacy fallback.
    if (!wsOpen_ && !setUp(url, token, deviceId, false)) return false;
    if (!buildRound(mode)) return false;
    state_ = State::Connecting;
    startPending_ = wsOpen_;
    return true;
  }
  if (state_ != State::Idle) {
    LOG_ERR("VRC", "start() while state=%u; close() first", static_cast<unsigned>(state_));
    return false;
  }
  if (!setUp(url, token, deviceId, false)) return false;
  if (!buildRound(mode)) return false;
  state_ = State::Connecting;
  return true;
}

bool Client::prepare(const char* url, const char* token, const char* deviceId) {
  if (state_ != State::Idle) return false;
  if (!setUp(url, token, deviceId, true)) return false;
  state_ = State::Connecting;
  warm_ = true;
  LOG_INF("VRC", "warm dial armed");
  return true;
}

bool Client::setUp(const char* url, const char* token, const char* deviceId, bool warm) {
  // A missed warm-up must only mean "no socket yet": stay Idle so the next
  // round dials (and reports the failure) the normal way.
  const auto reject = [this, warm](TransportError error) {
    if (warm) {
      transportError_ = error;
    } else {
      fail(error);
    }
    return false;
  };
  if (url == nullptr || token == nullptr || deviceId == nullptr) return reject(TransportError::BadConfig);
  if (!WiFi.isConnected()) return reject(TransportError::NoWifi);
  url_ = parseRelayUrl(url);
  const size_t tokenLen = strlen(token);
  const size_t deviceLen = strlen(deviceId);
  if (!url_.valid || tokenLen == 0 || tokenLen > TOKEN_CAP - 8 || deviceLen == 0 || deviceLen >= DEVICE_ID_CAP) {
    LOG_ERR("VRC", "bad config: url=%d tokenLen=%u deviceLen=%u", url_.valid ? 1 : 0, static_cast<unsigned>(tokenLen),
            static_cast<unsigned>(deviceLen));
    return reject(TransportError::BadConfig);
  }

  transportError_ = TransportError::None;
  startPending_ = false;
  audioStartPending_ = false;
  wsOpen_ = false;
  releasePending_ = false;
  snprintf(deviceId_, sizeof(deviceId_), "%s", deviceId);

  releaseSocket();
  ws_ = makeUniqueNoThrow<WebSocketsClient>();
  if (!ws_) {
    LOG_ERR("VRC", "OOM: WebSocketsClient");
    return reject(TransportError::Oom);
  }

  char auth[TOKEN_CAP];
  snprintf(auth, sizeof(auth), "Bearer %s", token);
  char headers[HEADERS_CAP];
  snprintf(headers, sizeof(headers), "X-Device-Id: %s\r\nX-Voice-Protocol: 1", deviceId_);

  ws_->onEvent([this](WStype_t type, uint8_t* payload, size_t length) { onWsEvent(type, payload, length); });
  // Small redial throttle; see REDIAL_INTERVAL_MS. The state machine owns
  // teardown (fail() -> releaseSocket()), so the library cannot redial a
  // session the server has already forgotten.
  ws_->setReconnectInterval(REDIAL_INTERVAL_MS);
  ws_->enableHeartbeat(PING_INTERVAL_MS, PONG_TIMEOUT_MS, PONG_TIMEOUT_COUNT);
  if (!url_.tls) {
    ws_->begin(url_.host, url_.port, url_.path, "");
  } else {
    LOG_INF("VRC", "TLS: IDF cert bundle, %u bytes", static_cast<unsigned>(defaultBundleSize()));
    ws_->beginSslWithBundle(url_.host, url_.port, url_.path, _binary_x509_crt_bundle_start, defaultBundleSize(), "");
  }
  // After begin(): begin() zeroes the connection fields it "only zeroes"
  // (plainAuthorization among them, WebSocketsClient.cpp:75-76), so a Bearer
  // set before it is silently dropped and the upgrade is rejected with
  // UNAUTHORIZED (missing bearer token).
  ws_->setAuthorization(auth);
  ws_->setExtraHeaders(headers);

  dialedAtMs_ = millis();
  LOG_INF("VRC", "dialing %s://%s:%u%s", url_.tls ? "wss" : "ws", url_.host, static_cast<unsigned>(url_.port),
          url_.path);
  return true;
}

bool Client::buildRound(const char* mode) {
  lastEvent_ = Event{};
  localLatency_ = Latency{};
  sessionId_[0] = '\0';
  ending_ = false;
  framer_.reset();
  formatRequestId(requestId_, esp_random(), esp_random(), esp_random(), esp_random());
  startFrameLen_ = buildSessionStart(startFrame_, sizeof(startFrame_), requestId_, deviceId_, mode);
  if (startFrameLen_ == 0) {
    LOG_ERR("VRC", "session.start build failed (cap=%u requestId=%u deviceId=%u)",
            static_cast<unsigned>(sizeof(startFrame_)), static_cast<unsigned>(strlen(requestId_)),
            static_cast<unsigned>(strlen(deviceId_)));
    fail(TransportError::BadConfig);
    return false;
  }
  return true;
}

void Client::poll() {
  if (state_ == State::Failed) {
    releaseSocket();
    return;
  }
  if (releasePending_) {
    // A warm socket closed from inside its own dispatch; the client cannot be
    // freed under the library's loop() (see onWsEvent).
    releasePending_ = false;
    releaseSocket();
    return;
  }
  if (!ws_) return;

  // First pass runs the blocking TCP connect (and TLS handshake) plus the
  // upgrade request; the 101 is read on a following pass.
  ws_->loop();
  if (startPending_) sendStart();
  if (audioStartPending_) sendAudioStart();
  if (state_ == State::Connecting && !wsOpen_ && elapsed(dialedAtMs_) > CONNECT_TIMEOUT_MS) {
    if (warm_) {
      // An unused warm socket that never opened is not a round's failure:
      // park at Idle and let the next round dial (and report) it cold.
      LOG_INF("VRC", "warm dial timed out");
      releaseSocket();  // clears warm_/wsOpen_ as well
      state_ = State::Idle;
      return;
    }
    fail(TransportError::ConnectTimeout);
  }
  if (state_ == State::Starting && elapsed(startSentAtMs_) > START_TIMEOUT_MS) fail(TransportError::StartTimeout);
  if (state_ == State::Active && ending_ && elapsed(endSentAtMs_) > DONE_TIMEOUT_MS) fail(TransportError::DoneTimeout);
  if (state_ == State::Failed) releaseSocket();
}

void Client::close() {
  if (state_ == State::Active && ws_ != nullptr) {
    // startFrame_ is free once session.start is on the wire; reuse it so the
    // FRAME_CAP buffer never lands on the stack.
    const size_t n = buildSessionCancel(startFrame_, sizeof(startFrame_), requestId_, sessionId_);
    // Best effort: an abrupt disconnect also frees the server-side session.
    if (n > 0) ws_->sendTXT(startFrame_, n);
  }
  releaseSocket();
  state_ = State::Idle;
  transportError_ = TransportError::None;
  ending_ = false;
  framer_.reset();
}

bool Client::sendAudio(const uint8_t* pcm, size_t len) {
  if (state_ != State::Active || ws_ == nullptr || ending_ || pcm == nullptr || len == 0) return false;
  framer_.feed(pcm, len, &Client::frameSink, this);
  return true;
}

bool Client::finishAudio(const char* reason) {
  if (state_ != State::Active || ws_ == nullptr || ending_) return false;
  const size_t n = buildAudioEnd(startFrame_, sizeof(startFrame_), requestId_, sessionId_, reason);
  if (n == 0) return false;
  if (!ws_->sendTXT(startFrame_, n)) {
    fail(TransportError::SocketLost);
    return false;
  }
  ending_ = true;
  endSentAtMs_ = millis();
  LOG_INF("VRC", "audio.end sent (%s)", reason != nullptr ? reason : "?");
  return true;
}

void Client::onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      const uint32_t connectMs = elapsed(dialedAtMs_);
      wsOpen_ = true;
      LOG_INF("VRC", "ws open in %ld ms%s", static_cast<long>(connectMs), warm_ ? " (warm)" : "");
      if (!warm_) {
        // Armed here, sent by the same poll() pass — the WS dispatch never
        // writes to the socket. A warm socket is armed by start() instead.
        localLatency_.connect = static_cast<int32_t>(connectMs);
        startPending_ = true;
      }
      return;
    }
    case WStype_TEXT:
      if (payload != nullptr && length > 0) handleText(reinterpret_cast<const char*>(payload), length);
      return;
    case WStype_DISCONNECTED:
      wsOpen_ = false;
      if (state_ == State::Connecting && warm_ && !startPending_) {
        // The unused warm socket closed (server 60 s idle, wifi flap): not a
        // round's failure. Park at Idle; poll() releases the client, which
        // cannot happen from inside its own dispatch.
        LOG_INF("VRC", "warm socket closed");
        warm_ = false;
        state_ = State::Idle;
        releasePending_ = true;
        return;
      }
      if (state_ == State::Connecting || state_ == State::Starting || state_ == State::Active) {
        fail(TransportError::SocketLost);
      }
      return;
    default:
      // WStype_ERROR is always followed by DISCONNECTED; server->device
      // binary frames do not exist in this protocol.
      LOG_DBG("VRC", "ws event %u (payload %u)", static_cast<unsigned>(type), static_cast<unsigned>(length));
      return;
  }
}

void Client::handleText(const char* json, size_t len) {
  Event event;
  if (!parseEvent(json, len, event)) {
    LOG_DBG("VRC", "unparsed frame (%u bytes)", static_cast<unsigned>(len));
    return;
  }
  handleEvent(event);
}

void Client::handleEvent(const Event& event) {
  lastEvent_ = event;
  switch (event.type) {
    case EventType::SessionStarted:
      snprintf(sessionId_, sizeof(sessionId_), "%s", event.sessionId);
      if (state_ == State::Starting) {
        state_ = State::Active;
        audioStartPending_ = true;
      }
      LOG_INF("VRC", "session.started id=%s", sessionId_);
      break;
    case EventType::AsrPartial:
      if (localLatency_.asrFirstPartial < 0 && startSentAtMs_ != 0) {
        localLatency_.asrFirstPartial = static_cast<int32_t>(elapsed(startSentAtMs_));
        LOG_INF("VRC", "first partial in %ld ms", static_cast<long>(localLatency_.asrFirstPartial));
      }
      break;
    case EventType::AsrFinal:
      if (localLatency_.asrFinal < 0 && startSentAtMs_ != 0) {
        localLatency_.asrFinal = static_cast<int32_t>(elapsed(startSentAtMs_));
      }
      break;
    case EventType::LlmStart:
    case EventType::LlmDelta:
      if (localLatency_.llmFirstToken < 0 && startSentAtMs_ != 0) {
        localLatency_.llmFirstToken = static_cast<int32_t>(elapsed(startSentAtMs_));
        LOG_INF("VRC", "first token in %ld ms", static_cast<long>(localLatency_.llmFirstToken));
      }
      break;
    case EventType::LlmFinal:
      if (localLatency_.llmFinal < 0 && startSentAtMs_ != 0) {
        localLatency_.llmFinal = static_cast<int32_t>(elapsed(startSentAtMs_));
      }
      break;
    case EventType::SessionDone:
      if (localLatency_.visibleTotal < 0 && startSentAtMs_ != 0) {
        localLatency_.visibleTotal = static_cast<int32_t>(elapsed(startSentAtMs_));
        LOG_INF("VRC", "session.done in %ld ms (server: connect=%ld asrFinal=%ld llmFirst=%ld total=%ld)",
                static_cast<long>(localLatency_.visibleTotal), static_cast<long>(event.latency.connect),
                static_cast<long>(event.latency.asrFinal), static_cast<long>(event.latency.llmFirstToken),
                static_cast<long>(event.latency.visibleTotal));
      }
      if (state_ != State::Failed) state_ = State::Done;
      break;
    case EventType::Error:
      LOG_ERR("VRC", "relay error %s retryable=%d", errorCodeName(event.code), event.retryable ? 1 : 0);
      if (state_ != State::Done) {
        state_ = State::Failed;
        transportError_ = TransportError::None;  // the error event is the cause
      }
      break;
    default:
      break;
  }
  if (notifyFn_ != nullptr) notifyFn_(notifyCtx_, event);
}

void Client::fail(TransportError error) {
  if (state_ == State::Failed) return;
  LOG_ERR("VRC", "transport failure %u", static_cast<unsigned>(error));
  state_ = State::Failed;
  transportError_ = error;
}

void Client::sendStart() {
  startPending_ = false;
  if (ws_ == nullptr || !wsOpen_ || state_ != State::Connecting) return;
  if (!ws_->sendTXT(startFrame_, startFrameLen_)) {
    fail(TransportError::SocketLost);
    return;
  }
  startSentAtMs_ = millis();
  state_ = State::Starting;
  LOG_INF("VRC", "session.start sent req=%s", requestId_);
}

void Client::sendAudioStart() {
  audioStartPending_ = false;
  if (ws_ == nullptr || !wsOpen_ || state_ != State::Active) return;
  // startFrame_ is free once session.start is on the wire.
  const size_t n = buildAudioStart(startFrame_, sizeof(startFrame_), requestId_, sessionId_);
  if (n == 0) {
    fail(TransportError::BadConfig);
    return;
  }
  if (!ws_->sendTXT(startFrame_, n)) {
    fail(TransportError::SocketLost);
    return;
  }
  LOG_INF("VRC", "audio.start sent");
}

void Client::releaseSocket() {
  if (ws_ != nullptr) {
    ws_->disconnect();
    ws_.reset();
  }
  wsOpen_ = false;
  startPending_ = false;
  audioStartPending_ = false;
  warm_ = false;
  releasePending_ = false;
}

void Client::frameSink(const uint8_t* frame, size_t len, void* ctx) {
  static_cast<Client*>(ctx)->ws_->sendBIN(frame, len);
}

}  // namespace VoiceRelay
