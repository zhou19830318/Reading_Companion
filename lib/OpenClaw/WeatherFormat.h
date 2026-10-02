#pragma once

#include <cstddef>
#include <cstdint>

// The workbench weather card's data contract (docs/v4.0-development-plan.md
// §11.5-D3): the device asks the gateway for ONE compact JSON object via
// chat.send, and this module is the only thing that understands the shape.
// Pure C++ — no ESP-IDF, no Arduino — so test/weather_format compiles the
// same translation unit the firmware links.
//
// The same parser reads the SD cache back, which is why the cache document
// is this shape plus `fetchedAt` (epoch ms) — one parser, two callers, no
// second format to drift.
namespace OpenClaw {

// Cache document version. Bump when the shape changes; an unreadable cache
// is not fatal (the card falls back to its empty state and resyncs).
static constexpr int WEATHER_FORMAT_VERSION = 1;

// Enough for the worst case: every string field fully escape-expanded
// (city 32 B, cond 48 B x2, wind 32 B x2 -> ~384 B) plus the fixed keys,
// with headroom so a hostile reply can never fail the write at 3/4 full.
static constexpr size_t WEATHER_CACHE_CAP = 768;

struct WeatherDay {
  int hi = 0;
  int lo = 0;
  // True once hi AND lo were present. `today` without temps is a parse
  // failure; `tomorrow` without them simply renders the placeholder.
  bool hasTemps = false;
  char cond[48] = {};
  char wind[32] = {};
};

struct WeatherSnapshot {
  // Epoch ms of the sync; 0 in a chat reply (the caller stamps it) and set
  // by the cache. Drives the 6 h freshness window.
  int64_t fetchedAtMs = 0;
  char city[32] = {};
  WeatherDay today{};
  WeatherDay tomorrow{};
  bool hasTomorrow = false;
};

// Finds the first balanced {...} object inside `text` (tolerating prose and
// markdown fences around it — the gateway's reply is chat text, not a
// document) and parses the contract out of it. Returns false when there is
// no object, it is malformed or truncated, or `today`'s temperatures are
// missing. Unknown keys are ignored, so gateway-side additions are safe.
//
// On failure `out` is left unspecified — callers must not read it.
bool parseWeatherJson(const char* text, size_t len, WeatherSnapshot& out);

// Serializes the cache document (with WEATHER_FORMAT_VERSION and
// `fetchedAt`). Returns bytes written (NUL-terminated), or 0 when it does
// not fit `cap`. Strings are JSON-escaped; control bytes are dropped (they
// cannot be display text anyway).
size_t formatWeatherJson(char* out, size_t cap, const WeatherSnapshot& s);

// ── device-IP geo + gateway tools/invoke weather (F4b round 2) ───────────

// ip-api.com answer for THIS device's public IP. Fetched directly by the
// device (plain HTTP): the gateway's web_fetch would geo-locate the
// gateway's IP, not the e-reader's. Chinese UI languages get `lang=zh-CN`
// so region/city arrive as the 汉字 the workbench card shows verbatim.
struct IpGeoResult {
  bool ok = false;  // status==success AND lat/lon present
  double lat = 0.0;
  double lon = 0.0;
  char country[48] = {};
  char region[48] = {};
  char city[48] = {};
};

// Parses one ip-api.com JSON answer (`fields=status,country,regionName,city,
// lat,lon`). Order-independent; on false `out.ok` stays false.
bool parseIpGeoJson(const char* json, size_t len, IpGeoResult& out);

// ── step-2 request builders ────────────────────────────────────────────
// The open-meteo forecast URL and the gateway envelope around it. Both used
// to be snprintf'd inline in WorkbenchActivity with eyeballed caps (192 B /
// 128 B) — the real worst case is 201 B / 239 B, so every sync silently
// failed the `bodyLen < sizeof(body)` guard before a single byte left the
// device (device log: geo ok, then "sync failed" 58 ms later with no HTTP
// line). Sizing lives here, next to the tests in test/weather_format, so the
// next query parameter cannot re-open that hole.

// Worst case is latitude=-90.00 / longitude=-180.00 (6 chars each, sign
// included): 201 B. 256 leaves room for another query parameter without a
// silent truncation — snprintf cutting a URL mid-parameter produces a
// well-formed-looking but wrong request.
static constexpr size_t WEATHER_FORECAST_URL_CAP = 256;
// {"tool":"web_fetch","args":{"url":"..."}}: 35 B of prefix + 3 B of suffix
// + the 201 B worst-case URL = 239 B. 320 gives the same headroom. Callers
// must pass at least this cap — a smaller one makes the builders return 0
// instead of posting a truncated body.
static constexpr size_t WEATHER_INVOKE_BODY_CAP = 320;

// Writes the open-meteo forecast URL for (lat,lon) into `out`. Returns bytes
// written (NUL-terminated), or 0 when it does not fit `cap` — callers treat
// 0 as a sync failure, never as a shorter URL. On 0, `out[0]` is NUL (cap>0).
size_t formatForecastUrl(char* out, size_t cap, double lat, double lon);

// Writes the gateway POST /tools/invoke body that fetches that forecast.
// Returns bytes written (NUL-terminated), or 0 when `cap` is below
// WEATHER_INVOKE_BODY_CAP or the assembled document does not fit. On 0,
// `out[0]` is NUL (cap>0).
size_t formatWeatherInvokeBody(char* out, size_t cap, double lat, double lon);

// The reply of POST /tools/invoke {"tool":"web_fetch", ...} for the
// open-meteo forecast URL: a double envelope — result.details.text is the
// gateway's SECURITY-NOTICE wrapper around the origin body, which itself is
// fenced by `Source: Web Fetch\n---\n` ... `<<<END_TOOL_UNTRUSTED_CONTENT`.
// Extracts the open-meteo document out of both layers and fills `out`:
//   city        <- caller's geo city (the origin JSON has no place name)
//   today/tomorrow hi/lo <- daily.temperature_2m_max/min [0]/[1] (rounded)
//   day.cond    <- decimal WMO weather_code (render maps it through tr())
//   today.wind  <- "deg,speed" from current wind_* (render maps the sector)
// False (out unspecified) on any layer missing, truncated, or without
// today's temperatures.
bool parseWeatherInvokeResponse(const char* json, size_t len, const char* city, WeatherSnapshot& out);

}  // namespace OpenClaw
