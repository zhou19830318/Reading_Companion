#include "WeatherFormat.h"

#include <Memory.h>
#include <StreamingJsonParser.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace OpenClaw {
namespace {

// Container currently being read, one slot per nesting level.
enum class C : uint8_t { Other, Root, Today, Tomorrow };

struct WeatherParseCtx {
  WeatherSnapshot* out = nullptr;
  int depth = 0;  // 0 = outside the document
  C stack[StreamingJsonParser::MAX_NESTING] = {};
  // Last key announced by the parser, used to decide what object is about
  // to start. Keys are never chunked, so a bounded copy is exact.
  char lastKey[32] = {};
  // Set while an oversized string value streams through onStringPart; the
  // chunks of one value arrive back-to-back under the same key.
  bool partOpen = false;

  // hi/lo are captured per key and committed when their day object closes,
  // so key order inside the day object cannot matter.
  bool hiSeen[2] = {false, false};  // 0 = today, 1 = tomorrow
  bool loSeen[2] = {false, false};
};

bool keyEq(const char* key, const char* want) { return strcmp(key, want) == 0; }

void copyBounded(char* dst, const size_t cap, const char* src, const size_t len) {
  if (cap == 0) return;
  const size_t n = len < cap - 1 ? len : cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

C here(const WeatherParseCtx* c) { return c->depth >= 1 ? c->stack[c->depth - 1] : C::Other; }

WeatherDay& dayOf(WeatherParseCtx* c) { return here(c) == C::Tomorrow ? c->out->tomorrow : c->out->today; }

int dayIndex(const WeatherParseCtx* c) { return here(c) == C::Tomorrow ? 1 : 0; }

// The model may quote numbers ("22" instead of 22) — both spellings land
// here so one contract violation does not cost a whole sync round.
void setTemp(WeatherParseCtx* c, const bool isHi, const int v) {
  WeatherDay& d = dayOf(c);
  if (isHi) {
    d.hi = v;
    c->hiSeen[dayIndex(c)] = true;
  } else {
    d.lo = v;
    c->loSeen[dayIndex(c)] = true;
  }
}

void captureNumberValue(WeatherParseCtx* c, const char* value, const size_t len) {
  char buf[24];
  copyBounded(buf, sizeof(buf), value, len);
  const C cur = here(c);
  if (cur == C::Root && keyEq(c->lastKey, "fetchedAt")) {
    c->out->fetchedAtMs = strtoll(buf, nullptr, 10);
    return;
  }
  if (cur != C::Today && cur != C::Tomorrow) return;
  if (keyEq(c->lastKey, "hi")) {
    setTemp(c, true, atoi(buf));
  } else if (keyEq(c->lastKey, "lo")) {
    setTemp(c, false, atoi(buf));
  }
}

void captureString(WeatherParseCtx* c, const char* value, const size_t len, const bool chunk) {
  const C cur = here(c);
  char* dst = nullptr;
  size_t cap = 0;
  if (cur == C::Root && keyEq(c->lastKey, "city")) {
    dst = c->out->city;
    cap = sizeof(c->out->city);
  } else if ((cur == C::Today || cur == C::Tomorrow) && keyEq(c->lastKey, "cond")) {
    dst = dayOf(c).cond;
    cap = sizeof(WeatherDay::cond);
  } else if ((cur == C::Today || cur == C::Tomorrow) && keyEq(c->lastKey, "wind")) {
    dst = dayOf(c).wind;
    cap = sizeof(WeatherDay::wind);
  } else if ((cur == C::Today || cur == C::Tomorrow) && keyEq(c->lastKey, "hi")) {
    char buf[24];
    copyBounded(buf, sizeof(buf), value, len);
    setTemp(c, true, atoi(buf));
    return;
  } else if ((cur == C::Today || cur == C::Tomorrow) && keyEq(c->lastKey, "lo")) {
    char buf[24];
    copyBounded(buf, sizeof(buf), value, len);
    setTemp(c, false, atoi(buf));
    return;
  } else if (cur == C::Root && keyEq(c->lastKey, "fetchedAt")) {
    char buf[24];
    copyBounded(buf, sizeof(buf), value, len);
    c->out->fetchedAtMs = strtoll(buf, nullptr, 10);
    return;
  }
  if (dst == nullptr) return;
  if (chunk && c->partOpen) {
    const size_t used = strnlen(dst, cap);
    if (used < cap - 1) copyBounded(dst + used, cap - used, value, len);
  } else {
    copyBounded(dst, cap, value, len);
  }
  c->partOpen = chunk;
}

void onKeyCb(void* ctx, const char* key, size_t len) {
  auto* c = static_cast<WeatherParseCtx*>(ctx);
  copyBounded(c->lastKey, sizeof(c->lastKey), key, len);
  c->partOpen = false;
}

void onStringCb(void* ctx, const char* value, size_t len) {
  auto* c = static_cast<WeatherParseCtx*>(ctx);
  c->partOpen = false;
  captureString(c, value, len, false);
}

void onStringPartCb(void* ctx, const char* chunk, size_t len) {
  captureString(static_cast<WeatherParseCtx*>(ctx), chunk, len, true);
}

void onNumberCb(void* ctx, const char* value, size_t len) {
  captureNumberValue(static_cast<WeatherParseCtx*>(ctx), value, len);
}

void containerStart(WeatherParseCtx* c, C here_) {
  ++c->depth;
  const C parent = c->depth >= 2 ? c->stack[c->depth - 2] : C::Other;
  if (c->depth == 1) {
    here_ = C::Root;
  } else if (here_ == C::Today && !(keyEq(c->lastKey, "today") && parent == C::Root)) {
    here_ = C::Other;
  } else if (here_ == C::Tomorrow && !(keyEq(c->lastKey, "tomorrow") && parent == C::Root)) {
    here_ = C::Other;
  }
  c->stack[c->depth - 1] = here_;
  c->lastKey[0] = '\0';
  c->partOpen = false;
}

void onObjectStartCb(void* ctx) {
  auto* c = static_cast<WeatherParseCtx*>(ctx);
  if (c->depth >= static_cast<int>(StreamingJsonParser::MAX_NESTING)) return;  // parser errors below
  C candidate = C::Other;
  if (keyEq(c->lastKey, "today")) {
    candidate = C::Today;
  } else if (keyEq(c->lastKey, "tomorrow")) {
    candidate = C::Tomorrow;
  }
  containerStart(c, candidate);
}

void onArrayStartCb(void* ctx) {
  auto* c = static_cast<WeatherParseCtx*>(ctx);
  if (c->depth >= static_cast<int>(StreamingJsonParser::MAX_NESTING)) return;
  containerStart(c, C::Other);
}

void containerEnd(WeatherParseCtx* c) {
  if (c->depth <= 0) return;
  const C ending = c->stack[c->depth - 1];
  if (ending == C::Today) {
    c->out->today.hasTemps = c->hiSeen[0] && c->loSeen[0];
  } else if (ending == C::Tomorrow) {
    c->out->tomorrow.hasTemps = c->hiSeen[1] && c->loSeen[1];
    // A tomorrow block without temperatures renders as the placeholder, so
    // it must not advertise itself as present.
    c->out->hasTomorrow = c->out->tomorrow.hasTemps;
  }
  --c->depth;
  c->lastKey[0] = '\0';
  c->partOpen = false;
}

void onObjectEndCb(void* ctx) { containerEnd(static_cast<WeatherParseCtx*>(ctx)); }
void onArrayEndCb(void* ctx) { containerEnd(static_cast<WeatherParseCtx*>(ctx)); }

// Locates the balanced {...} span *starting at* `begin`, honouring strings
// and escapes so a brace inside a quoted value cannot end the object early.
// False when the brace never closes inside `len`.
bool balancedSpanFrom(const char* text, const size_t len, const size_t begin, size_t& span) {
  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t i = begin; i < len; ++i) {
    const char ch = text[i];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        inString = false;
      }
      continue;
    }
    if (ch == '"') {
      inString = true;
    } else if (ch == '{') {
      ++depth;
    } else if (ch == '}') {
      if (--depth == 0) {
        span = i + 1 - begin;
        return true;
      }
    }
  }
  return false;  // unbalanced — prose with a stray brace, or a cut-off reply
}

