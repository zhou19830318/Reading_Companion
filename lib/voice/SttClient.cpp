#include "SttClient.h"

// clang-format off
// HalStorage.h pulls SdFat macros that collide with lwip's ip4_addr.h unless
// seen before esp_http_client (which includes lwip). Pin this order; see
// src/network/OtaUpdater.cpp for the same constraint.
#include <Arduino.h>
#include <CloudConfig.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <NetBootstrap.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_task_wdt.h>
// clang-format on

#include <cstdint>
#include <cstring>
#include <ctime>

// Pre-TLS bootstrap (system clock + mbedTLS allocator) lives in
// lib/NetBootstrap so the OpenClaw wss client installs the very same pair.
namespace {

// Same rationale as HttpDownloader: 4 KB of response headroom for the SSE
// stream, 2 KB for the request line we write ourselves.
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 2048;
constexpr int HTTP_TIMEOUT_MS = 60000;
// The settings-page credential probe runs on the web server task: keep it
// well under the browser's 30 s abort and the 5 s task-WDT window per wait.
constexpr int PROBE_TIMEOUT_MS = 15000;
constexpr size_t READ_CHUNK = 2048;
// Room for a base64 line ("data: " + JSON delta) — never the whole body.
constexpr size_t HEAD_BUF = 512;
constexpr size_t TAIL_BUF = 256;

void trimInPlace(char* s) {
  if (s == nullptr) return;
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t')) {
    s[--len] = '\0';
  }
  char* p = s;
  while (*p == ' ' || *p == '\t') ++p;
  if (p != s) memmove(s, p, strlen(p) + 1);
}

// esp_http_client_write may accept a partial buffer; loop until it is all sent.
bool writeAll(esp_http_client_handle_t client, const char* data, size_t len, SttClient::Stats& stats) {
  size_t sent = 0;
  while (sent < len) {
    const int n = esp_http_client_write(client, data + sent, static_cast<int>(len - sent));
    if (n <= 0) {
      LOG_ERR("STT", "body write failed at %u/%u bytes (%d)", static_cast<unsigned>(sent), static_cast<unsigned>(len),
              n);
      return false;
    }
    sent += static_cast<size_t>(n);
    esp_task_wdt_reset();  // a 216 KB upload must not trip the task WDT
  }
  stats.requestBytes += len;
  return true;
}

}  // namespace

bool SttClient::loadConfig(char* urlBuf, size_t urlCap, char* keyBuf, size_t keyCap, Config& cfg) {
  cfg.url = DEFAULT_URL;
  cfg.apiKey = nullptr;
  cfg.model = SttCodec::DEFAULT_MODEL;

  if (urlBuf == nullptr || urlCap == 0 || keyBuf == nullptr || keyCap == 0) return false;
  urlBuf[0] = '\0';
  keyBuf[0] = '\0';

  // NVS (edited from the web settings page) is the primary source; the SD
  // files stay as a fallback so a card prepared before the web UI existed
  // keeps working unchanged.
  CloudConfig& cloud = CLOUD_CFG;
  cloud.load();

  if (cloud.sttUrl[0] != '\0') {
    snprintf(urlBuf, urlCap, "%s", cloud.sttUrl);
    trimInPlace(urlBuf);
    if (urlBuf[0] != '\0') {
      cfg.url = urlBuf;
      LOG_INF("STT", "endpoint override (nvs): %s", urlBuf);
    }
  } else if (Storage.exists(ENDPOINT_FILE)) {
    Storage.readFileToBuffer(ENDPOINT_FILE, urlBuf, urlCap);
    trimInPlace(urlBuf);
    if (urlBuf[0] != '\0') {
      cfg.url = urlBuf;
      LOG_INF("STT", "endpoint override: %s", urlBuf);
    }
  }

  if (cloud.sttKey[0] != '\0') {
    snprintf(keyBuf, keyCap, "%s", cloud.sttKey);
  } else {
    Storage.readFileToBuffer(KEY_FILE, keyBuf, keyCap);
  }
  trimInPlace(keyBuf);
  if (keyBuf[0] == '\0') {
    LOG_ERR("STT", "no API key in NVS and none at %s", KEY_FILE);
    return false;
  }
  cfg.apiKey = keyBuf;
  return true;
}

