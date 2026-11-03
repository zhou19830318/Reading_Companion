#include "PigeonClient.h"

// clang-format off
#include <Arduino.h>
#include <ArduinoJson.h>
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

namespace Pigeon {

namespace {

constexpr int HTTP_RX_BUF = 1024;
constexpr int HTTP_TX_BUF = 2048;
constexpr int HTTP_TIMEOUT_MS = 8000;
constexpr size_t REQ_CAP = 2048;
// 20 messages with 512-byte texts plus the envelope.
constexpr size_t BODY_CAP = 12288;
constexpr size_t URL_CAP = 256;
constexpr size_t AUTH_CAP = 192;
constexpr size_t ERROR_BODY_CAP = 159;

struct HeapCapsDeleter {
  void operator()(char* p) const { heap_caps_free(p); }
};
using Scratch = std::unique_ptr<char[], HeapCapsDeleter>;

Scratch allocScratch(size_t size) {
  char* p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  if (p == nullptr) p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return Scratch(p);
}

bool writeAll(esp_http_client_handle_t client, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int n = esp_http_client_write(client, data + sent, static_cast<int>(len - sent));
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

PigeonClient::Result mapHttp(int status) {
  if (status == 401 || status == 403) return PigeonClient::Result::Auth;
  if (status == 429) return PigeonClient::Result::RateLimited;
  if (status == 400 || status == 404) return PigeonClient::Result::BadConfig;
  if (status >= 500) return PigeonClient::Result::Upstream;
  return PigeonClient::Result::Http;
}

PigeonClient::Result exchange(const char* url, const char* token, const char* deviceId, esp_http_client_method_t method,
                              const char* path, const char* requestBody, size_t requestLen, char* body, size_t bodyCap,
                              size_t& bodyLen, int& status, bool& httpError) {
  bodyLen = 0;
  status = 0;
  httpError = false;
  if (url == nullptr || token == nullptr || deviceId == nullptr || path == nullptr || body == nullptr || bodyCap == 0) {
    return PigeonClient::Result::BadConfig;
  }
  if (!WiFi.isConnected()) return PigeonClient::Result::NoWifi;
  const VoiceRelay::ParsedUrl parsedUrl = VoiceRelay::parseRelayUrl(url);
  if (!parsedUrl.valid || token[0] == '\0' || deviceId[0] == '\0' || strlen(token) >= AUTH_CAP - 8) {
    return PigeonClient::Result::BadConfig;
  }

  char httpUrl[URL_CAP];
  const int urlLen = snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u%s", parsedUrl.tls ? "https" : "http",
                              parsedUrl.host, static_cast<unsigned>(parsedUrl.port), path);
  if (urlLen <= 0 || static_cast<size_t>(urlLen) >= sizeof(httpUrl)) return PigeonClient::Result::BadConfig;

  NetBootstrap::ensureTlsAllocator();
  if (parsedUrl.tls && !NetBootstrap::ensureSystemTime()) return PigeonClient::Result::Time;

  esp_http_client_config_t config = {};
  config.url = httpUrl;
  config.method = method;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("PIGEON", "client init failed");
    return PigeonClient::Result::Connect;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  char auth[AUTH_CAP];
  snprintf(auth, sizeof(auth), "Bearer %s", token);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "X-Device-Id", deviceId);

  LOG_INF("PIGEON", "%s %s (%u body bytes)", method == HTTP_METHOD_POST ? "POST" : "GET", httpUrl,
          static_cast<unsigned>(requestLen));
  if (esp_http_client_open(client, static_cast<int64_t>(requestLen)) != ESP_OK) {
    LOG_ERR("PIGEON", "open failed");
    esp_http_client_cleanup(client);
    return PigeonClient::Result::Connect;
  }
  if (requestBody != nullptr && !writeAll(client, requestBody, requestLen)) {
    LOG_ERR("PIGEON", "body write failed");
    esp_http_client_cleanup(client);
    return PigeonClient::Result::Http;
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
      LOG_ERR("PIGEON", "read failed after %u bytes", static_cast<unsigned>(total));
      esp_http_client_cleanup(client);
      return PigeonClient::Result::Read;
    }
    if (got == 0) break;
    total += static_cast<size_t>(got);
    esp_task_wdt_reset();
  }
  esp_http_client_cleanup(client);
  body[total] = '\0';
  bodyLen = total;
  if (httpError) {
    LOG_ERR("PIGEON", "HTTP %d (%lld bytes): %s", status, static_cast<long long>(contentLength), total > 0 ? body : "");
  }
  return PigeonClient::Result::Ok;
}

