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
// One review queue page is ~150 B per item at LEARNING_REVIEW_LIMIT items.
// 4 KB leaves room for long addedAt/dueAt timestamps on every item.
constexpr size_t BODY_CAP = 4096;
constexpr size_t URL_CAP = 192;
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

namespace {

// Blocking round trip shared by every learning endpoint. Returns Ok when a
// complete HTTP response was read — `httpError` then says whether the status
// was outside 2xx and `body` holds the error prefix for the caller's parser.
// Transport failures map straight to a Result.
LearningClient::Result exchange(const char* url, const char* token, const char* deviceId, const char* path,
                                const char* requestBody, size_t requestLen, char* body, size_t bodyCap,
                                size_t& bodyLen, int& status, bool& httpError) {
  bodyLen = 0;
  status = 0;
  httpError = false;
  if (url == nullptr || token == nullptr || deviceId == nullptr || path == nullptr || body == nullptr || bodyCap == 0) {
    return LearningClient::Result::BadConfig;
  }
  if (!WiFi.isConnected()) return LearningClient::Result::NoWifi;
  const ParsedUrl parsedUrl = parseRelayUrl(url);
  if (!parsedUrl.valid || token[0] == '\0' || deviceId[0] == '\0' || strlen(token) >= AUTH_CAP - 8) {
    return LearningClient::Result::BadConfig;
  }

  char httpUrl[URL_CAP];
  const int urlLen = snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u%s", parsedUrl.tls ? "https" : "http",
                              parsedUrl.host, static_cast<unsigned>(parsedUrl.port), path);
  if (urlLen <= 0 || static_cast<size_t>(urlLen) >= sizeof(httpUrl)) return LearningClient::Result::BadConfig;

  NetBootstrap::ensureTlsAllocator();
  // Only a TLS endpoint cares about the clock (same rule as ChatClient).
  if (parsedUrl.tls && !NetBootstrap::ensureSystemTime()) return LearningClient::Result::Time;

  esp_http_client_config_t config = {};
  config.url = httpUrl;
  config.method = requestBody != nullptr ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("VLEARN", "client init failed");
    return LearningClient::Result::Connect;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  char auth[AUTH_CAP];
  snprintf(auth, sizeof(auth), "Bearer %s", token);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "X-Device-Id", deviceId);

  LOG_INF("VLEARN", "%s %s (%u body bytes)", requestBody != nullptr ? "POST" : "GET", httpUrl,
          static_cast<unsigned>(requestLen));
  if (esp_http_client_open(client, static_cast<int64_t>(requestLen)) != ESP_OK) {
    LOG_ERR("VLEARN", "open failed");
    esp_http_client_cleanup(client);
    return LearningClient::Result::Connect;
  }
  if (requestBody != nullptr && !writeAll(client, requestBody, requestLen)) {
    LOG_ERR("VLEARN", "body write failed");
    esp_http_client_cleanup(client);
    return LearningClient::Result::Http;
  }

  const int64_t contentLength = esp_http_client_fetch_headers(client);
  status = esp_http_client_get_status_code(client);
  httpError = status < 200 || status >= 300;
  // Error bodies only need their prefix (log + error token); a success body
  // is the whole payload and must fit.
  const size_t cap = httpError ? (ERROR_BODY_CAP < bodyCap - 1 ? ERROR_BODY_CAP : bodyCap - 1) : bodyCap - 1;
  size_t total = 0;
  while (total < cap) {
    const int got = esp_http_client_read(client, body + total, static_cast<int>(cap - total));
    if (got < 0) {
      // A broken error body still leaves the status class usable; a truncated
      // success body does not.
      if (httpError) break;
      LOG_ERR("VLEARN", "read failed after %u bytes", static_cast<unsigned>(total));
      esp_http_client_cleanup(client);
      return LearningClient::Result::Read;
    }
    if (got == 0) break;  // end of body
    total += static_cast<size_t>(got);
    esp_task_wdt_reset();
  }
  esp_http_client_cleanup(client);
  body[total] = '\0';
  bodyLen = total;
  if (httpError) {
    LOG_ERR("VLEARN", "HTTP %d (%lld bytes): %s", status, static_cast<long long>(contentLength),
            total > 0 ? body : "");
  }
  return LearningClient::Result::Ok;
}