// Bounded appender for the cache document. `cap` always keeps one byte back
// for the NUL, so a rejected write can never half-terminate the buffer.
struct Writer {
  char* buf;
  size_t cap;
  size_t used = 0;
  bool ok = true;

  void put(const char c) {
    if (!ok) return;
    if (used + 1 >= cap) {
      ok = false;
      return;
    }
    buf[used++] = c;
  }
  void lit(const char* s) {
    for (; *s != '\0'; ++s) put(*s);
  }
  void num(const long long v) {
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%lld", v);
    lit(tmp);
  }
  // JSON string body: escape the two characters JSON cares about, drop
  // control bytes (they cannot be display text), pass UTF-8 through.
  void esc(const char* s) {
    for (; *s != '\0'; ++s) {
      const unsigned char c = static_cast<unsigned char>(*s);
      if (c == '"' || c == '\\') {
        put('\\');
        put(static_cast<char>(c));
      } else if (c >= 0x20) {
        put(static_cast<char>(c));
      }
    }
  }
  void day(const WeatherDay& d) {
    lit("{\"hi\":");
    num(d.hi);
    lit(",\"lo\":");
    num(d.lo);
    lit(",\"cond\":\"");
    esc(d.cond);
    lit("\",\"wind\":\"");
    esc(d.wind);
    lit("\"}");
  }
};

