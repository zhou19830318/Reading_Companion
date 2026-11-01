#include "PomodoroClient.h"

// clang-format off
#include <Arduino.h>
#include <Logging.h>
#include <NetBootstrap.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_task_wdt.h>
// clang-format on

#include <cstdio>
#include <cstring>
#include <memory>

#include "VoiceRelayCodec.h"

namespace VoiceRelay {

namespace {

constexpr int HTTP_RX_BUF = 1024;
constexpr int HTTP_TX_BUF = 2048;
// A session batch is one SQLite commit server-side; this only bounds a dead
// link. Shorter than the learning batch because the alert screen is already
// visible and the user may want to continue reading.
constexpr int HTTP_TIMEOUT_MS = 15000;
// 16 records at ~160 B plus the envelope.
constexpr size_t REQ_CAP = 4096;
// Success body is {"ok":true,"accepted":N,"stored":N}; 1 KB leaves room for
// any error prefix worth logging.
constexpr size_t BODY_CAP = 1024;
constexpr size_t URL_CAP = 192;
constexpr size_t AUTH_CAP = 192;
constexpr size_t ERROR_BODY_CAP = 159;

bool writeAll(esp_http_client_handle_t client, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int n = esp_http_client_write(client, data + sent, static_cast<int>(len - sent));
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

PomodoroClient::Result mapError(const ErrorCode code) {
  switch (code) {
    case ErrorCode::Unauthorized:
    case ErrorCode::DeviceNotAllowed:
      return PomodoroClient::Result::Auth;
    case ErrorCode::BadProtocol:
      return PomodoroClient::Result::BadConfig;
    case ErrorCode::ServerOverloaded:
    case ErrorCode::InternalError:
      return PomodoroClient::Result::Upstream;
    default:
      return PomodoroClient::Result::Http;
  }
}

// heap_caps_free is not delete[] — the scratch buffers need the matching
// deleter so a PSRAM pointer is never released through the wrong allocator.
struct HeapCapsDeleter {
  void operator()(char* p) const { heap_caps_free(p); }
};
using Scratch = std::unique_ptr<char[], HeapCapsDeleter>;

// PSRAM first: the buffers sit across the TLS handshake, which dips the
// internal heap hard.
Scratch allocScratch(const size_t size) {
  char* p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  if (p == nullptr) p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return Scratch(p);
}

PomodoroClient::Result mapHttpStatus(const int status, const ErrorCode code) {
  if (status == 401 || code == ErrorCode::Unauthorized || code == ErrorCode::DeviceNotAllowed) {
    return PomodoroClient::Result::Auth;
  }
  // 400 is a caller bug (bad batch); 404 means the relay predates the
  // pomodoro routes.
  if (status == 400 || status == 404) return PomodoroClient::Result::BadConfig;
  if (code != ErrorCode::None) return mapError(code);
  return status >= 500 ? PomodoroClient::Result::Upstream : PomodoroClient::Result::Http;
}

// Blocking POST shared by the one endpoint. Returns Ok when a complete HTTP
// response was read — `httpError` then says whether the status was outside
// 2xx and `body` holds the error prefix for the caller's parser.
PomodoroClient::Result exchange(const char* url, const char* token, const char* deviceId, const char* path,
                                const char* requestBody, const size_t requestLen, char* body, const size_t bodyCap,
                                size_t& bodyLen, int& status, bool& httpError) {
  bodyLen = 0;
  status = 0;
  httpError = false;
  if (url == nullptr || token == nullptr || deviceId == nullptr || path == nullptr || body == nullptr || bodyCap == 0) {
    return PomodoroClient::Result::BadConfig;
  }
  if (!WiFi.isConnected()) return PomodoroClient::Result::NoWifi;
  const ParsedUrl parsedUrl = parseRelayUrl(url);
  if (!parsedUrl.valid || token[0] == '\0' || deviceId[0] == '\0' || strlen(token) >= AUTH_CAP - 8) {
    return PomodoroClient::Result::BadConfig;
  }

  char httpUrl[URL_CAP];
  const int urlLen = snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u%s", parsedUrl.tls ? "https" : "http",
                              parsedUrl.host, static_cast<unsigned>(parsedUrl.port), path);
  if (urlLen <= 0 || static_cast<size_t>(urlLen) >= sizeof(httpUrl)) return PomodoroClient::Result::BadConfig;

  NetBootstrap::ensureTlsAllocator();
  // Only a TLS endpoint cares about the clock (same rule as ChatClient).
  if (parsedUrl.tls && !NetBootstrap::ensureSystemTime()) return PomodoroClient::Result::Time;

  esp_http_client_config_t config = {};
  config.url = httpUrl;
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("VPOMO", "client init failed");
    return PomodoroClient::Result::Connect;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  char auth[AUTH_CAP];
  snprintf(auth, sizeof(auth), "Bearer %s", token);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "X-Device-Id", deviceId);

