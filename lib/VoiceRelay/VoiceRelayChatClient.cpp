#include "VoiceRelayChatClient.h"

// clang-format off
#include <Arduino.h>
#include <Logging.h>
#include <NetBootstrap.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
// clang-format on

#include <cstdio>
#include <cstring>
#include <memory>

#include "VoiceRelayCodec.h"

namespace VoiceRelay {

namespace {

// The response is capped at CHAT_TEXT_CAP plus JSON overhead; 4 KB holds the
// worst case and doubles as the header-read headroom esp_http_client asks for.
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 2048;
// One blocking LLM turn; the server's own provider timeout is lower, so this
// only bounds a dead link.
constexpr int HTTP_TIMEOUT_MS = 30000;
constexpr size_t REQ_CAP = 4096;   // escaped prompt + envelope
constexpr size_t BODY_CAP = 4096;  // response JSON
constexpr size_t URL_CAP = 160;
constexpr size_t AUTH_CAP = 192;
// Longest error body prefix drained for the log + the UNAUTHORIZED token.
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

ChatClient::Result mapProviderError(ErrorCode code) {
  switch (code) {
    case ErrorCode::Unauthorized:
    case ErrorCode::DeviceNotAllowed:
    case ErrorCode::AsrAuthError:
    case ErrorCode::LlmAuthError:
      return ChatClient::Result::Auth;
    case ErrorCode::BadProtocol:
    case ErrorCode::AudioFormatUnsupported:
      return ChatClient::Result::BadConfig;
    case ErrorCode::ServerOverloaded:
    case ErrorCode::AsrRateLimited:
    case ErrorCode::LlmRateLimited:
    case ErrorCode::AsrConnectionError:
    case ErrorCode::LlmConnectionError:
    case ErrorCode::AudioTimeout:
    case ErrorCode::SessionTimeout:
    case ErrorCode::InternalError:
      return ChatClient::Result::Upstream;
    default:
      return ChatClient::Result::Http;
  }
}

// heap_caps_free is not delete[] — the scratch buffers need the matching
// deleter so a PSRAM pointer is never released through the wrong allocator.
struct HeapCapsDeleter {
  void operator()(char* p) const { heap_caps_free(p); }
};
using Scratch = std::unique_ptr<char[], HeapCapsDeleter>;

// PSRAM first: both buffers sit across the TLS handshake, which dips the
// internal heap hard (the writer's class rule about large internals).
Scratch allocScratch(size_t size) {
  char* p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  if (p == nullptr) p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return Scratch(p);
}

}  // namespace

ChatClient::Result ChatClient::ask(const char* url, const char* token, const char* deviceId, const char* text,
                                   char* reply, size_t replyCap, bool& truncated, const char* mode, Stats* stats) {
  truncated = false;
  if (stats != nullptr) *stats = Stats{};
  if (url == nullptr || token == nullptr || deviceId == nullptr || text == nullptr || reply == nullptr ||
      replyCap == 0) {
    return Result::BadConfig;
  }
  if (!WiFi.isConnected()) return Result::NoWifi;
  const ParsedUrl parsed = parseRelayUrl(url);
  if (!parsed.valid || token[0] == '\0' || deviceId[0] == '\0' || strlen(token) >= AUTH_CAP - 8) {
    return Result::BadConfig;
  }
  reply[0] = '\0';

  char httpUrl[URL_CAP];
  snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u/api/v1/chat", parsed.tls ? "https" : "http", parsed.host,
           static_cast<unsigned>(parsed.port));

  Scratch req = allocScratch(REQ_CAP);
  Scratch body = allocScratch(BODY_CAP);
  if (!req || !body) {
    LOG_ERR("VCHAT", "OOM: %u + %u bytes", static_cast<unsigned>(REQ_CAP), static_cast<unsigned>(BODY_CAP));
    return Result::Oom;
  }

  char requestId[REQUEST_ID_CAP];
  formatRequestId(requestId, esp_random(), esp_random(), esp_random(), esp_random());
  const size_t reqLen = buildChatRequest(req.get(), REQ_CAP, requestId, deviceId, text, mode);
  if (reqLen == 0) {
    LOG_ERR("VCHAT", "request build failed (%u text bytes)", static_cast<unsigned>(strlen(text)));
    return Result::BadConfig;
  }