// Parses one candidate {...} span; false when it is malformed, truncated,
// or lacks today's temperatures.
bool tryParseCandidate(const char* p, const size_t n, WeatherSnapshot& w) {
  w = WeatherSnapshot{};
  WeatherParseCtx c;
  c.out = &w;

  JsonCallbacks cb = {};
  cb.ctx = &c;
  cb.onKey = &onKeyCb;
  cb.onString = &onStringCb;
  cb.onStringPart = &onStringPartCb;
  cb.onNumber = &onNumberCb;
  cb.onObjectStart = &onObjectStartCb;
  cb.onObjectEnd = &onObjectEndCb;
  cb.onArrayStart = &onArrayStartCb;
  cb.onArrayEnd = &onArrayEndCb;

  StreamingJsonParser parser(cb);
  parser.feed(p, n);
  if (parser.hasError() || c.depth != 0) return false;
  // The card's whole point is the temperature line: a reply without
  // today's hi/lo is not this contract.
  return w.today.hasTemps;
}

}  // namespace

bool parseWeatherJson(const char* text, const size_t len, WeatherSnapshot& out) {
  if (text == nullptr || len == 0) return false;

  // Try each balanced {...} span in order until one parses as our contract:
  // the reply is chat text, so prose may open a brace of its own ("see
  // {docs}") and markdown fences or chatter may wrap the real object.
  size_t cursor = 0;
  while (cursor < len) {
    const void* p = memchr(text + cursor, '{', len - cursor);
    if (p == nullptr) return false;
    const size_t begin = static_cast<const char*>(p) - text;
    size_t span = 0;
    if (balancedSpanFrom(text, len, begin, span)) {
      WeatherSnapshot w{};
      if (tryParseCandidate(text + begin, span, w)) {
        out = w;
        return true;
      }
    }
    // Either not our contract or never balanced — skip past this '{' and
    // keep looking; the real object may sit further along.
    cursor = begin + 1;
  }
  return false;
}

