#include "VoiceRelayImageClient.h"

// clang-format off
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <NetBootstrap.h>
#include <SttCodec.h>
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

// Success bodies are a few hundred bytes; the cap just bounds a misbehaving
// server. Doubles as the header-read headroom esp_http_client asks for.
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 1024;
// The server holds the POST open while the provider renders (its T2I_TIMEOUT_S
// bounds the backend, default 150 s), so the wire timeout must sit above that:
// anything shorter turns a slow render into a dead link.
constexpr int GENERATE_TIMEOUT_MS = 180000;
// Asset GET: 48 KB over the LAN; 30 s bounds a dead link, never a healthy one.
constexpr int DOWNLOAD_TIMEOUT_MS = 30000;
constexpr size_t REQ_CAP = 2048;   // escaped prompt + envelope
constexpr size_t BODY_CAP = 2048;  // response JSON
constexpr size_t URL_CAP = 176;    // origin + /api/v1/image/<16 hex>
constexpr size_t AUTH_CAP = 192;
// Longest error body prefix drained for the log + the code token.
constexpr size_t ERROR_BODY_CAP = 159;
// One network read, one SD write. 4 KB keeps the SPI chunks reasonable
// without ever holding the 48 KB asset in RAM.
constexpr size_t STREAM_BUF = 4096;
// Edit mode: the panel BMP (~48 KB) is read whole, base64-encoded (~64 KB)
// and embedded in the request (~65 KB) — all PSRAM. A file larger than this
// is not one of ours (the history only ever holds generated panel BMPs).
constexpr size_t EDIT_FILE_MAX = 256 * 1024;
// JSON around the base64 payload in an edit request (ids, text, keys).
constexpr size_t EDIT_ENVELOPE = 512;

bool writeAll(esp_http_client_handle_t client, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int n = esp_http_client_write(client, data + sent, static_cast<int>(len - sent));
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

ImageClient::Result mapProviderError(ErrorCode code) {
  switch (code) {
    case ErrorCode::Unauthorized:
    case ErrorCode::DeviceNotAllowed:
    case ErrorCode::AsrAuthError:
    case ErrorCode::LlmAuthError:
      return ImageClient::Result::Auth;
    case ErrorCode::BadProtocol:
      return ImageClient::Result::BadConfig;
    case ErrorCode::T2iRejected:
      return ImageClient::Result::Rejected;
    case ErrorCode::T2iNotConfigured:
      return ImageClient::Result::NotConfigured;
    case ErrorCode::T2iTimeout:
      return ImageClient::Result::Timeout;
    case ErrorCode::T2iEmptyPrompt:  // server-side prompt guard; generate() never sends one
    case ErrorCode::T2iBadParam:     // our style/size/edit image was rejected — a caller bug
    case ErrorCode::T2iUpstream:
    case ErrorCode::ServerOverloaded:
    case ErrorCode::InternalError:
      return ImageClient::Result::Upstream;
    default:
      return ImageClient::Result::Http;
  }
}

// heap_caps_free is not delete[] — the scratch buffers need the matching
// deleter so a PSRAM pointer is never released through the wrong allocator.
struct HeapCapsDeleter {
  void operator()(char* p) const { heap_caps_free(p); }
};
using Scratch = std::unique_ptr<char[], HeapCapsDeleter>;

// PSRAM first: the buffers sit across the request, which dips the internal
// heap hard on the TLS path (same rule as ChatClient).
Scratch allocScratch(size_t size) {
  char* p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  if (p == nullptr) p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return Scratch(p);
}

// Applies the headers every relay call shares. `auth` must outlive the call.
void setCommonHeaders(esp_http_client_handle_t client, char* auth, size_t authCap, const char* token,
                      const char* deviceId) {
  snprintf(auth, authCap, "Bearer %s", token);
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "X-Device-Id", deviceId);
}