  LOG_INF("VPOMO", "POST %s (%u body bytes)", httpUrl, static_cast<unsigned>(requestLen));
  if (esp_http_client_open(client, static_cast<int64_t>(requestLen)) != ESP_OK) {
    LOG_ERR("VPOMO", "open failed");
    esp_http_client_cleanup(client);
    return PomodoroClient::Result::Connect;
  }
  if (!writeAll(client, requestBody, requestLen)) {
    LOG_ERR("VPOMO", "body write failed");
    esp_http_client_cleanup(client);
    return PomodoroClient::Result::Http;
  }

  const int64_t contentLength = esp_http_client_fetch_headers(client);
  status = esp_http_client_get_status_code(client);
  httpError = status < 200 || status >= 300;
  const size_t cap = httpError ? (ERROR_BODY_CAP < bodyCap - 1 ? ERROR_BODY_CAP : bodyCap - 1) : bodyCap - 1;
  size_t total = 0;
  while (total < cap) {
    const int got = esp_http_client_read(client, body + total, static_cast<int>(cap - total));
    if (got < 0) {
      if (httpError) break;
      LOG_ERR("VPOMO", "read failed after %u bytes", static_cast<unsigned>(total));
      esp_http_client_cleanup(client);
      return PomodoroClient::Result::Read;
    }
    if (got == 0) break;  // end of body
    total += static_cast<size_t>(got);
    esp_task_wdt_reset();
  }
  esp_http_client_cleanup(client);
  body[total] = '\0';
  bodyLen = total;
  if (httpError) {
    LOG_ERR("VPOMO", "HTTP %d (%lld bytes): %s", status, static_cast<long long>(contentLength), total > 0 ? body : "");
  }
  return PomodoroClient::Result::Ok;
}

}  // namespace

PomodoroClient::Result PomodoroClient::uploadSessions(const char* url, const char* token, const char* deviceId,
                                                      const PomodoroSessionWire* sessions, const size_t count,
                                                      Stats* stats) {
  if (stats != nullptr) *stats = Stats{};
  if (url == nullptr || token == nullptr || deviceId == nullptr || sessions == nullptr || count == 0 ||
      count > POMODORO_MAX_SESSION_BATCH) {
    return Result::BadConfig;
  }

  Scratch req = allocScratch(REQ_CAP);
  Scratch body = allocScratch(BODY_CAP);
  if (!req || !body) {
    LOG_ERR("VPOMO", "OOM: %u + %u bytes", static_cast<unsigned>(REQ_CAP), static_cast<unsigned>(BODY_CAP));
    return Result::Oom;
  }

  const size_t reqLen = buildPomodoroSessionsRequest(req.get(), REQ_CAP, sessions, count);
  if (reqLen == 0) {
    LOG_ERR("VPOMO", "request build failed (%u sessions)", static_cast<unsigned>(count));
    return Result::BadConfig;
  }

  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, "/api/v1/pomodoro/sessions", req.get(), reqLen, body.get(),
                                    BODY_CAP, bodyLen, status, httpError);
  if (stats != nullptr) stats->httpStatus = status;
  if (exchanged != Result::Ok) return exchanged;
  if (stats != nullptr) stats->responseBytes = bodyLen;

  if (httpError) {
    PomodoroSyncResult errorBody;
    parsePomodoroSessionsResponse(body.get(), bodyLen, errorBody);
    return mapHttpStatus(status, errorBody.error);
  }

  PomodoroSyncResult result;
  if (!parsePomodoroSessionsResponse(body.get(), bodyLen, result)) {
    LOG_ERR("VPOMO", "no ok in a %u-byte body (code=%s)", static_cast<unsigned>(bodyLen), errorCodeName(result.error));
    if (result.error != ErrorCode::None) return mapError(result.error);
    return Result::NoData;
  }
  LOG_INF("VPOMO", "synced %u sessions (accepted %ld)", static_cast<unsigned>(count),
          static_cast<long>(result.accepted));
  return Result::Ok;
}

const char* PomodoroClient::resultName(const Result result) {
  switch (result) {
    case Result::Ok:
      return "OK";
    case Result::BadConfig:
      return "BAD_CONFIG";
    case Result::NoWifi:
      return "NO_WIFI";
    case Result::Time:
      return "TIME";
    case Result::Connect:
      return "CONNECT";
    case Result::Http:
      return "HTTP";
    case Result::Auth:
      return "AUTH";
    case Result::Upstream:
      return "UPSTREAM";
    case Result::Read:
      return "READ";
    case Result::Oom:
      return "OOM";
    case Result::NoData:
      return "NO_DATA";
  }
  return "UNKNOWN";
}

}  // namespace VoiceRelay