size_t formatWeatherJson(char* out, const size_t cap, const WeatherSnapshot& s) {
  if (out == nullptr || cap < 64) return 0;
  Writer w{out, cap};
  w.lit("{\"v\":");
  w.num(WEATHER_FORMAT_VERSION);
  w.lit(",\"fetchedAt\":");
  w.num(static_cast<long long>(s.fetchedAtMs));
  w.lit(",\"city\":\"");
  w.esc(s.city);
  w.lit("\",\"today\":");
  w.day(s.today);
  if (s.hasTomorrow) {
    w.lit(",\"tomorrow\":");
    w.day(s.tomorrow);
  }
  w.lit("}");
  if (!w.ok) return 0;
  out[w.used] = '\0';
  return w.used;
}

// ── tools/invoke + ip-api parsing (F4b round 2) ───────────────────────────

namespace {

// Transient capture of one nested string out of the invoke envelope. The
// gateway's details.text for open-meteo measures ~1.6 KB — heap, not the
// <256 B stack budget; freed by the unique_ptr on every return path.
constexpr size_t INVOKE_TEXT_CAP = 4096;

struct InvokeTextCtx {
  char* dst = nullptr;
  size_t cap = 0;  // includes the reserved NUL byte
  size_t used = 0;
  bool overflow = false;
  bool partOpen = false;
  int depth = 0;
  bool inResult = false;
  bool inDetails = false;
  bool atText = false;
  char lastKey[32] = {};
};

void invokeKeyCb(void* ctx, const char* key, const size_t len) {
  auto* c = static_cast<InvokeTextCtx*>(ctx);
  copyBounded(c->lastKey, sizeof(c->lastKey), key, len);
  c->atText = c->inDetails && keyEq(c->lastKey, "text");
  c->partOpen = false;
}

void invokeCapture(InvokeTextCtx* c, const char* value, const size_t len, const bool chunk) {
  if (!c->atText || !c->inDetails) return;
  if (!chunk) {
    c->used = 0;  // a fresh value starts over (both spellings land here)
  } else if (!c->partOpen) {
    c->partOpen = true;
    c->used = 0;
  }
  if (c->used + 1 >= c->cap) {
    c->overflow = true;
    return;
  }
  const size_t room = c->cap - 1 - c->used;
  const size_t n = len < room ? len : room;
  memcpy(c->dst + c->used, value, n);
  c->used += n;
  if (n < len) c->overflow = true;
}

void invokeStringCb(void* ctx, const char* value, const size_t len) {
  auto* c = static_cast<InvokeTextCtx*>(ctx);
  c->partOpen = false;
  invokeCapture(c, value, len, false);
}

void invokeStringPartCb(void* ctx, const char* chunk, const size_t len) {
  invokeCapture(static_cast<InvokeTextCtx*>(ctx), chunk, len, true);
}

void invokeObjectStartCb(void* ctx) {
  auto* c = static_cast<InvokeTextCtx*>(ctx);
  if (c->depth >= static_cast<int>(StreamingJsonParser::MAX_NESTING)) return;
  ++c->depth;
  if (c->depth == 2 && keyEq(c->lastKey, "result")) {
    c->inResult = true;
  } else if (c->inResult && c->depth == 3 && keyEq(c->lastKey, "details")) {
    c->inDetails = true;
  }
  c->lastKey[0] = '\0';
  c->atText = false;
  c->partOpen = false;
}

void invokeObjectEndCb(void* ctx) {
  auto* c = static_cast<InvokeTextCtx*>(ctx);
  if (c->depth <= 0) return;
  if (c->inDetails && c->depth == 3) c->inDetails = false;
  if (c->inResult && c->depth == 2) c->inResult = false;
  --c->depth;
  c->lastKey[0] = '\0';
  c->atText = false;
  c->partOpen = false;
}

// Stage 2: the open-meteo document inside the un-wrapped wrapper text.
struct OmCtx {
  WeatherSnapshot* out = nullptr;
  int depth = 0;
  bool inDaily = false;
  bool inCurrent = false;
  int arrIdx = 0;  // position inside the daily array currently being read
  bool t0hi = false, t0lo = false, t1hi = false, t1lo = false;
  int windDeg = -1;
  int windSpd = -1;
  char lastKey[32] = {};
};

int roundInt(const char* v, const size_t len) {
  char buf[24];
  copyBounded(buf, sizeof(buf), v, len);
  return static_cast<int>(std::lround(strtod(buf, nullptr)));
}

void omKeyCb(void* ctx, const char* key, const size_t len) {
  auto* c = static_cast<OmCtx*>(ctx);
  copyBounded(c->lastKey, sizeof(c->lastKey), key, len);
}

void omNumberCb(void* ctx, const char* value, const size_t len) {
  auto* c = static_cast<OmCtx*>(ctx);
  if (c->inCurrent) {
    if (keyEq(c->lastKey, "wind_direction_10m")) {
      c->windDeg = roundInt(value, len);
    } else if (keyEq(c->lastKey, "wind_speed_10m")) {
      c->windSpd = roundInt(value, len);
    }
    return;
  }
  if (!c->inDaily) return;
  const int idx = c->arrIdx++;
  WeatherDay& d = idx == 0 ? c->out->today : c->out->tomorrow;
  if (keyEq(c->lastKey, "temperature_2m_max")) {
    d.hi = roundInt(value, len);
    (idx == 0 ? c->t0hi : c->t1hi) = true;
  } else if (keyEq(c->lastKey, "temperature_2m_min")) {
    d.lo = roundInt(value, len);
    (idx == 0 ? c->t0lo : c->t1lo) = true;
  } else if (keyEq(c->lastKey, "weather_code") && idx < 2) {
    copyBounded(d.cond, sizeof(d.cond), value, len);
  }
}

void omObjectStartCb(void* ctx) {
  auto* c = static_cast<OmCtx*>(ctx);
  if (c->depth >= static_cast<int>(StreamingJsonParser::MAX_NESTING)) return;
  ++c->depth;
  if (c->depth == 2 && keyEq(c->lastKey, "daily")) {
    c->inDaily = true;
  } else if (c->depth == 2 && keyEq(c->lastKey, "current")) {
    c->inCurrent = true;
  }
  c->lastKey[0] = '\0';
}

void omObjectEndCb(void* ctx) {
  auto* c = static_cast<OmCtx*>(ctx);
  if (c->depth <= 0) return;
  if (c->depth == 2) {
    c->inDaily = false;
    c->inCurrent = false;
  }
  --c->depth;
  c->lastKey[0] = '\0';
}

void omArrayStartCb(void* ctx) { static_cast<OmCtx*>(ctx)->arrIdx = 0; }

bool omFinish(OmCtx& c) {
  WeatherSnapshot& w = *c.out;
  w.today.hasTemps = c.t0hi && c.t0lo;
  w.tomorrow.hasTemps = c.t1hi && c.t1lo;
  // A tomorrow without both temperatures renders as the placeholder, so it
  // must not advertise itself as present (same rule as the contract parser).
  w.hasTomorrow = w.tomorrow.hasTemps;
  if (c.windDeg >= 0 && c.windSpd >= 0) {
    snprintf(w.today.wind, sizeof(w.today.wind), "%d,%d", c.windDeg % 360, c.windSpd);
  }
  return w.today.hasTemps;
}

// Stage 3: ip-api.com — flat root-level keys only.
struct GeoCtx {
  IpGeoResult* out = nullptr;
  int depth = 0;
  bool partOpen = false;
  char lastKey[32] = {};
  char status[16] = {};
  bool latSeen = false;
  bool lonSeen = false;
  char* strDst = nullptr;
  size_t strCap = 0;
};

void geoSelect(GeoCtx* c) {
  c->strDst = nullptr;
  c->strCap = 0;
  if (c->depth != 1) return;
  if (keyEq(c->lastKey, "country")) {
    c->strDst = c->out->country;
    c->strCap = sizeof(IpGeoResult::country);
  } else if (keyEq(c->lastKey, "regionName")) {
    c->strDst = c->out->region;
    c->strCap = sizeof(IpGeoResult::region);
  } else if (keyEq(c->lastKey, "city")) {
    c->strDst = c->out->city;
    c->strCap = sizeof(IpGeoResult::city);
  }
}

void geoKeyCb(void* ctx, const char* key, const size_t len) {
  auto* c = static_cast<GeoCtx*>(ctx);
  copyBounded(c->lastKey, sizeof(c->lastKey), key, len);
  c->partOpen = false;
  geoSelect(c);
}

void geoCapture(GeoCtx* c, const char* value, const size_t len, const bool chunk) {
  if (c->depth == 1 && keyEq(c->lastKey, "status")) {
    copyBounded(c->status, sizeof(c->status), value, len);
    return;
  }
  if (c->strDst == nullptr) return;
  if (chunk && c->partOpen) {
    const size_t used = strnlen(c->strDst, c->strCap);
    if (used < c->strCap - 1) copyBounded(c->strDst + used, c->strCap - used, value, len);
  } else {
    copyBounded(c->strDst, c->strCap, value, len);
  }
  c->partOpen = chunk;
}

void geoStringCb(void* ctx, const char* value, const size_t len) {
  auto* c = static_cast<GeoCtx*>(ctx);
  c->partOpen = false;
  geoCapture(c, value, len, false);
}

void geoStringPartCb(void* ctx, const char* chunk, const size_t len) {
  geoCapture(static_cast<GeoCtx*>(ctx), chunk, len, true);
}

void geoNumberCb(void* ctx, const char* value, const size_t len) {
  auto* c = static_cast<GeoCtx*>(ctx);
  if (c->depth != 1) return;
  char buf[24];
  copyBounded(buf, sizeof(buf), value, len);
  if (keyEq(c->lastKey, "lat")) {
    c->out->lat = strtod(buf, nullptr);
    c->latSeen = true;
  } else if (keyEq(c->lastKey, "lon")) {
    c->out->lon = strtod(buf, nullptr);
    c->lonSeen = true;
  }
}

void geoObjectStartCb(void* ctx) {
  auto* c = static_cast<GeoCtx*>(ctx);
  if (c->depth < static_cast<int>(StreamingJsonParser::MAX_NESTING)) ++c->depth;
  c->lastKey[0] = '\0';
  c->strDst = nullptr;
}

void geoObjectEndCb(void* ctx) {
  auto* c = static_cast<GeoCtx*>(ctx);
  if (c->depth > 0) --c->depth;
  c->lastKey[0] = '\0';
  c->strDst = nullptr;
}

JsonCallbacks makeNullCb() {
  JsonCallbacks cb = {};
  return cb;
}

}  // namespace