LearningClient::Result mapHttpStatus(const int status, const ErrorCode code) {
  if (status == 401 || code == ErrorCode::Unauthorized || code == ErrorCode::DeviceNotAllowed) {
    return LearningClient::Result::Auth;
  }
  // 400 is a caller bug (bad batch / bad review); 404 means the relay predates
  // the learning routes.
  if (status == 400 || status == 404) return LearningClient::Result::BadConfig;
  if (code != ErrorCode::None) return mapError(code);
  return status >= 500 ? LearningClient::Result::Upstream : LearningClient::Result::Http;
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

  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, "/api/v1/learning/words", req.get(), reqLen, body.get(),
                                    BODY_CAP, bodyLen, status, httpError);
  if (stats != nullptr) stats->httpStatus = status;
  if (exchanged != Result::Ok) return exchanged;
  if (stats != nullptr) stats->responseBytes = bodyLen;

  if (httpError) {
    AddWordsResult errorBody;
    parseAddWordsResponse(body.get(), bodyLen, errorBody);
    return mapHttpStatus(status, errorBody.error);
  }

  AddWordsResult result;
  if (!parseAddWordsResponse(body.get(), bodyLen, result)) {
    LOG_ERR("VLEARN", "no ok in a %u-byte body (code=%s)", static_cast<unsigned>(bodyLen),
            errorCodeName(result.error));
    if (result.error != ErrorCode::None) return mapError(result.error);
    return Result::NoData;
  }
  if (added != nullptr) *added = result.added;
  if (accepted != nullptr) *accepted = result.accepted;
  LOG_INF("VLEARN", "synced %u words (accepted %ld, added %ld)", static_cast<unsigned>(count),
          static_cast<long>(result.accepted), static_cast<long>(result.added));
  return Result::Ok;
}

LearningClient::Result LearningClient::fetchReviewQueue(const char* url, const char* token, const char* deviceId,
                                                        ReviewItem* items, size_t cap, size_t& stored, int32_t& due,
                                                        Stats* stats) {
  stored = 0;
  due = -1;
  if (stats != nullptr) *stats = Stats{};
  if (items == nullptr || cap == 0) return Result::BadConfig;

  char path[48];
  const int pathLen = snprintf(path, sizeof(path), "/api/v1/learning/review?limit=%u",
                               static_cast<unsigned>(LEARNING_REVIEW_LIMIT));
  if (pathLen <= 0 || static_cast<size_t>(pathLen) >= sizeof(path)) return Result::BadConfig;

  Scratch body = allocScratch(BODY_CAP);
  if (!body) {
    LOG_ERR("VLEARN", "OOM: %u bytes", static_cast<unsigned>(BODY_CAP));
    return Result::Oom;
  }

  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged =
      exchange(url, token, deviceId, path, nullptr, 0, body.get(), BODY_CAP, bodyLen, status, httpError);
  if (stats != nullptr) stats->httpStatus = status;
  if (exchanged != Result::Ok) return exchanged;
  if (stats != nullptr) stats->responseBytes = bodyLen;

  if (httpError) {
    ReviewSubmitResult errorBody;  // same {"error":...} token shape
    parseReviewResponse(body.get(), bodyLen, errorBody);
    return mapHttpStatus(status, errorBody.error);
  }

  ReviewQueueResult result;
  if (!parseReviewQueueResponse(body.get(), bodyLen, items, cap, stored, result)) {
    LOG_ERR("VLEARN", "no words in a %u-byte queue body", static_cast<unsigned>(bodyLen));
    return Result::NoData;
  }
  due = result.due;
  LOG_INF("VLEARN", "review queue: %u shown, %ld due", static_cast<unsigned>(stored), static_cast<long>(due));
  return Result::Ok;
}

LearningClient::Result LearningClient::submitReview(const char* url, const char* token, const char* deviceId,
                                                    const char* word, int rating, const char* clientEventId,
                                                    bool* duplicate, Stats* stats) {
  if (stats != nullptr) *stats = Stats{};
  if (duplicate != nullptr) *duplicate = false;
  if (word == nullptr || clientEventId == nullptr) return Result::BadConfig;

  Scratch req = allocScratch(REQ_CAP);
  Scratch body = allocScratch(BODY_CAP);
  if (!req || !body) {
    LOG_ERR("VLEARN", "OOM: %u + %u bytes", static_cast<unsigned>(REQ_CAP), static_cast<unsigned>(BODY_CAP));
    return Result::Oom;
  }

  const size_t reqLen = buildReviewRequest(req.get(), REQ_CAP, word, rating, clientEventId);
  if (reqLen == 0) {
    LOG_ERR("VLEARN", "review request build failed (%s, rating %d)", word, rating);
    return Result::BadConfig;
  }

  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, "/api/v1/learning/review", req.get(), reqLen, body.get(),
                                    BODY_CAP, bodyLen, status, httpError);
  if (stats != nullptr) stats->httpStatus = status;
  if (exchanged != Result::Ok) return exchanged;
  if (stats != nullptr) stats->responseBytes = bodyLen;

  if (httpError) {
    ReviewSubmitResult errorBody;
    parseReviewResponse(body.get(), bodyLen, errorBody);
    return mapHttpStatus(status, errorBody.error);
  }

  ReviewSubmitResult result;
  if (!parseReviewResponse(body.get(), bodyLen, result)) {
    LOG_ERR("VLEARN", "no ok in a %u-byte review body", static_cast<unsigned>(bodyLen));
    if (result.error != ErrorCode::None) return mapError(result.error);
    return Result::NoData;
  }
  if (duplicate != nullptr) *duplicate = result.duplicate;
  LOG_INF("VLEARN", "review stored: %s rating %d%s", word, rating, result.duplicate ? " (duplicate)" : "");
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
