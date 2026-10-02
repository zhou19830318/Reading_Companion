#pragma once

#include <cctype>
#include <cstddef>
#include <cstring>

// Credential redaction for every place a gateway payload is echoed: the DBG
// frame dump, the protocol status line drawn on the panel, a server error
// message. The auth response carries the issued device token and our own
// connect request carries the shared token, and a log line does not end at the
// serial port — logPrintf() also feeds getLastLogs(), the ring buffer attached
// to crash reports, so anything printed once is kept until the next clear.
//
// Pure C++ — no ESP-IDF, no Arduino — so test/openclaw_redact compiles the
// same translation unit the firmware links.
namespace OpenClaw {

// Shown in place of a payload that carries a credential. Short enough for the
// 96-byte status line, and identical in a log line, so panel and serial stay
// comparable while debugging.
inline constexpr const char CREDENTIAL_REDACTED[] = "[redacted: credential]";

// True when [s, s + n) contains a JSON key that names a credential, i.e.
//
//     "deviceToken":"4f2c…"
//
// The quote that closes the key, the colon after it and the opening quote of
// the value are required (optional space around the colon): a server message
// that merely mentions a token ("token expired") is not a credential and has
// to stay readable. Matching is case-insensitive so deviceToken and
// accessToken are caught by one list.
//
// Deliberately fail-closed for the pattern it recognises: callers substitute a
// placeholder instead of trying to cut the value out of the payload.
inline bool payloadCarriesCredential(const char* s, size_t n) {
  static constexpr const char* CRED_KEYS[] = {"token",   "password",      "secret",    "apikey",
                                              "api_key", "authorization", "credential"};
  if (s == nullptr || n == 0) return false;
  const auto keyEqualsNoCase = [](const char* p, const char* key, size_t klen) {
    for (size_t j = 0; j < klen; j++) {
      if (tolower(static_cast<unsigned char>(p[j])) != key[j]) return false;
    }
    return true;
  };
  const char* end = s + n;
  for (const char* key : CRED_KEYS) {
    const size_t klen = strlen(key);
    for (size_t i = 0; i + klen <= n; i++) {
      if (!keyEqualsNoCase(s + i, key, klen)) continue;
      const char* p = s + i + klen;
      if (p >= end || *p != '"') continue;
      p++;
      while (p < end && (*p == ' ' || *p == '\t')) p++;
      if (p >= end || *p != ':') continue;
      p++;
      while (p < end && (*p == ' ' || *p == '\t')) p++;
      if (p < end && *p == '"') return true;
    }
  }
  return false;
}

// A payload and its length, already substituted where necessary, so call sites
// read like a plain echo: LOG_INF("… %.*s", echo.len, echo.text).
struct Echo {
  const char* text;
  int len;
};

inline Echo safeEcho(const char* s, size_t n) {
  if (payloadCarriesCredential(s, n)) return {CREDENTIAL_REDACTED, static_cast<int>(strlen(CREDENTIAL_REDACTED))};
  return {s, static_cast<int>(n)};
}

// Same rule for the NUL-terminated strings the gateway writes itself (error
// messages, chat errors): those are shown on the panel and logged verbatim.
inline Echo safeEcho(const char* s) {
  if (s == nullptr) return {"", 0};
  return safeEcho(s, strlen(s));
}

}  // namespace OpenClaw