bool parseWeatherInvokeResponse(const char* json, const size_t len, const char* city, WeatherSnapshot& out) {
  if (json == nullptr || len == 0) return false;

  // Stage 1: pull result.details.text (the wrapped origin body) out of the
  // envelope. Heap, not stack: up to INVOKE_TEXT_CAP bytes transiently.
  auto text = makeUniqueNoThrow<char[]>(INVOKE_TEXT_CAP);
  if (!text) return false;
  InvokeTextCtx c;
  c.dst = text.get();
  c.cap = INVOKE_TEXT_CAP;
  JsonCallbacks cb = makeNullCb();
  cb.ctx = &c;
  cb.onKey = &invokeKeyCb;
  cb.onString = &invokeStringCb;
  cb.onStringPart = &invokeStringPartCb;
  cb.onObjectStart = &invokeObjectStartCb;
  cb.onObjectEnd = &invokeObjectEndCb;
  StreamingJsonParser p1(cb);
  p1.feed(json, len);
  if (p1.hasError() || c.overflow || c.used == 0) return false;
  text[c.used] = '\0';

  // Stage 2: strip the SECURITY-NOTICE wrapper at `Source: Web Fetch\n---\n`
  // and take the balanced {...} that follows — the same brace scan the chat
  // contract parser uses, so a brace inside a quoted value cannot cut it.
  const char* src = strstr(text.get(), "Source: Web Fetch");
  if (src == nullptr) return false;
  const char* dashes = strstr(src, "---\n");
  if (dashes == nullptr) return false;
  const char* begin = strchr(dashes + 4, '{');
  if (begin == nullptr) return false;
  const size_t beginIdx = static_cast<size_t>(begin - text.get());
  size_t span = 0;
  if (!balancedSpanFrom(text.get(), c.used, beginIdx, span)) return false;

  // Stage 3: the open-meteo document itself.
  WeatherSnapshot w{};
  OmCtx oc;
  oc.out = &w;
  JsonCallbacks cb2 = makeNullCb();
  cb2.ctx = &oc;
  cb2.onKey = &omKeyCb;
  cb2.onNumber = &omNumberCb;
  cb2.onObjectStart = &omObjectStartCb;
  cb2.onObjectEnd = &omObjectEndCb;
  cb2.onArrayStart = &omArrayStartCb;
  StreamingJsonParser p2(cb2);
  p2.feed(text.get() + beginIdx, span);
  if (p2.hasError() || oc.depth != 0 || !omFinish(oc)) return false;
  copyBounded(w.city, sizeof(w.city), city != nullptr ? city : "", city != nullptr ? strlen(city) : 0);
  out = w;
  return true;
}

