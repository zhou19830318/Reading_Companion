#include "LearningClient.h"

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

// One batch is LEARNING_CHUNK_WORDS short entries plus the envelope; 2 KB
// holds the worst case with room for hostile escaping.
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 2048;
// A word-book batch is a local SQLite commit server-side; this only bounds a
// dead link (same value as the chat client keeps for one LLM turn).
constexpr int HTTP_TIMEOUT_MS = 30000;
constexpr size_t REQ_CAP = 2048;
constexpr size_t BODY_CAP = 2048;  // response JSON; error bodies are short too
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

LearningClient::Result mapError(ErrorCode code) {
  switch (code) {
    case ErrorCode::Unauthorized:
    case ErrorCode::DeviceNotAllowed:
      return LearningClient::Result::Auth;
    case ErrorCode::BadProtocol:
      return LearningClient::Result::BadConfig;
    case ErrorCode::ServerOverloaded:
    case ErrorCode::InternalError:
      return LearningClient::Result::Upstream;
    default:
      return LearningClient::Result::Http;
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

LearningClient::Result LearningClient::addWords(const char* url, const char* token, const char* deviceId,
                                                const char* const* words, size_t count, const char* source,
                                                int32_t* added, int32_t* accepted, Stats* stats) {
  if (stats != nullptr) *stats = Stats{};
  if (added != nullptr) *added = -1;
  if (accepted != nullptr) *accepted = -1;
  if (url == nullptr || token == nullptr || deviceId == nullptr || words == nullptr || count == 0 ||
      count > LEARNING_MAX_WORDS_PER_REQUEST) {
    return Result::BadConfig;
  }
  if (!WiFi.isConnected()) return Result::NoWifi;
  const ParsedUrl parsedUrl = parseRelayUrl(url);
  if (!parsedUrl.valid || token[0] == '\0' || deviceId[0] == '\0' || strlen(token) >= AUTH_CAP - 8) {
    return Result::BadConfig;
  }

  char httpUrl[URL_CAP];
  snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u/api/v1/learning/words", parsedUrl.tls ? "https" : "http",
           parsedUrl.host, static_cast<unsigned>(parsedUrl.port));

  Scratch req = allocScratch(REQ_CAP);
  Scratch body = allocScratch(BODY_CAP);
  if (!req || !body) {
    LOG_ERR("VLEARN", "OOM: %u + %u bytes", static_cast<unsigned>(REQ_CAP), static_cast<unsigned>(BODY_CAP));
    return Result::Oom;
  }

  const size_t reqLen = buildAddWordsRequest(req.get(), REQ_CAP, words, count, source);
  if (reqLen == 0) {
    LOG_ERR("VLEARN", "request build failed (%u words)", static_cast<unsigned>(count));
    return Result::BadConfig;
  }

  NetBootstrap::ensureTlsAllocator();
  // Only a TLS endpoint cares about the clock (same rule as ChatClient).
  if (parsedUrl.tls && !NetBootstrap::ensureSystemTime()) return Result::Time;

  esp_http_client_config_t config = {};
  config.url = httpUrl;
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("VLEARN", "client init failed");
    return Result::Connect;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  char auth[AUTH_CAP];
  snprintf(auth, sizeof(auth), "Bearer %s", token);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "X-Device-Id", deviceId);

  LOG_INF("VLEARN", "POST %s (%u words, %u body bytes)", httpUrl, static_cast<unsigned>(count),
          static_cast<unsigned>(reqLen));
  if (esp_http_client_open(client, static_cast<int64_t>(reqLen)) != ESP_OK) {
    LOG_ERR("VLEARN", "open failed");
    esp_http_client_cleanup(client);
    return Result::Connect;
  }
  if (!writeAll(client, req.get(), reqLen)) {
    LOG_ERR("VLEARN", "body write failed");
    esp_http_client_cleanup(client);
    return Result::Http;
  }

  const int64_t contentLength = esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (stats != nullptr) stats->httpStatus = status;

  if (status < 200 || status >= 300) {
    // Drain a short prefix of the error body: enough for the log line and the
    // {"error":"UNAUTHORIZED"} template that picks the class.
    size_t n = 0;
    while (n < ERROR_BODY_CAP) {
      const int got = esp_http_client_read(client, body.get() + n, static_cast<int>(ERROR_BODY_CAP - n));
      if (got <= 0) break;
      n += static_cast<size_t>(got);
    }
    body[n] = '\0';
    esp_http_client_cleanup(client);
    AddWordsResult errorBody;
    parseAddWordsResponse(body.get(), n, errorBody);
    LOG_ERR("VLEARN", "HTTP %d (%lld bytes): %s", status, static_cast<long long>(contentLength),
            n > 0 ? body.get() : "");
    if (status == 401 || errorBody.error == ErrorCode::Unauthorized || errorBody.error == ErrorCode::DeviceNotAllowed) {
      return Result::Auth;
    }
    // 400 invalid_words is a caller bug (batch too large / empty word); 404
    // means the relay predates the learning routes.
    if (status == 400 || status == 404) return Result::BadConfig;
    if (errorBody.error != ErrorCode::None) return mapError(errorBody.error);
    return status >= 500 ? Result::Upstream : Result::Http;
  }

  size_t total = 0;
  while (total < BODY_CAP - 1) {
    const int got = esp_http_client_read(client, body.get() + total, static_cast<int>(BODY_CAP - 1 - total));
    if (got < 0) {
      LOG_ERR("VLEARN", "read failed after %u bytes", static_cast<unsigned>(total));
      esp_http_client_cleanup(client);
      return Result::Read;
    }
    if (got == 0) break;  // end of body
    total += static_cast<size_t>(got);
    esp_task_wdt_reset();
  }
  esp_http_client_cleanup(client);
  body[total] = '\0';
  if (stats != nullptr) stats->responseBytes = total;

  AddWordsResult result;
  if (!parseAddWordsResponse(body.get(), total, result)) {
    LOG_ERR("VLEARN", "no ok in a %u-byte body (code=%s)", static_cast<unsigned>(total), errorCodeName(result.error));
    if (result.error != ErrorCode::None) return mapError(result.error);
    return Result::NoData;
  }
  if (added != nullptr) *added = result.added;
  if (accepted != nullptr) *accepted = result.accepted;
  LOG_INF("VLEARN", "synced %u words (accepted %ld, added %ld)", static_cast<unsigned>(count),
          static_cast<long>(result.accepted), static_cast<long>(result.added));
  return Result::Ok;
}

const char* LearningClient::resultName(Result result) {
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
