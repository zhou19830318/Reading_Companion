#include "WeReadClient.h"

// clang-format off
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <Memory.h>
#include <NetBootstrap.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_task_wdt.h>
// clang-format on

#include <cctype>
#include <cstdio>
#include <cstring>

#include "VoiceRelayCodec.h"

namespace WeRead {

namespace {

constexpr int HTTP_RX_BUF = 1024;
constexpr int HTTP_TX_BUF = 256;
constexpr int HTTP_TIMEOUT_MS = 8000;
constexpr size_t BODY_CAP = 8192;
constexpr size_t URL_CAP = 256;
constexpr size_t AUTH_CAP = 192;
constexpr size_t PATH_CAP = 128;
constexpr size_t ERROR_BODY_CAP = 159;

bool validBookId(const char* bookId) {
  if (bookId == nullptr || bookId[0] == '\0') return false;
  size_t len = 0;
  for (const char* p = bookId; *p != '\0'; ++p) {
    if (++len >= BOOK_ID_CAP) return false;
    if (!std::isalnum(static_cast<unsigned char>(*p)) && *p != '_' && *p != '-') return false;
  }
  return true;
}

WeReadClient::Result mapHttp(int status, const char* body) {
  if (status == 401 || status == 403) return WeReadClient::Result::Auth;
  if (status == 409) return WeReadClient::Result::NotConfigured;
  if (status == 429) return WeReadClient::Result::RateLimited;
  if (status == 400 || status == 404) return WeReadClient::Result::BadConfig;
  if (status >= 500) {
    if (body != nullptr && strstr(body, "weread_auth") != nullptr) return WeReadClient::Result::KeyInvalid;
    if (body != nullptr && strstr(body, "not_configured") != nullptr) return WeReadClient::Result::NotConfigured;
    if (body != nullptr && strstr(body, "rate_limited") != nullptr) return WeReadClient::Result::RateLimited;
    return WeReadClient::Result::Upstream;
  }
  return WeReadClient::Result::Http;
}

WeReadClient::Result exchange(const char* url, const char* token, const char* deviceId, const char* path, char* body,
                              size_t bodyCap, size_t& bodyLen, int& status, bool& httpError) {
  bodyLen = 0;
  status = 0;
  httpError = false;
  if (url == nullptr || token == nullptr || deviceId == nullptr || path == nullptr || body == nullptr || bodyCap == 0) {
    return WeReadClient::Result::BadConfig;
  }
  if (!WiFi.isConnected()) return WeReadClient::Result::NoWifi;

  const VoiceRelay::ParsedUrl parsedUrl = VoiceRelay::parseRelayUrl(url);
  if (!parsedUrl.valid || token[0] == '\0' || deviceId[0] == '\0' || strlen(token) >= AUTH_CAP - 8) {
    return WeReadClient::Result::BadConfig;
  }

  char httpUrl[URL_CAP];
  const int urlLen = snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u%s", parsedUrl.tls ? "https" : "http",
                              parsedUrl.host, static_cast<unsigned>(parsedUrl.port), path);
  if (urlLen <= 0 || static_cast<size_t>(urlLen) >= sizeof(httpUrl)) return WeReadClient::Result::BadConfig;

  NetBootstrap::ensureTlsAllocator();
  if (parsedUrl.tls && !NetBootstrap::ensureSystemTime()) return WeReadClient::Result::Time;

  esp_http_client_config_t config = {};
  config.url = httpUrl;
  config.method = HTTP_METHOD_GET;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("WEREAD", "client init failed");
    return WeReadClient::Result::Connect;
  }

  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  char auth[AUTH_CAP];
  snprintf(auth, sizeof(auth), "Bearer %s", token);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "X-Device-Id", deviceId);

  LOG_INF("WEREAD", "GET %s", httpUrl);
  if (esp_http_client_open(client, 0) != ESP_OK) {
    LOG_ERR("WEREAD", "open failed");
    esp_http_client_cleanup(client);
    return WeReadClient::Result::Connect;
  }

  esp_http_client_fetch_headers(client);
  status = esp_http_client_get_status_code(client);
  httpError = status < 200 || status >= 300;
  const size_t cap = httpError ? (ERROR_BODY_CAP < bodyCap - 1 ? ERROR_BODY_CAP : bodyCap - 1) : bodyCap - 1;
  size_t total = 0;
  while (total < cap) {
    const int got = esp_http_client_read(client, body + total, static_cast<int>(cap - total));
    if (got < 0) {
      if (httpError) break;
      LOG_ERR("WEREAD", "read failed after %u bytes", static_cast<unsigned>(total));
      esp_http_client_cleanup(client);
      return WeReadClient::Result::Read;
    }
    if (got == 0) break;
    total += static_cast<size_t>(got);
    esp_task_wdt_reset();
  }
  esp_http_client_cleanup(client);
  body[total] = '\0';
  bodyLen = total;
  if (httpError) {
    LOG_ERR("WEREAD", "HTTP %d: %s", status, total > 0 ? body : "");
  }
  return WeReadClient::Result::Ok;
}

}  // namespace