// Shared POST /api/v1/image round used by generate() and edit(). `req` holds
// the complete JSON body (`reqLen` bytes); `bodyScratch` (>= BODY_CAP) is the
// response scratch. The server holds the connection while the provider
// renders, so this owns the loop for tens of seconds.
ImageClient::Result postImageRound(const ParsedUrl& parsed, const char* httpUrl, const char* token,
                                   const char* deviceId, char* req, size_t reqLen, char* bodyScratch, ImageMeta& meta,
                                   char* phrase, size_t phraseCap, ImageClient::Stats* stats) {
  NetBootstrap::ensureTlsAllocator();
  // Only a TLS endpoint cares about the clock (same rule as ChatClient).
  if (parsed.tls && !NetBootstrap::ensureSystemTime()) return ImageClient::Result::Time;

  esp_http_client_config_t config = {};
  config.url = httpUrl;
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = GENERATE_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("VIMG", "client init failed");
    return ImageClient::Result::Connect;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "application/json");
  char auth[AUTH_CAP];
  setCommonHeaders(client, auth, sizeof(auth), token, deviceId);

  LOG_INF("VIMG", "POST %s (%u body bytes)", httpUrl, static_cast<unsigned>(reqLen));
  if (esp_http_client_open(client, static_cast<int64_t>(reqLen)) != ESP_OK) {
    LOG_ERR("VIMG", "open failed");
    esp_http_client_cleanup(client);
    return ImageClient::Result::Connect;
  }
  if (!writeAll(client, req, reqLen)) {
    LOG_ERR("VIMG", "body write failed");
    esp_http_client_cleanup(client);
    return ImageClient::Result::Http;
  }

  const int64_t contentLength = esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (stats != nullptr) stats->httpStatus = status;

  if (status < 200 || status >= 300) {
    // Drain a short prefix of the error body: enough for the log line and
    // the {"code":"T2I_..."} token that picks the class.
    size_t n = 0;
    while (n < ERROR_BODY_CAP) {
      const int got = esp_http_client_read(client, bodyScratch + n, static_cast<int>(ERROR_BODY_CAP - n));
      if (got <= 0) break;
      n += static_cast<size_t>(got);
    }
    bodyScratch[n] = '\0';
    esp_http_client_cleanup(client);
    ImageMeta scratchMeta;
    ErrorCode code = ErrorCode::None;
    parseImageResponse(bodyScratch, n, scratchMeta, nullptr, 0, code);
    LOG_ERR("VIMG", "HTTP %d (%lld bytes): %s (code=%s)", status, static_cast<long long>(contentLength),
            n > 0 ? bodyScratch : "", errorCodeName(code));
    if (status == 401 || code == ErrorCode::Unauthorized || code == ErrorCode::DeviceNotAllowed) {
      return ImageClient::Result::Auth;
    }
    if (status == 404) return ImageClient::Result::BadConfig;  // endpoint disabled
    if (code != ErrorCode::None) return mapProviderError(code);
    return status >= 500 ? ImageClient::Result::Upstream : ImageClient::Result::Http;
  }

  size_t total = 0;
  while (total < BODY_CAP - 1) {
    const int got = esp_http_client_read(client, bodyScratch + total, static_cast<int>(BODY_CAP - 1 - total));
    if (got < 0) {
      LOG_ERR("VIMG", "read failed after %u bytes", static_cast<unsigned>(total));
      esp_http_client_cleanup(client);
      return ImageClient::Result::Read;
    }
    if (got == 0) break;  // end of body
    total += static_cast<size_t>(got);
    esp_task_wdt_reset();
  }
  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  bodyScratch[total] = '\0';
  if (stats != nullptr) stats->responseBytes = total;
  if (!complete && total == BODY_CAP - 1) return ImageClient::Result::Read;  // body outgrew the read buffer

  ErrorCode code = ErrorCode::None;
  if (!parseImageResponse(bodyScratch, total, meta, phrase, phraseCap, code)) {
    LOG_ERR("VIMG", "no image in a %u-byte body (code=%s)", static_cast<unsigned>(total), errorCodeName(code));
    if (code != ErrorCode::None) return mapProviderError(code);
    return ImageClient::Result::NoImage;
  }
  LOG_INF("VIMG", "image %s %dx%d %d bytes", meta.id, static_cast<int>(meta.width), static_cast<int>(meta.height),
          static_cast<int>(meta.bytes));
  return ImageClient::Result::Ok;
}

}  // namespace