bool parseMessage(JsonVariantConst item, const char* self, Message& out) {
  const char* from = item["from"] | "";
  const char* to = item["to"] | "";
  if (from[0] == '\0' || to[0] == '\0') return false;
  out.id = item["id"] | 0u;
  snprintf(out.peerId, sizeof(out.peerId), "%s", strcmp(from, self) == 0 ? to : from);
  snprintf(out.text, sizeof(out.text), "%s", item["text"] | "");
  snprintf(out.createdAt, sizeof(out.createdAt), "%s", item["createdAt"] | "");
  out.outgoing = strcmp(from, self) == 0;
  const char* readAt = item["readAt"] | "";
  out.read = readAt[0] != '\0';
  return true;
}

}  // namespace

PigeonClient::Result PigeonClient::heartbeat(const char* url, const char* token, const char* deviceId, const char* name,
                                             Device* out, size_t cap, size_t& count, int& unread) {
  count = 0;
  unread = 0;
  if (out == nullptr || cap == 0) return Result::BadConfig;
  Scratch req = allocScratch(REQ_CAP);
  Scratch body = allocScratch(BODY_CAP);
  if (!req || !body) {
    LOG_ERR("PIGEON", "OOM: heartbeat buffers");
    return Result::Oom;
  }

  JsonDocument reqDoc;
  reqDoc["name"] = name != nullptr ? name : "";
  const size_t reqLen = serializeJson(reqDoc, req.get(), REQ_CAP);
  if (reqLen == 0 || reqLen >= REQ_CAP) return Result::BadConfig;

  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, HTTP_METHOD_POST, "/api/v1/pigeon/heartbeat", req.get(),
                                    reqLen, body.get(), BODY_CAP, bodyLen, status, httpError);
  if (exchanged != Result::Ok) return exchanged;
  if (httpError) return mapHttp(status);

  JsonDocument doc;
  if (deserializeJson(doc, body.get(), bodyLen) != DeserializationError::Ok) return Result::NoData;
  if (!(doc["ok"] | false)) return Result::NoData;
  unread = doc["unread"] | 0;
  JsonArrayConst devices = doc["devices"];
  for (JsonObjectConst item : devices) {
    if (count >= cap) break;
    Device& device = out[count];
    snprintf(device.id, sizeof(device.id), "%s", item["id"] | "");
    snprintf(device.name, sizeof(device.name), "%s", item["name"] | "");
    if (device.name[0] == '\0') snprintf(device.name, sizeof(device.name), "%s", device.id);
    device.online = item["online"] | false;
    ++count;
  }
  return Result::Ok;
}

PigeonClient::Result PigeonClient::sendText(const char* url, const char* token, const char* deviceId, const char* to,
                                            const char* text, const char* clientEventId, Message& out) {
  if (to == nullptr || text == nullptr || clientEventId == nullptr) return Result::BadConfig;
  Scratch req = allocScratch(REQ_CAP);
  Scratch body = allocScratch(BODY_CAP);
  if (!req || !body) {
    LOG_ERR("PIGEON", "OOM: send buffers");
    return Result::Oom;
  }

  JsonDocument reqDoc;
  reqDoc["to"] = to;
  reqDoc["text"] = text;
  reqDoc["clientEventId"] = clientEventId;
  const size_t reqLen = serializeJson(reqDoc, req.get(), REQ_CAP);
  if (reqLen == 0 || reqLen >= REQ_CAP) return Result::BadConfig;

  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, HTTP_METHOD_POST, "/api/v1/pigeon/send", req.get(), reqLen,
                                    body.get(), BODY_CAP, bodyLen, status, httpError);
  if (exchanged != Result::Ok) return exchanged;
  if (httpError) return mapHttp(status);

  JsonDocument doc;
  if (deserializeJson(doc, body.get(), bodyLen) != DeserializationError::Ok) return Result::NoData;
  if (!(doc["ok"] | false)) return Result::NoData;
  if (!parseMessage(doc["message"], deviceId, out)) return Result::NoData;
  return Result::Ok;
}

PigeonClient::Result PigeonClient::fetchInbox(const char* url, const char* token, const char* deviceId,
                                              uint32_t afterId, Message* out, size_t cap, size_t& count,
                                              uint32_t& latestId, int& unread) {
  count = 0;
  latestId = afterId;
  unread = 0;
  if (out == nullptr || cap == 0) return Result::BadConfig;
  char path[96];
  const int pathLen = snprintf(path, sizeof(path), "/api/v1/pigeon/inbox?after=%u&limit=%u",
                               static_cast<unsigned>(afterId), static_cast<unsigned>(cap));
  if (pathLen <= 0 || static_cast<size_t>(pathLen) >= sizeof(path)) return Result::BadConfig;

  Scratch body = allocScratch(BODY_CAP);
  if (!body) {
    LOG_ERR("PIGEON", "OOM: inbox buffer");
    return Result::Oom;
  }
  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, HTTP_METHOD_GET, path, nullptr, 0, body.get(), BODY_CAP,
                                    bodyLen, status, httpError);
  if (exchanged != Result::Ok) return exchanged;
  if (httpError) return mapHttp(status);

  JsonDocument doc;
  if (deserializeJson(doc, body.get(), bodyLen) != DeserializationError::Ok) return Result::NoData;
  latestId = doc["latestId"] | afterId;
  unread = doc["unread"] | 0;
  JsonArrayConst messages = doc["messages"];
  for (JsonVariantConst item : messages) {
    if (count >= cap) break;
    if (parseMessage(item, deviceId, out[count])) ++count;
  }
  return Result::Ok;
}