WeReadClient::Result WeReadClient::fetchShelf(const char* url, const char* token, const char* deviceId, Book* out,
                                              size_t cap, size_t& count, size_t& total) {
  count = 0;
  total = 0;
  if (out == nullptr || cap == 0) return Result::BadConfig;

  const size_t requestCap = cap < MAX_BOOKS ? cap : MAX_BOOKS;
  char path[PATH_CAP];
  const int pathLen =
      snprintf(path, sizeof(path), "/api/v1/weread/shelf?limit=%u&offset=0", static_cast<unsigned>(requestCap));
  if (pathLen <= 0 || static_cast<size_t>(pathLen) >= sizeof(path)) return Result::BadConfig;

  auto body = makeUniqueNoThrow<char[]>(BODY_CAP);
  if (!body) {
    LOG_ERR("WEREAD", "OOM: shelf buffer");
    return Result::Oom;
  }
  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, path, body.get(), BODY_CAP, bodyLen, status, httpError);
  if (exchanged != Result::Ok) return exchanged;
  if (httpError) return mapHttp(status, body.get());

  JsonDocument doc;
  if (deserializeJson(doc, body.get(), bodyLen) != DeserializationError::Ok) return Result::NoData;
  if (!(doc["ok"] | false)) return Result::NoData;
  total = static_cast<size_t>(doc["total"] | 0u);

  JsonArrayConst books = doc["books"];
  for (JsonObjectConst item : books) {
    if (count >= requestCap) break;
    Book& book = out[count];
    snprintf(book.bookId, sizeof(book.bookId), "%s", item["bookId"] | "");
    snprintf(book.title, sizeof(book.title), "%s", item["title"] | "");
    snprintf(book.author, sizeof(book.author), "%s", item["author"] | "");
    book.finished = item["finished"] | false;
    book.lastRead = item["lastRead"] | 0u;
    if (book.bookId[0] != '\0') ++count;
  }
  return Result::Ok;
}

WeReadClient::Result WeReadClient::fetchBook(const char* url, const char* token, const char* deviceId,
                                             const char* bookId, BookDetail& out) {
  if (!validBookId(bookId)) return Result::BadConfig;

  char path[PATH_CAP];
  const int pathLen = snprintf(path, sizeof(path), "/api/v1/weread/book?bookId=%s", bookId);
  if (pathLen <= 0 || static_cast<size_t>(pathLen) >= sizeof(path)) return Result::BadConfig;

  auto body = makeUniqueNoThrow<char[]>(BODY_CAP);
  if (!body) {
    LOG_ERR("WEREAD", "OOM: book buffer");
    return Result::Oom;
  }
  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, path, body.get(), BODY_CAP, bodyLen, status, httpError);
  if (exchanged != Result::Ok) return exchanged;
  if (httpError) return mapHttp(status, body.get());

  JsonDocument doc;
  if (deserializeJson(doc, body.get(), bodyLen) != DeserializationError::Ok) return Result::NoData;
  if (!(doc["ok"] | false)) return Result::NoData;

  out = BookDetail{};
  snprintf(out.bookId, sizeof(out.bookId), "%s", doc["bookId"] | "");
  snprintf(out.title, sizeof(out.title), "%s", doc["title"] | "");
  snprintf(out.author, sizeof(out.author), "%s", doc["author"] | "");
  snprintf(out.readerLink, sizeof(out.readerLink), "%s", doc["readerLink"] | "");
  out.finished = doc["finished"] | false;
  out.progress = static_cast<uint8_t>(doc["progress"] | 0u);
  if (out.progress > 100) out.progress = 100;
  out.readingTime = doc["readingTime"] | 0u;
  out.finishTime = doc["finishTime"] | 0u;
  return Result::Ok;
}

WeReadClient::Result WeReadClient::fetchStats(const char* url, const char* token, const char* deviceId, Stats& out) {
  auto body = makeUniqueNoThrow<char[]>(BODY_CAP);
  if (!body) {
    LOG_ERR("WEREAD", "OOM: stats buffer");
    return Result::Oom;
  }
  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, "/api/v1/weread/stats?mode=overall", body.get(), BODY_CAP,
                                    bodyLen, status, httpError);
  if (exchanged != Result::Ok) return exchanged;
  if (httpError) return mapHttp(status, body.get());

  JsonDocument doc;
  if (deserializeJson(doc, body.get(), bodyLen) != DeserializationError::Ok) return Result::NoData;
  if (!(doc["ok"] | false)) return Result::NoData;

  out = Stats{};
  out.totalReadTime = doc["totalReadTime"] | 0u;
  out.readDays = static_cast<uint16_t>(doc["readDays"] | 0u);
  out.dayAverageReadTime = doc["dayAverageReadTime"] | 0u;
  return Result::Ok;
}

WeReadClient::Result WeReadClient::fetchConfig(const char* url, const char* token, const char* deviceId,
                                               RelayConfig& out) {
  auto body = makeUniqueNoThrow<char[]>(BODY_CAP);
  if (!body) {
    LOG_ERR("WEREAD", "OOM: config buffer");
    return Result::Oom;
  }
  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged =
      exchange(url, token, deviceId, "/api/v1/device/config", body.get(), BODY_CAP, bodyLen, status, httpError);
  if (exchanged != Result::Ok) return exchanged;
  if (httpError) return mapHttp(status, body.get());

  JsonDocument doc;
  if (deserializeJson(doc, body.get(), bodyLen) != DeserializationError::Ok) return Result::NoData;
  JsonObjectConst relay = doc["relay"];
  if (relay.isNull()) return Result::NoData;

  out = RelayConfig{};
  snprintf(out.url, sizeof(out.url), "%s", relay["url"] | "");
  snprintf(out.token, sizeof(out.token), "%s", relay["token"] | "");
  snprintf(out.deviceId, sizeof(out.deviceId), "%s", relay["deviceId"] | "");
  return Result::Ok;
}

const char* WeReadClient::resultName(Result result) {
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
    case Result::NotConfigured:
      return "NOT_CONFIGURED";
    case Result::KeyInvalid:
      return "KEY_INVALID";
    case Result::RateLimited:
      return "RATE_LIMITED";
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

}  // namespace WeRead