  NetBootstrap::ensureTlsAllocator();
  // Only a TLS endpoint cares about the clock (same rule as SttClient).
  if (parsed.tls && !NetBootstrap::ensureSystemTime()) return Result::Time;

  esp_http_client_config_t config = {};
  config.url = httpUrl;
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("VCHAT", "client init failed");
    return Result::Connect;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  char auth[AUTH_CAP];
  snprintf(auth, sizeof(auth), "Bearer %s", token);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "X-Device-Id", deviceId);

  LOG_INF("VCHAT", "POST %s (%u body bytes)", httpUrl, static_cast<unsigned>(reqLen));
  if (esp_http_client_open(client, static_cast<int64_t>(reqLen)) != ESP_OK) {
    LOG_ERR("VCHAT", "open failed");
    esp_http_client_cleanup(client);
    return Result::Connect;
  }
  if (!writeAll(client, req.get(), reqLen)) {
    LOG_ERR("VCHAT", "body write failed");
    esp_http_client_cleanup(client);
    return Result::Http;
  }

  const int64_t contentLength = esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (stats != nullptr) stats->httpStatus = status;

  if (status < 200 || status >= 300) {
    // Drain a short prefix of the error body: enough for the log line and the
    // {"error":"UNAUTHORIZED"} / {"code":"LLM_..."} token that picks the class.
    size_t n = 0;
    while (n < ERROR_BODY_CAP) {
      const int got = esp_http_client_read(client, body.get() + n, static_cast<int>(ERROR_BODY_CAP - n));
      if (got <= 0) break;
      n += static_cast<size_t>(got);
    }
    body[n] = '\0';
    esp_http_client_cleanup(client);
    char scratch[8];
    bool bodyTruncated = false;
    ErrorCode code = ErrorCode::None;
    parseChatResponse(body.get(), n, scratch, sizeof(scratch), bodyTruncated, code);
    LOG_ERR("VCHAT", "HTTP %d (%lld bytes): %s", status, static_cast<long long>(contentLength),
            n > 0 ? body.get() : "");
    if (status == 401 || code == ErrorCode::Unauthorized || code == ErrorCode::DeviceNotAllowed) return Result::Auth;
    if (status == 404) return Result::BadConfig;  // endpoint disabled: no admin/device token on the server
    if (code != ErrorCode::None) return mapProviderError(code);
    return status >= 500 ? Result::Upstream : Result::Http;
  }

  size_t total = 0;
  while (total < BODY_CAP - 1) {
    const int got = esp_http_client_read(client, body.get() + total, static_cast<int>(BODY_CAP - 1 - total));
    if (got < 0) {
      LOG_ERR("VCHAT", "read failed after %u bytes", static_cast<unsigned>(total));
      esp_http_client_cleanup(client);
      return Result::Read;
    }
    if (got == 0) break;  // end of body
    total += static_cast<size_t>(got);
    esp_task_wdt_reset();
  }
  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  body[total] = '\0';
  if (stats != nullptr) stats->responseBytes = total;
  if (!complete && total == BODY_CAP - 1) truncated = true;  // body cut by the read buffer

  bool parseTruncated = false;
  ErrorCode code = ErrorCode::None;
  if (!parseChatResponse(body.get(), total, reply, replyCap, parseTruncated, code)) {
    LOG_ERR("VCHAT", "no text in a %u-byte body (code=%s)", static_cast<unsigned>(total), errorCodeName(code));
    if (code != ErrorCode::None) return mapProviderError(code);
    return Result::NoText;
  }
  truncated = truncated || parseTruncated;
  LOG_INF("VCHAT", "reply %u bytes%s", static_cast<unsigned>(strlen(reply)), truncated ? " (truncated)" : "");
  return Result::Ok;
}

const char* ChatClient::resultName(Result result) {
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
    case Result::NoText:
      return "NO_TEXT";
  }
  return "UNKNOWN";
}

}  // namespace VoiceRelay
