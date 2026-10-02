#pragma once

#include <SttCodec.h>

#include <cstddef>
#include <cstdint>

// Cloud ASR transport for the MiMo endpoint (POST + SSE reply).
//
// Device-only: built on esp_http_client / mbedTLS, so unlike SttCodec it is
// deliberately NOT host-compilable. The wire format it carries (and every byte
// of the request body) is covered by test/stt_codec instead.
//
// Memory model: the request body is never assembled in one buffer. A 5 s
// capture is 160 KB of PCM → 215 KB of base64 → ~216 KB of JSON. Instead the
// body is written to the socket in three pieces using the same size that
// requestBodySize() declares:
//
//   writeBodyHead() | WavBase64Writer::next()... | writeBodyTail()
//
// so the peak working set is the 28 KB WavBase64Writer allocation (PSRAM) plus
// a 2 KB read buffer, instead of ~216 KB.
//
// The API key is read from the SD card at runtime and is never compiled in.
class SttClient {
 public:
  // Required: the `api-key` header value, one line.
  static constexpr const char* KEY_FILE = "/stt_api_key.txt";
  // Optional: full POST URL. Only for bring-up (a plain-http local server
  // exercises the transport without TLS, a valid clock or real credentials).
  static constexpr const char* ENDPOINT_FILE = "/stt_endpoint.txt";
  static constexpr const char* DEFAULT_URL = "https://api.xiaomimimo.com/v1/chat/completions";

  struct Config {
    const char* url = DEFAULT_URL;
    const char* apiKey = nullptr;  // nullptr omits the api-key header
    const char* model = SttCodec::DEFAULT_MODEL;
  };

  enum Result {
    OK = 0,
    ERR_NO_KEY,   // key file missing/empty
    ERR_NO_PCM,   // nothing to transcribe
    ERR_ALLOC,    // out of memory
    ERR_TIME,     // system clock unset, TLS certificate dates cannot validate
    ERR_CONNECT,  // socket/TLS open failed (no WiFi, bad DNS, handshake)
    ERR_WRITE,    // request body upload failed
    ERR_HTTP,     // non-2xx response
    ERR_READ,     // response body read failed
    ERR_NO_TEXT,  // 2xx but no transcript arrived
  };

  struct Stats {
    int httpStatus = 0;
    size_t requestBytes = 0;  // body bytes written
    size_t responseBytes = 0;
  };

  // Outcome of a credential probe: the status line plus the head of the
  // response body, which on a refusal is the gateway's own error message
  // ("Invalid api key", …) — the string the settings page shows the user.
  struct KeyProbe {
    int httpStatus = 0;
    char detail[160] = {};
  };

  // Reads KEY_FILE and (if present) ENDPOINT_FILE into the caller-owned
  // buffers and points cfg at them. Returns false when no usable key exists;
  // cfg.url still ends up at DEFAULT_URL so callers can log what they tried.
  // Buffers must outlive any transcribe() call using cfg.
  static bool loadConfig(char* urlBuf, size_t urlCap, char* keyBuf, size_t keyCap, Config& cfg);

  // Uploads `samples` mono s16le frames and streams the SSE reply into parser.
  // Blocking: returns when the response is complete, an error occurs, or the
  // HTTP exchange fails. Fills stats (may be null) for diagnostics.
  // On OK, parser.text() holds the transcript (possibly empty → ERR_NO_TEXT).
  // For https endpoints an unset system clock is reported as ERR_TIME before
  // any socket is opened (the cert bundle validates certificate dates).
  static Result transcribe(const Config& cfg, const int16_t* pcm, size_t samples, uint32_t sampleRate,
                           SttCodec::SseTranscriptParser& parser, Stats* stats = nullptr);

  // Static, human-readable name for a Result — for logs and error strings.
  static const char* resultName(Result r);

  // Sends one minimal request (no audio, no SSE parsing) to cfg.url with
  // cfg.apiKey so the web settings page can verify a stored key: OK for 2xx,
  // ERR_HTTP otherwise with probe.httpStatus / probe.detail filled in. The
  // gateway checks credentials before it validates the body, so a 4xx that is
  // not 401/403 still means the key was accepted. Bounded at 15 s — the web
  // server task is blocked for the duration, exactly as for the OpenClaw test.
  static Result probeKey(const Config& cfg, KeyProbe& probe);
};