PigeonClient::Result PigeonClient::fetchHistory(const char* url, const char* token, const char* deviceId,
                                                const char* peerId, Message* out, size_t cap, size_t& count) {
  count = 0;
  if (peerId == nullptr || peerId[0] == '\0' || out == nullptr || cap == 0) return Result::BadConfig;
  char path[128];
  const int pathLen =
      snprintf(path, sizeof(path), "/api/v1/pigeon/history?peer=%s&limit=%u", peerId, static_cast<unsigned>(cap));
  if (pathLen <= 0 || static_cast<size_t>(pathLen) >= sizeof(path)) return Result::BadConfig;

  Scratch body = allocScratch(BODY_CAP);
  if (!body) {
    LOG_ERR("PIGEON", "OOM: history buffer");
    return Result::Oom;
  }
  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, HTTP_METHOD_GET, path, nullptr, 0, body.get(), BODY_CAP,
                                    bodyLen, status, httpError);
  if (exchanged != Result::Ok) return exchanged;
  if (httpError) return mapHttp(status);

  JsonDocument doc;
  if (deserializeJson(doc, body.get(), bodyLen) != DeserializationError::Ok) return Result::NoData;
  JsonArrayConst messages = doc["messages"];
  for (JsonVariantConst item : messages) {
    if (count >= cap) break;
    if (parseMessage(item, deviceId, out[count])) ++count;
  }
  return Result::Ok;
}

PigeonClient::Result PigeonClient::ack(const char* url, const char* token, const char* deviceId, const uint32_t* ids,
                                       size_t count) {
  if (ids == nullptr || count == 0) return Result::BadConfig;
  Scratch req = allocScratch(REQ_CAP);
  Scratch body = allocScratch(BODY_CAP);
  if (!req || !body) {
    LOG_ERR("PIGEON", "OOM: ack buffers");
    return Result::Oom;
  }

  JsonDocument reqDoc;
  JsonArray arr = reqDoc["ids"].to<JsonArray>();
  for (size_t i = 0; i < count; ++i) arr.add(ids[i]);
  const size_t reqLen = serializeJson(reqDoc, req.get(), REQ_CAP);
  if (reqLen == 0 || reqLen >= REQ_CAP) return Result::BadConfig;

  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, HTTP_METHOD_POST, "/api/v1/pigeon/ack", req.get(), reqLen,
                                    body.get(), BODY_CAP, bodyLen, status, httpError);
  if (exchanged != Result::Ok) return exchanged;
  if (httpError) return mapHttp(status);

  JsonDocument doc;
  if (deserializeJson(doc, body.get(), bodyLen) != DeserializationError::Ok) return Result::NoData;
  return (doc["ok"] | false) ? Result::Ok : Result::NoData;
}

PigeonClient::Result PigeonClient::clearHistory(const char* url, const char* token, const char* deviceId,
                                                const char* peerId) {
  if (peerId == nullptr || peerId[0] == '\0') return Result::BadConfig;
  char path[128];
  const int pathLen = snprintf(path, sizeof(path), "/api/v1/pigeon/clear?peer=%s", peerId);
  if (pathLen <= 0 || static_cast<size_t>(pathLen) >= sizeof(path)) return Result::BadConfig;

  Scratch body = allocScratch(BODY_CAP);
  if (!body) {
    LOG_ERR("PIGEON", "OOM: clear buffer");
    return Result::Oom;
  }
  size_t bodyLen = 0;
  int status = 0;
  bool httpError = false;
  const Result exchanged = exchange(url, token, deviceId, HTTP_METHOD_POST, path, nullptr, 0, body.get(), BODY_CAP,
                                    bodyLen, status, httpError);
  if (exchanged != Result::Ok) return exchanged;
  if (httpError) return mapHttp(status);

  JsonDocument doc;
  if (deserializeJson(doc, body.get(), bodyLen) != DeserializationError::Ok) return Result::NoData;
  return (doc["ok"] | false) ? Result::Ok : Result::NoData;
}

const char* PigeonClient::resultName(Result result) {
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

}  // namespace Pigeon