const char* SttClient::resultName(Result r) {
  switch (r) {
    case OK:
      return "OK";
    case ERR_NO_KEY:
      return "NO_KEY";
    case ERR_NO_PCM:
      return "NO_PCM";
    case ERR_ALLOC:
      return "ALLOC";
    case ERR_TIME:
      return "TIME";
    case ERR_CONNECT:
      return "CONNECT";
    case ERR_WRITE:
      return "WRITE";
    case ERR_HTTP:
      return "HTTP";
    case ERR_READ:
      return "READ";
    case ERR_NO_TEXT:
      return "NO_TEXT";
  }
  return "UNKNOWN";
}

SttClient::Result SttClient::transcribe(const Config& cfg, const int16_t* pcm, size_t samples, uint32_t sampleRate,
                                        SttCodec::SseTranscriptParser& parser, Stats* statsOut) {
  Stats local;
  Stats& st = statsOut != nullptr ? *statsOut : local;
  st = Stats();

  parser.reset();

  if (cfg.url == nullptr || cfg.url[0] == '\0') {
    LOG_ERR("STT", "no endpoint configured");
    return ERR_NO_KEY;
  }
  if (pcm == nullptr || samples == 0) {
    LOG_ERR("STT", "empty capture");
    return ERR_NO_PCM;
  }

  const size_t pcmBytes = samples * sizeof(int16_t);
  const size_t wavBytes = SttCodec::WAV_HEADER_SIZE + pcmBytes;
  const size_t bodyLen = SttCodec::requestBodySize(wavBytes, cfg.model);
  if (bodyLen == 0) {
    LOG_ERR("STT", "cannot size request body for %u WAV bytes", static_cast<unsigned>(wavBytes));
    return ERR_ALLOC;
  }

  SttCodec::WavHeader hdr;
  SttCodec::buildWavHeader(hdr, static_cast<uint32_t>(pcmBytes), sampleRate);

  // PSRAM-backed (28 KB > the always-internal threshold), freed on every path.
  SttCodec::WavBase64Writer writer;
  if (!writer.begin(&hdr, SttCodec::WAV_HEADER_SIZE, reinterpret_cast<const uint8_t*>(pcm), pcmBytes)) {
    LOG_ERR("STT", "base64 writer OOM");
    return ERR_ALLOC;
  }

  NetBootstrap::ensureTlsAllocator();

  LOG_INF("STT", "POST %u WAV bytes -> %u body bytes", static_cast<unsigned>(wavBytes), static_cast<unsigned>(bodyLen));

  // Only a TLS endpoint cares about the clock; a plain-http endpoint file is
  // used precisely so bring-up can proceed without one.
  if (strncmp(cfg.url, "https://", 8) == 0 && !NetBootstrap::ensureSystemTime()) return ERR_TIME;

  esp_http_client_config_t config = {};
  config.url = cfg.url;
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  // Verified HTTPS for the cloud endpoint; plain http for a local endpoint file.
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("STT", "client init failed");
    return ERR_CONNECT;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "text/event-stream");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (cfg.apiKey != nullptr && cfg.apiKey[0] != '\0') {
    esp_http_client_set_header(client, "api-key", cfg.apiKey);
  }

  // Open with the exact declared length, then stream head / base64 / tail.
  const esp_err_t openErr = esp_http_client_open(client, static_cast<int64_t>(bodyLen));
  if (openErr != ESP_OK) {
    LOG_ERR("STT", "open failed: %s", esp_err_to_name(openErr));
    esp_http_client_cleanup(client);
    return ERR_CONNECT;
  }

  char head[HEAD_BUF];
  const size_t headLen = SttCodec::writeBodyHead(head, sizeof(head), cfg.model);
  if (headLen == 0 || !writeAll(client, head, headLen, st)) {
    esp_http_client_cleanup(client);
    return headLen == 0 ? ERR_ALLOC : ERR_WRITE;
  }

  const char* chunk = nullptr;
  size_t chunkLen = 0;
  while (writer.next(&chunk, &chunkLen)) {
    if (!writeAll(client, chunk, chunkLen, st)) {
      esp_http_client_cleanup(client);
      return ERR_WRITE;
    }
  }

  char tail[TAIL_BUF];
  const size_t tailLen = SttCodec::writeBodyTail(tail, sizeof(tail));
  if (tailLen == 0 || !writeAll(client, tail, tailLen, st)) {
    esp_http_client_cleanup(client);
    return tailLen == 0 ? ERR_ALLOC : ERR_WRITE;
  }

  // The declared length must match what we actually sent.
  if (st.requestBytes != bodyLen) {
    LOG_ERR("STT", "body length mismatch: sent %u, declared %u", static_cast<unsigned>(st.requestBytes),
            static_cast<unsigned>(bodyLen));
    esp_http_client_cleanup(client);
    return ERR_WRITE;
  }

  const int64_t contentLength = esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  st.httpStatus = status;
  LOG_INF("STT", "HTTP %d, request %u bytes, response header length %lld", status,
          static_cast<unsigned>(st.requestBytes), static_cast<long long>(contentLength));

  if (status < 200 || status >= 300) {
    char body[160];
    const int n = esp_http_client_read(client, body, static_cast<int>(sizeof(body) - 1));
    if (n > 0) {
      body[n] = '\0';
      LOG_ERR("STT", "response: %s", body);
    }
    esp_http_client_cleanup(client);
    return ERR_HTTP;
  }

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("STT", "OOM: %u byte read buffer", static_cast<unsigned>(READ_CHUNK));
    esp_http_client_cleanup(client);
    return ERR_ALLOC;
  }

  while (true) {
    const int got = esp_http_client_read(client, buf.get(), static_cast<int>(READ_CHUNK));
    if (got < 0) {
      LOG_ERR("STT", "read failed after %u bytes", static_cast<unsigned>(st.responseBytes));
      esp_http_client_cleanup(client);
      return ERR_READ;
    }
    if (got == 0) break;  // end of body
    st.responseBytes += static_cast<size_t>(got);
    if (parser.feed(reinterpret_cast<const uint8_t*>(buf.get()), static_cast<size_t>(got))) break;
    esp_task_wdt_reset();
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  parser.finish();

  LOG_INF("STT", "response %u bytes, %d events, text %u bytes, done=%d complete=%d lineOverflow=%d",
          static_cast<unsigned>(st.responseBytes), parser.eventCount(), static_cast<unsigned>(parser.textSize()),
          parser.done() ? 1 : 0, complete ? 1 : 0, parser.lineOverflow() ? 1 : 0);

  if (parser.textSize() == 0) {
    LOG_ERR("STT", "no transcript in a %d response", status);
    return ERR_NO_TEXT;
  }
  return OK;
}