ImageClient::Result ImageClient::generate(const char* url, const char* token, const char* deviceId, const char* text,
                                          ImageMeta& meta, char* phrase, size_t phraseCap, Stats* stats,
                                          const char* style, const char* size) {
  meta = ImageMeta{};
  if (stats != nullptr) *stats = Stats{};
  if (phrase != nullptr && phraseCap > 0) phrase[0] = '\0';
  if (url == nullptr || token == nullptr || deviceId == nullptr || text == nullptr) return Result::BadConfig;
  if (!WiFi.isConnected()) return Result::NoWifi;
  const ParsedUrl parsed = parseRelayUrl(url);
  if (!parsed.valid || token[0] == '\0' || deviceId[0] == '\0' || strlen(token) >= AUTH_CAP - 8) {
    return Result::BadConfig;
  }

  char httpUrl[URL_CAP];
  snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u/api/v1/image", parsed.tls ? "https" : "http", parsed.host,
           static_cast<unsigned>(parsed.port));

  Scratch req = allocScratch(REQ_CAP);
  Scratch body = allocScratch(BODY_CAP);
  if (!req || !body) {
    LOG_ERR("VIMG", "OOM: %u + %u bytes", static_cast<unsigned>(REQ_CAP), static_cast<unsigned>(BODY_CAP));
    return Result::Oom;
  }

  char requestId[REQUEST_ID_CAP];
  formatRequestId(requestId, esp_random(), esp_random(), esp_random(), esp_random());
  const size_t reqLen = buildImageRequest(req.get(), REQ_CAP, requestId, deviceId, text, style, size);
  if (reqLen == 0) {
    LOG_ERR("VIMG", "request build failed (%u text bytes)", static_cast<unsigned>(strlen(text)));
    return Result::BadConfig;
  }

  return postImageRound(parsed, httpUrl, token, deviceId, req.get(), reqLen, body.get(), meta, phrase, phraseCap,
                        stats);
}