bool parseIpGeoJson(const char* json, const size_t len, IpGeoResult& out) {
  out = IpGeoResult{};
  if (json == nullptr || len == 0) return false;
  GeoCtx c;
  c.out = &out;
  JsonCallbacks cb = makeNullCb();
  cb.ctx = &c;
  cb.onKey = &geoKeyCb;
  cb.onString = &geoStringCb;
  cb.onStringPart = &geoStringPartCb;
  cb.onNumber = &geoNumberCb;
  cb.onObjectStart = &geoObjectStartCb;
  cb.onObjectEnd = &geoObjectEndCb;
  StreamingJsonParser p(cb);
  p.feed(json, len);
  if (p.hasError() || c.depth != 0) return false;
  out.ok = strcmp(c.status, "success") == 0 && c.latSeen && c.lonSeen;
  return out.ok;
}

size_t formatForecastUrl(char* out, const size_t cap, const double lat, const double lon) {
  if (out == nullptr || cap == 0) return 0;
  // 163 B fixed + "(-)99.99" x2 <= 201 B; the cap is documented in the header.
  static constexpr char kFmt[] =
      "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
      "&daily=weather_code,temperature_2m_max,temperature_2m_min"
      "&current=wind_direction_10m,wind_speed_10m&forecast_days=2&timezone=auto";
  const int n = snprintf(out, cap, kFmt, lat, lon);
  if (n <= 0 || static_cast<size_t>(n) >= cap) {
    out[0] = '\0';  // never leave a truncated URL behind for a caller to post
    return 0;
  }
  return static_cast<size_t>(n);
}

size_t formatWeatherInvokeBody(char* out, const size_t cap, const double lat, const double lon) {
  static constexpr char kPrefix[] = "{\"tool\":\"web_fetch\",\"args\":{\"url\":\"";
  static constexpr char kSuffix[] = "\"}}";
  constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
  constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;
  // Too small a cap is a caller bug, not a shorter document: refuse rather
  // than hand back something postJson would send truncated.
  if (out == nullptr || cap < WEATHER_INVOKE_BODY_CAP) {
    if (out != nullptr && cap > 0) out[0] = '\0';
    return 0;
  }
  memcpy(out, kPrefix, kPrefixLen);
  const size_t urlLen = formatForecastUrl(out + kPrefixLen, cap - kPrefixLen, lat, lon);
  if (urlLen == 0) {
    out[0] = '\0';
    return 0;
  }
  const size_t used = kPrefixLen + urlLen;
  if (used + kSuffixLen + 1 > cap) {
    out[0] = '\0';
    return 0;
  }
  memcpy(out + used, kSuffix, kSuffixLen + 1);  // +1: the NUL is part of the suffix
  return used + kSuffixLen;
}

}  // namespace OpenClaw
