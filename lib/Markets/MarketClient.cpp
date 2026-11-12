#include "MarketClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <CloudConfig.h>
#include <Logging.h>
#include <Memory.h>
#include <NetBootstrap.h>
#include <VoiceRelayCodec.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_task_wdt.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Markets {
namespace {
bool safeCode(const char* text) {
  if (!text || !*text || strlen(text) >= CODE_CAP) return false;
  for (; *text; ++text) {
    if (!std::isalnum(static_cast<unsigned char>(*text)) && *text != '.' && *text != '-') return false;
  }
  return true;
}

bool numeric(JsonVariantConst value) { return value.is<double>() && std::isfinite(value.as<double>()); }

bool parseCard(JsonObjectConst obj, Card& card) {
  card = Card{};
  const char* id = obj["id"] | "";
  if (!safeCode(id)) return false;
  snprintf(card.id, sizeof(card.id), "%s", id);
  snprintf(card.name, sizeof(card.name), "%s", obj["name"] | id);
  snprintf(card.currency, sizeof(card.currency), "%s", obj["currency"] | "");
  snprintf(card.unit, sizeof(card.unit), "%s", obj["unit"] | "");
  snprintf(card.source, sizeof(card.source), "%s", obj["source"] | "");
  snprintf(card.error, sizeof(card.error), "%s", obj["error"] | "");
  snprintf(card.timezone, sizeof(card.timezone), "%s", obj["timezone"] | "");
  snprintf(card.timeLabel, sizeof(card.timeLabel), "%s", obj["timeLabel"] | "");
  snprintf(card.startLabel, sizeof(card.startLabel), "%s", obj["startLabel"] | "");
  snprintf(card.endLabel, sizeof(card.endLabel), "%s", obj["endLabel"] | "");
  card.available = obj["available"] | false;
  card.stale = obj["stale"] | false;
  if (!card.available) return true;
  if (!numeric(obj["price"]) || !obj["asOf"].is<uint32_t>() || obj["asOf"].as<uint32_t>() == 0) return false;
  card.price = obj["price"].as<double>();
  card.asOf = obj["asOf"].as<uint32_t>();
  card.hasChange = numeric(obj["change"]) && numeric(obj["changePct"]);
  card.change = card.hasChange ? obj["change"].as<double>() : 0;
  card.changePct = card.hasChange ? obj["changePct"].as<double>() : 0;
  card.hasPrevious = numeric(obj["previousClose"]);
  card.previousClose = card.hasPrevious ? obj["previousClose"].as<double>() : 0;
  // Some upstreams fill a missing day range with 0/0; showing "high 0.00 low
  // 0.00" reads as a real price, so treat a non-positive pair as absent.
  card.high = numeric(obj["high"]) ? obj["high"].as<double>() : 0;
  card.low = numeric(obj["low"]) ? obj["low"].as<double>() : 0;
  card.hasRange = card.high > 0 && card.low > 0;
  JsonArrayConst points = obj["points"];
  if (points.isNull() || points.size() > MAX_POINTS) return false;
  for (JsonArrayConst point : points) {
    if (point.size() != 2 || !point[0].is<uint32_t>() || !numeric(point[1])) return false;
    const uint32_t time = point[0].as<uint32_t>();
    const float price = point[1].as<float>();
    if (!time || !std::isfinite(price) || (card.pointCount && time <= card.points[card.pointCount - 1].time)) {
      return false;
    }
    card.points[card.pointCount++] = {time, price};
  }
  return true;
}
}  // namespace

bool Client::begin() {
  body = makeUniqueNoThrow<char[]>(BODY_CAP);
  if (!body) LOG_ERR("MARKET", "OOM: response buffer");
  return body != nullptr;
}