ImageClient::Result ImageClient::edit(const char* url, const char* token, const char* deviceId, const char* text,
                                      const char* imagePath, ImageMeta& meta, char* phrase, size_t phraseCap,
                                      Stats* stats) {
  meta = ImageMeta{};
  if (stats != nullptr) *stats = Stats{};
  if (phrase != nullptr && phraseCap > 0) phrase[0] = '\0';
  if (url == nullptr || token == nullptr || deviceId == nullptr || text == nullptr || text[0] == '\0' ||
      imagePath == nullptr || imagePath[0] == '\0') {
    return Result::BadConfig;
  }
  if (!WiFi.isConnected()) return Result::NoWifi;
  const ParsedUrl parsed = parseRelayUrl(url);
  if (!parsed.valid || token[0] == '\0' || deviceId[0] == '\0' || strlen(token) >= AUTH_CAP - 8) {
    return Result::BadConfig;
  }

  // Read the whole source BMP into PSRAM, base64 it, and build the request.
  // Sizes are bounded above, so the three buffers are known before any
  // allocation (raw ~48 KB + b64 ~64 KB + request ~65 KB, all SPIRAM).
  HalFile file;
  if (!Storage.openFileForRead("VIMG", imagePath, file)) {
    LOG_ERR("VIMG", "cannot open %s for edit", imagePath);
    return Result::File;
  }
  const size_t fileBytes = file.size();
  if (fileBytes < 54 || fileBytes > EDIT_FILE_MAX) {  // 54 = minimal BMP header
    LOG_ERR("VIMG", "edit source implausible: %u bytes", static_cast<unsigned>(fileBytes));
    return Result::BadConfig;
  }
  Scratch raw = allocScratch(fileBytes);
  if (!raw) {
    LOG_ERR("VIMG", "OOM: %u bytes (edit source)", static_cast<unsigned>(fileBytes));
    return Result::Oom;
  }
  if (file.read(raw.get(), fileBytes) != static_cast<int>(fileBytes)) {
    LOG_ERR("VIMG", "edit source read failed (%u bytes)", static_cast<unsigned>(fileBytes));
    return Result::File;
  }
  // BMP header: width at offset 18, height at 22 (int32 LE; height may be
  // negative for top-down bitmaps). memcpy — RISC-V faults on unaligned
  // loads through a wider pointer.
  int32_t width = 0;
  int32_t height = 0;
  memcpy(&width, raw.get() + 18, sizeof(width));
  memcpy(&height, raw.get() + 22, sizeof(height));
  if (height < 0) height = -height;
  if (width <= 0 || height <= 0) {
    LOG_ERR("VIMG", "edit source header bad: %dx%d", static_cast<int>(width), static_cast<int>(height));
    return Result::BadConfig;
  }

  const size_t b64Cap = SttCodec::base64EncodedSize(fileBytes) + 1;
  const size_t reqCap = EDIT_ENVELOPE + b64Cap;
  Scratch b64 = allocScratch(b64Cap);
  Scratch req = allocScratch(reqCap);
  Scratch body = allocScratch(BODY_CAP);
  if (!b64 || !req || !body) {
    LOG_ERR("VIMG", "OOM: %u + %u + %u bytes (edit)", static_cast<unsigned>(b64Cap), static_cast<unsigned>(reqCap),
            static_cast<unsigned>(BODY_CAP));
    return Result::Oom;
  }
  size_t b64Len = 0;
  if (!SttCodec::base64Encode(reinterpret_cast<const uint8_t*>(raw.get()), fileBytes, b64.get(), b64Cap, b64Len)) {
    LOG_ERR("VIMG", "base64 encode failed (%u bytes)", static_cast<unsigned>(fileBytes));
    return Result::BadConfig;
  }
  raw.reset();  // the file bytes are not needed on the wire — free before the POST

  char httpUrl[URL_CAP];
  snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u/api/v1/image", parsed.tls ? "https" : "http", parsed.host,
           static_cast<unsigned>(parsed.port));
  char requestId[REQUEST_ID_CAP];
  formatRequestId(requestId, esp_random(), esp_random(), esp_random(), esp_random());
  const size_t reqLen = buildImageEditRequest(req.get(), reqCap, requestId, deviceId, text, b64.get(), width, height);
  if (reqLen == 0) {
    LOG_ERR("VIMG", "edit request build failed (b64 %u bytes)", static_cast<unsigned>(b64Len));
    return Result::BadConfig;
  }
  b64.reset();  // request holds the only copy now

  LOG_INF("VIMG", "edit %s -> %dx%d from %s", requestId, static_cast<int>(width), static_cast<int>(height), imagePath);
  return postImageRound(parsed, httpUrl, token, deviceId, req.get(), reqLen, body.get(), meta, phrase, phraseCap,
                        stats);
}

