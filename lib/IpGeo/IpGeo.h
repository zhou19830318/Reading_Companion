#pragma once

#include <cstddef>

// ip-api.com language subdomain table (F4b round 2).
//
// The workbench's weather sync queries ip-api.com directly for THIS device's
// public-IP location, and asks for the place names in the UI's language via
// the two-letter subdomain (ip-api.com/json/?lang=zh-CN -> 江苏 / 南京). The
// service speaks exactly 8 languages plus its English default, so every other
// UI language gets the English names — an honest fallback, not a missing
// translation: the names are data from the geo source, not UI strings, so
// they deliberately do NOT go through tr().
//
// Pure C++ — no ESP-IDF, no Arduino — host-testable by construction.
namespace IpGeo {

// Longest documented value + NUL.
constexpr size_t LANG_CODE_SIZE = 8;

// Maps an I18n language code (LANGUAGE_CODES[] spelling, e.g. "zh-Hans",
// "EN", "pt-BR") to the ip-api language code. Copies at most
// LANG_CODE_SIZE - 1 bytes; always NUL-terminates. Never fails: an unknown
// code yields "en", which the service serves by default.
void langCodeFor(const char* i18nCode, char* out, size_t cap);

}  // namespace IpGeo