Client::Result Client::fetch(const char* group, const char* code, const char* period, bool lookup, Snapshot& out) {
  if (!body) return Result::Memory;
  if (!WiFi.isConnected()) return Result::Wifi;
  if ((group && !safeCode(group)) || (!group && !safeCode(code))) return Result::InvalidCode;
  if (!period || (strcmp(period, "1d") && strcmp(period, "1mo") && strcmp(period, "1y"))) return Result::InvalidCode;
  // Keep the larger URL/config objects off the small main-task stack.
  struct Request {
    char url[320] = {};
    char auth[208] = {};
    esp_http_client_config_t config = {};
    VoiceRelay::ParsedUrl parsed;
  };
  auto req = makeUniqueNoThrow<Request>();
  if (!req) {
    LOG_ERR("MARKET", "OOM: request");
    return Result::Memory;
  }
  req->parsed = VoiceRelay::parseRelayUrl(CLOUD_CFG.relayUrl);
  if (!req->parsed.valid || !CLOUD_CFG.relayToken[0] || !CLOUD_CFG.relayDeviceId[0]) return Result::Config;
  const int n = snprintf(req->url, sizeof(req->url), "%s://%s:%u/api/v1/markets/%s?%s=%s&period=%s",
                         req->parsed.tls ? "https" : "http", req->parsed.host, req->parsed.port,
                         group ? "board" : (lookup ? "lookup" : "chart"),
                         group ? "group" : (lookup ? "code" : "instrument"), group ? group : code, period);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(req->url)) return Result::Config;
  NetBootstrap::ensureTlsAllocator();
  if (req->parsed.tls && !NetBootstrap::ensureSystemTime()) return Result::Unavailable;
  req->config.url = req->url;
  req->config.method = HTTP_METHOD_GET;
  req->config.buffer_size = 1024;
  req->config.buffer_size_tx = 512;
  req->config.timeout_ms = 18000;  // Greater than relay's maximum upstream deadline.
  req->config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t http = esp_http_client_init(&req->config);
  if (!http) {
    LOG_ERR("MARKET", "http init failed (heap=%u)", static_cast<unsigned>(ESP.getFreeHeap()));
    return Result::Memory;
  }
  snprintf(req->auth, sizeof(req->auth), "Bearer %s", CLOUD_CFG.relayToken);
  esp_http_client_set_header(http, "Authorization", req->auth);
  esp_http_client_set_header(http, "X-Device-Id", CLOUD_CFG.relayDeviceId);
  esp_http_client_set_header(http, "Accept", "application/json");
  if (esp_http_client_open(http, 0) != ESP_OK) {
    esp_http_client_cleanup(http);
    LOG_ERR("MARKET", "open failed (wifi=%d): %s", static_cast<int>(WiFi.status()), req->url);
    return Result::Unavailable;
  }
  esp_http_client_fetch_headers(http);
  const int status = esp_http_client_get_status_code(http);
  if (status != 200) {
    esp_http_client_cleanup(http);
    LOG_ERR("MARKET", "HTTP %d", status);
    if (status == 401 || status == 403) return Result::Auth;
    return status == 400 ? Result::InvalidCode : Result::Unavailable;
  }
  size_t length = 0;
  const uint32_t start = millis();
  bool complete = false;
  while (length < BODY_CAP - 1 && millis() - start < 20000) {
    const int nread = esp_http_client_read(http, body.get() + length, BODY_CAP - 1 - length);
    if (nread < 0) break;
    length += nread;
    esp_task_wdt_reset();
    if (esp_http_client_is_complete_data_received(http)) {
      complete = true;
      break;
    }
    if (nread == 0) break;
  }
  esp_http_client_cleanup(http);
  if (!complete) {
    LOG_ERR("MARKET", "incomplete body (%u bytes)", static_cast<unsigned>(length));
    return Result::InvalidData;
  }
  body[length] = '\0';
  JsonDocument doc;
  // Mutable input enables zero-copy strings; body stays alive through parsing.
  if (deserializeJson(doc, body.get(), length) != DeserializationError::Ok || !(doc["ok"] | false)) {
    LOG_ERR("MARKET", "invalid payload (%u bytes)", static_cast<unsigned>(length));
    return Result::InvalidData;
  }
  out.count = 0;
  if (group) {
    JsonArrayConst cards = doc["cards"];
    if (cards.isNull() || cards.size() == 0 || cards.size() > MAX_CARDS) return Result::InvalidData;
    for (JsonObjectConst card : cards) {
      if (!parseCard(card, out.cards[out.count])) {
        LOG_ERR("MARKET", "card %u rejected", static_cast<unsigned>(out.count));
        return Result::InvalidData;
      }
      ++out.count;
    }
  } else {
    if (!parseCard(doc["card"], out.cards[0])) {
      LOG_ERR("MARKET", "card rejected");
      return Result::InvalidData;
    }
    out.count = 1;
  }
  return Result::Ok;
}
}  // namespace Markets