ImageClient::Result ImageClient::download(const char* url, const char* token, const char* deviceId,
                                          const ImageMeta& meta, const char* path) {
  if (url == nullptr || token == nullptr || deviceId == nullptr || path == nullptr || path[0] == '\0') {
    return Result::BadConfig;
  }
  if (meta.id[0] == '\0') return Result::BadConfig;
  if (!WiFi.isConnected()) return Result::NoWifi;
  const ParsedUrl parsed = parseRelayUrl(url);
  if (!parsed.valid || token[0] == '\0' || deviceId[0] == '\0' || strlen(token) >= AUTH_CAP - 8) {
    return Result::BadConfig;
  }

  char httpUrl[URL_CAP];
  snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u/api/v1/image/%s", parsed.tls ? "https" : "http", parsed.host,
           static_cast<unsigned>(parsed.port), meta.id);

  Scratch buf = allocScratch(STREAM_BUF);
  if (!buf) {
    LOG_ERR("VIMG", "OOM: %u bytes", static_cast<unsigned>(STREAM_BUF));
    return Result::Oom;
  }

  NetBootstrap::ensureTlsAllocator();
  if (parsed.tls && !NetBootstrap::ensureSystemTime()) return Result::Time;

  esp_http_client_config_t config = {};
  config.url = httpUrl;
  config.method = HTTP_METHOD_GET;
  config.buffer_size = HTTP_RX_BUF;
  config.timeout_ms = DOWNLOAD_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("VIMG", "client init failed");
    return Result::Connect;
  }

  esp_http_client_set_header(client, "Accept", "image/bmp");
  char auth[AUTH_CAP];
  setCommonHeaders(client, auth, sizeof(auth), token, deviceId);

  LOG_INF("VIMG", "GET %s", httpUrl);
  if (esp_http_client_open(client, 0) != ESP_OK) {
    LOG_ERR("VIMG", "open failed");
    esp_http_client_cleanup(client);
    return Result::Connect;
  }
  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (status < 200 || status >= 300) {
    char small[ERROR_BODY_CAP + 1];
    size_t n = 0;
    while (n < ERROR_BODY_CAP) {
      const int got = esp_http_client_read(client, small + n, static_cast<int>(ERROR_BODY_CAP - n));
      if (got <= 0) break;
      n += static_cast<size_t>(got);
    }
    small[n] = '\0';
    esp_http_client_cleanup(client);
    LOG_ERR("VIMG", "asset HTTP %d: %s", status, n > 0 ? small : "");
    if (status == 401) return Result::Auth;
    if (status == 404) return Result::Download;  // asset expired or cleaned up
    return status >= 500 ? Result::Upstream : Result::Http;
  }

  HalFile file;
  if (!Storage.openFileForWrite("VIMG", path, file)) {
    LOG_ERR("VIMG", "cannot open %s for write", path);
    esp_http_client_cleanup(client);
    return Result::File;
  }

  ImageClient::Result result = Result::Ok;
  size_t total = 0;
  while (true) {
    const int got = esp_http_client_read(client, buf.get(), static_cast<int>(STREAM_BUF));
    if (got < 0) {
      LOG_ERR("VIMG", "read failed after %u bytes", static_cast<unsigned>(total));
      result = Result::Read;
      break;
    }
    if (got == 0) break;  // end of body
    if (file.write(reinterpret_cast<const uint8_t*>(buf.get()), static_cast<size_t>(got)) != static_cast<size_t>(got)) {
      LOG_ERR("VIMG", "SD write failed at %u bytes", static_cast<unsigned>(total));
      result = Result::File;
      break;
    }
    total += static_cast<size_t>(got);
    esp_task_wdt_reset();
  }
  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (result == Result::Ok && !complete) {
    LOG_ERR("VIMG", "asset truncated at %u bytes", static_cast<unsigned>(total));
    result = Result::Download;
  }

  if (result != Result::Ok) {
    file.close();  // must close before removing the file it still holds
    Storage.remove(path);
    return result;
  }
  if (meta.bytes > 0 && total != static_cast<size_t>(meta.bytes)) {
    // The header lied; the bytes on disk are what the GET returned, and the
    // viewer validates the BMP before drawing. Worth a log, not a failure.
    LOG_ERR("VIMG", "asset size mismatch: meta %d, got %u", static_cast<int>(meta.bytes), static_cast<unsigned>(total));
  }
  LOG_INF("VIMG", "asset saved: %s (%u bytes)", path, static_cast<unsigned>(total));
  return Result::Ok;
}

const char* ImageClient::resultName(Result result) {
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
    case Result::Rejected:
      return "REJECTED";
    case Result::NotConfigured:
      return "NOT_CONFIGURED";
    case Result::Upstream:
      return "UPSTREAM";
    case Result::Timeout:
      return "TIMEOUT";
    case Result::Read:
      return "READ";
    case Result::Oom:
      return "OOM";
    case Result::Download:
      return "DOWNLOAD";
    case Result::File:
      return "FILE";
    case Result::NoImage:
      return "NO_IMAGE";
  }
  return "UNKNOWN";
}

}  // namespace VoiceRelay