SttClient::Result SttClient::probeKey(const Config& cfg, KeyProbe& probe) {
  probe = KeyProbe();

  if (cfg.url == nullptr || cfg.url[0] == '\0') {
    LOG_ERR("STT", "probe: no endpoint configured");
    return ERR_NO_KEY;
  }
  if (cfg.apiKey == nullptr || cfg.apiKey[0] == '\0') {
    LOG_ERR("STT", "probe: no API key configured");
    return ERR_NO_KEY;
  }

  // Smallest body the chat-shaped endpoint accepts. Nothing is transcribed —
  // the point is which status comes back, and auth runs before the body is
  // examined, so 401/403 mean "key refused" and any other 4xx means the key
  // was accepted and only the body was rejected.
  char body[192];
  const int bodyLen =
      snprintf(body, sizeof(body), "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}]}",
               cfg.model != nullptr ? cfg.model : "");
  if (bodyLen <= 0 || bodyLen >= static_cast<int>(sizeof(body))) {
    LOG_ERR("STT", "probe: request body did not fit");
    return ERR_ALLOC;
  }

  NetBootstrap::ensureTlsAllocator();
  if (strncmp(cfg.url, "https://", 8) == 0 && !NetBootstrap::ensureSystemTime()) return ERR_TIME;

  esp_http_client_config_t config = {};
  config.url = cfg.url;
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = PROBE_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("STT", "probe: client init failed");
    return ERR_CONNECT;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  esp_http_client_set_header(client, "api-key", cfg.apiKey);

  const esp_err_t openErr = esp_http_client_open(client, bodyLen);
  if (openErr != ESP_OK) {
    LOG_ERR("STT", "probe: open failed: %s", esp_err_to_name(openErr));
    esp_http_client_cleanup(client);
    return ERR_CONNECT;
  }

  Stats st;
  if (!writeAll(client, body, static_cast<size_t>(bodyLen), st)) {
    esp_http_client_cleanup(client);
    return ERR_WRITE;
  }

  (void)esp_http_client_fetch_headers(client);
  probe.httpStatus = esp_http_client_get_status_code(client);
  const int n = esp_http_client_read(client, probe.detail, static_cast<int>(sizeof(probe.detail) - 1));
  if (n > 0) {
    probe.detail[n] = '\0';
  }
  esp_http_client_cleanup(client);

  LOG_INF("STT", "probe: HTTP %d, %u bytes sent, body: %s", probe.httpStatus, static_cast<unsigned>(st.requestBytes),
          probe.detail);
  return (probe.httpStatus >= 200 && probe.httpStatus < 300) ? OK : ERR_HTTP;
}
