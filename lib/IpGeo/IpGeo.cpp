#include "IpGeo.h"

#include <cstdio>
#include <cstring>

// ip-api.com's documented data fields (https://ip-api.com/docs/api:json):
// zh-CN, de, en, es, fr, ja, pt-BR, ru — anything else answers in English.
// Matched case-insensitively against the prefix of I18n's language codes
// ("zh-Hans" -> zh-CN, "ES" -> es) so the mapping survives case drift.
namespace IpGeo {
namespace {

struct LangEntry {
  const char* prefix;     // lowercase prefix of the i18n code
  const char* ipApiCode;  // ip-api's language code
};

// ip-api matches full language tags with a hyphen ("zh-CN", "pt-BR"); its
// table has exactly one Chinese and one Portuguese variant, so the bare
// prefix is unambiguous. First match wins — order matters only for codes
// that share a prefix (pt-BR before pt would be wrong; they don't overlap
// here because "pt-BR" starts with "pt-B", so the longer prefix goes first).
constexpr LangEntry LANG_TABLE[] = {
    {"zh", "zh-CN"},  // zh-Hans / zh-Hant -> 简体/繁體地名 (ip-api serves one)
    {"de", "de"},    {"en", "en"},       {"es", "es"},    {"fr", "fr"},
    {"ja", "ja"},    {"pt-br", "pt-BR"}, {"pt", "pt-BR"},  // bare Portuguese: ip-api only speaks pt-BR
    {"ru", "ru"},
};

char lower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Case-insensitive prefix test: does `code` start with `prefix`?
bool startsWithLower(const char* code, const char* prefix) {
  for (size_t i = 0; prefix[i] != '\0'; ++i) {
    if (code[i] == '\0' || lower(code[i]) != prefix[i]) return false;
  }
  return true;
}

}  // namespace

void langCodeFor(const char* i18nCode, char* out, const size_t cap) {
  const char* mapped = "en";  // the service's own default — never a miss
  if (i18nCode != nullptr && i18nCode[0] != '\0' && out != nullptr && cap > 0) {
    for (const LangEntry& e : LANG_TABLE) {
      if (startsWithLower(i18nCode, e.prefix)) {
        mapped = e.ipApiCode;
        break;
      }
    }
  }
  snprintf(out, cap, "%s", mapped);
}

}  // namespace IpGeo
