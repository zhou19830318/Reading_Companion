#include "CronListFormat.h"

#include <StreamingJsonParser.h>
#include <Utf8.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace OpenClaw {
namespace {

// Container currently being read, one slot per nesting level (the parser's
// MAX_NESTING is 32, so a deeper document errors out before it can overrun
// this stack).
enum class C : uint8_t { Other, Root, Payload, Jobs, Job, Schedule, State, JobPayload };

struct CronParseCtx {
  CronJob* out = nullptr;
  size_t cap = 0;
  size_t count = 0;
  bool sawJobs = false;
  // Set when the top-level container opens. Garbage that is not JSON at all
  // parses to "depth 0, no error" in a SAX parser — without a root seen, the
  // verdict must be Error, not NoJobs.
  bool sawRoot = false;

  int depth = 0;  // 0 = outside the document
  C stack[StreamingJsonParser::MAX_NESTING] = {};
  // Last key announced by the parser, used to decide what object/array is
  // about to start. Keys are never chunked, so a bounded copy is exact.
  char lastKey[48] = {};
  // Set while an oversized string value streams through onStringPart; the
  // chunks of one value arrive back-to-back under the same key.
  bool partOpen = false;

  CronJob* cur = nullptr;  // job slot being filled, null when beyond cap
  // Drop the gateway's own routines while parsing (see isSystemCronJob).
  bool userJobsOnly = false;

  // schedule / state sub-objects arrive key-by-key, so their fields are
  // staged and composed into the job when the sub-object closes.
  char schedKind[16] = {};
  char schedExpr[40] = {};
  char schedAt[40] = {};
  int64_t everyMs = 0;
  int64_t runningAtMs = 0;
  int64_t nextRunAtMs = 0;
  char lastStatus[12] = {};

  // payload.text of the job being read — staged like the schedule/state
  // fields, then copied into the job when its payload object closes.
  char payloadText[64] = {};
};

bool keyEq(const char* key, const char* want) { return strcmp(key, want) == 0; }

// Bounded copy that always NUL-terminates; an over-long value is truncated
// (the UI truncates again for display) rather than spilling the buffer.
void copyBounded(char* dst, size_t cap, const char* src, size_t len) {
  if (cap == 0) return;
  const size_t n = len < cap - 1 ? len : cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

C here(const CronParseCtx* c) { return c->depth >= 1 ? c->stack[c->depth - 1] : C::Other; }

// onString assigns a whole value; onStringPart delivers one value as
// consecutive chunks (the first indistinguishable from a later one), so
// partOpen decides append-vs-replace for the current key.
void captureString(CronParseCtx* c, const char* value, size_t len, bool chunk) {
  char* dst = nullptr;
  size_t cap = 0;
  if (here(c) == C::Job && c->cur != nullptr) {
    if (keyEq(c->lastKey, "id")) {
      dst = c->cur->id;
      cap = sizeof(c->cur->id);
    } else if (keyEq(c->lastKey, "name")) {
      dst = c->cur->name;
      cap = sizeof(c->cur->name);
    } else if (keyEq(c->lastKey, "declarationKey")) {
      dst = c->cur->declarationKey;
      cap = sizeof(c->cur->declarationKey);
    }
  } else if (here(c) == C::JobPayload) {
    if (keyEq(c->lastKey, "text")) {
      dst = c->payloadText;
      cap = sizeof(c->payloadText);
    }
  } else if (here(c) == C::Schedule) {
    if (keyEq(c->lastKey, "kind")) {
      dst = c->schedKind;
      cap = sizeof(c->schedKind);
    } else if (keyEq(c->lastKey, "expr")) {
      dst = c->schedExpr;
      cap = sizeof(c->schedExpr);
    } else if (keyEq(c->lastKey, "at")) {
      dst = c->schedAt;
      cap = sizeof(c->schedAt);
    }
  } else if (here(c) == C::State) {
    if (keyEq(c->lastKey, "lastStatus")) {
      dst = c->lastStatus;
      cap = sizeof(c->lastStatus);
    }
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
  auto* c = static_cast<CronParseCtx*>(ctx);
  copyBounded(c->lastKey, sizeof(c->lastKey), key, len);
  c->partOpen = false;
}

void onStringCb(void* ctx, const char* value, size_t len) {
  auto* c = static_cast<CronParseCtx*>(ctx);
  c->partOpen = false;
  captureString(c, value, len, false);
}

void onStringPartCb(void* ctx, const char* chunk, size_t len) {
  captureString(static_cast<CronParseCtx*>(ctx), chunk, len, true);
}

void onNumberCb(void* ctx, const char* value, size_t len) {
  auto* c = static_cast<CronParseCtx*>(ctx);
  char buf[24];
  copyBounded(buf, sizeof(buf), value, len);
  const int64_t n = strtoll(buf, nullptr, 10);
  if (here(c) == C::Schedule && keyEq(c->lastKey, "everyMs")) {
    c->everyMs = n;
  } else if (here(c) == C::State) {
    if (keyEq(c->lastKey, "runningAtMs")) {
      c->runningAtMs = n;
    } else if (keyEq(c->lastKey, "nextRunAtMs")) {
      c->nextRunAtMs = n;
    }
  }
}

void onBoolCb(void* ctx, bool value) {
  auto* c = static_cast<CronParseCtx*>(ctx);
  if (here(c) == C::Job && c->cur != nullptr && keyEq(c->lastKey, "enabled")) {
    c->cur->enabled = value;
  }
}

void containerStart(CronParseCtx* c, C here) {
  ++c->depth;
  const C parent = c->depth >= 2 ? c->stack[c->depth - 2] : C::Other;
  if (c->depth == 1) {
    here = C::Root;
  } else if (here == C::Jobs && !(keyEq(c->lastKey, "jobs") && parent == C::Payload)) {
    here = C::Other;
  } else if (here == C::Job && parent != C::Jobs) {
    here = C::Other;
  } else if (here == C::Schedule && !(keyEq(c->lastKey, "schedule") && parent == C::Job)) {
    here = C::Other;
  } else if (here == C::State && !(keyEq(c->lastKey, "state") && parent == C::Job)) {
    here = C::Other;
  } else if (here == C::Payload && !(keyEq(c->lastKey, "payload") && parent == C::Root)) {
    here = C::Other;
  } else if (here == C::JobPayload && !(keyEq(c->lastKey, "payload") && parent == C::Job)) {
    here = C::Other;
  }
  c->stack[c->depth - 1] = here;
  c->lastKey[0] = '\0';
  c->partOpen = false;

  if (c->depth == 1) c->sawRoot = true;
  if (here == C::Jobs) c->sawJobs = true;
  if (here == C::Job) {
    c->cur = (c->out != nullptr && c->count < c->cap) ? &c->out[c->count] : nullptr;
    if (c->cur != nullptr) *c->cur = CronJob{};
  }
  if (here == C::Schedule) {
    c->schedKind[0] = '\0';
    c->schedExpr[0] = '\0';
    c->schedAt[0] = '\0';
    c->everyMs = 0;
  }
  if (here == C::State) {
    c->runningAtMs = 0;
    c->nextRunAtMs = 0;
    c->lastStatus[0] = '\0';
  }
  if (here == C::JobPayload) {
    c->payloadText[0] = '\0';
  }
}

void onObjectStartCb(void* ctx) {
  auto* c = static_cast<CronParseCtx*>(ctx);
  if (c->depth >= static_cast<int>(StreamingJsonParser::MAX_NESTING)) return;  // parser errors below
  const C parent = c->depth >= 1 ? c->stack[c->depth - 1] : C::Other;
  C here = C::Other;
  if (parent == C::Jobs) {
    here = C::Job;
  } else if (keyEq(c->lastKey, "payload")) {
    // Two different payloads: the envelope that wraps jobs[], and a job's own
    // {kind,text} object. The parent decides which this one is.
    here = (parent == C::Job) ? C::JobPayload : C::Payload;
  } else if (keyEq(c->lastKey, "schedule")) {
    here = C::Schedule;
  } else if (keyEq(c->lastKey, "state")) {
    here = C::State;
  }
  containerStart(c, here);
}

void onArrayStartCb(void* ctx) {
  auto* c = static_cast<CronParseCtx*>(ctx);
  if (c->depth >= static_cast<int>(StreamingJsonParser::MAX_NESTING)) return;
  containerStart(c, keyEq(c->lastKey, "jobs") ? C::Jobs : C::Other);
}

void composeSchedule(CronParseCtx* c) {
  if (c->cur == nullptr) return;
  // Same shapes the AIWatch reference parse emits (openclaw_client.c:1009-1070).
  if (keyEq(c->schedKind, "every")) {
    snprintf(c->cur->schedule, sizeof(c->cur->schedule), "every %ldms", static_cast<long>(c->everyMs));
  } else if (keyEq(c->schedKind, "cron")) {
    snprintf(c->cur->schedule, sizeof(c->cur->schedule), "%s", c->schedExpr);
  } else if (keyEq(c->schedKind, "at")) {
    snprintf(c->cur->schedule, sizeof(c->cur->schedule), "%s", c->schedAt);
  }
}

void composeState(CronParseCtx* c) {
  if (c->cur == nullptr) return;
  c->cur->running = c->runningAtMs > 0;
  // 0 means "none"; everything positive is kept verbatim — a real epoch-ms
  // stamp (~1.8e12) has no uint32 representation, which is exactly what the
  // old `<= 1<<32` guard got wrong (it zeroed every actual value).
  c->cur->nextRunAtMs = c->nextRunAtMs > 0 ? static_cast<uint64_t>(c->nextRunAtMs) : 0;
  snprintf(c->cur->lastStatus, sizeof(c->cur->lastStatus), "%s", c->lastStatus);
}

void containerEnd(CronParseCtx* c) {
  if (c->depth <= 0) return;
  const C here = c->stack[c->depth - 1];
  if (here == C::Schedule) {
    composeSchedule(c);
  } else if (here == C::State) {
    composeState(c);
  } else if (here == C::JobPayload) {
    if (c->cur != nullptr) {
      copyBounded(c->cur->text, sizeof(c->cur->text), c->payloadText, strlen(c->payloadText));
    }
  } else if (here == C::Job && c->cur != nullptr) {
    // Slots are claimed at close, so a partial job never counts. A system
    // routine claims nothing and its slot is reused by the next job, so the
    // cap is spent on user to-dos instead of filling up with the platform's.
    if (!(c->userJobsOnly && isSystemCronJob(*c->cur))) ++c->count;
    c->cur = nullptr;
  }
  --c->depth;
  c->lastKey[0] = '\0';
  c->partOpen = false;
}

void onObjectEndCb(void* ctx) { containerEnd(static_cast<CronParseCtx*>(ctx)); }
void onArrayEndCb(void* ctx) { containerEnd(static_cast<CronParseCtx*>(ctx)); }

}  // namespace

bool isSystemCronJob(const CronJob& job) {
  if (job.declarationKey[0] != '\0') return true;
  // The routines the gateway never declares come back as plain names; both
  // spellings are the ones a live cron.list actually returns.
  static constexpr const char* kSystemNames[] = {
      "heartbeat-main",
      "skill-collection-review-main",
      "Memory Dreaming Promotion",
  };
  for (const char* name : kSystemNames) {
    if (strcmp(job.name, name) == 0) return true;
  }
  return false;
}

namespace {

bool startsWith(const char* s, const char* prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

bool asciiDigit(const char c) { return c >= '0' && c <= '9'; }

// Byte length of a sentence-closing "！" (3 bytes in UTF-8) or "!" (1), 0 for
// anything else. A `char == '！'` literal would be a multi-character constant
// and always false — the comparison has to be a byte-wise compare.
size_t bangLen(const char* s) {
  if (startsWith(s, "！")) return strlen("！");
  if (s[0] == '!') return 1;
  return 0;
}

// Byte length of one Chinese numeral at `s`, 0 when there is none. Used by
// the scan below, which has to step over whole characters (RISC-V-safe byte
// arithmetic — no casting to a wider pointer).
size_t cnNumeralLen(const char* s) {
  static constexpr const char* kNumerals[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十", "两"};
  for (const char* n : kNumerals) {
    if (startsWith(s, n)) return strlen(n);
  }
  return 0;
}

// Leading run of separators a chat payload may open with: ASCII blanks and
// the UTF-8 ideographic space U+3000. Both are invisible where they matter
// (right after the card's "[ ]N." head) but they defeat a literal
// startsWith() match on the time clause below.
const char* skipSeparator(const char* s) {
  static constexpr const char* kIdeographicSpace = "\xE3\x80\x80";  // U+3000
  for (;;) {
    if (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r') {
      ++s;
      continue;
    }
    if (startsWith(s, kIdeographicSpace)) {
      s += strlen(kIdeographicSpace);
      continue;
    }
    return s;
  }
}

// Byte length of the pictograph a gateway payload opens with — "⏰ 12:20了！…",
// "📚 下午四点半了！…" — 0 when there is none. The card prints the sentence,
// not the sticker in front of it: the UI faces carry no emoji glyphs (they
// draw blank) and the row format starts at the words. Only a *leading*
// pictograph is dropped; one inside a title stays.
size_t decorationLen(const char* s) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  const uint32_t cp = utf8NextCodepoint(&p);
  if (cp == 0) return 0;
  const bool pictographic = (cp >= 0x2300 && cp <= 0x23FF)       // misc technical: ⏰
                            || (cp >= 0x2600 && cp <= 0x27BF)    // misc symbols + dingbats
                            || (cp >= 0x1F000 && cp <= 0x1FAFF)  // emoji blocks: 📚 🎬
                            || cp == 0xFE0E || cp == 0xFE0F      // variation selectors
                            || cp == 0x200D                      // ZWJ inside an emoji sequence
                            || cp == 0xFEFF;                     // BOM
  if (!pictographic) return 0;
  return static_cast<size_t>(p - reinterpret_cast<const unsigned char*>(s));
}

// Byte length of the time clause the sentence opens with, 0 if it does not
// open with one. Two shapes are recognised, both taken from live gateway
// payloads: "12:20了！" (ASCII clock — and its full-width-punctuation twin
// "12：20了！", which voice transcripts produce) and "下午四点半了！"
// (period word, numeral, 点, optional 半 / N分, then 了).
size_t leadingTimeClause(const char* s) {
  if (asciiDigit(s[0])) {
    size_t i = 0;
    while (asciiDigit(s[i])) ++i;
    const size_t colonLen = s[i] == ':' ? 1 : (startsWith(s + i, "：") ? strlen("：") : 0);
    if (colonLen > 0) {
      size_t j = i + colonLen;
      while (asciiDigit(s[j])) ++j;
      if (j > i + colonLen) {
        const char* after = skipSeparator(s + j);
        if (startsWith(after, "了")) {
          j = static_cast<size_t>(after - s);
          j += strlen("了");
          j += bangLen(s + j);
          return j;
        }
      }
    }
  }

  size_t i = 0;
  static constexpr const char* kPeriods[] = {"早上", "上午", "中午", "下午", "晚上", "今早"};
  for (const char* p : kPeriods) {
    if (startsWith(s, p)) {
      i += strlen(p);
      break;
    }
  }
  size_t j = i;
  for (;;) {
    if (asciiDigit(s[j])) {
      ++j;
      continue;
    }
    const size_t n = cnNumeralLen(s + j);
    if (n == 0) break;
    j += n;
  }
  if (j == i || !startsWith(s + j, "点")) return 0;
  j += strlen("点");
  if (startsWith(s + j, "半")) {
    j += strlen("半");
  } else {
    size_t k = j;
    while (asciiDigit(s[k])) ++k;
    if (k > j && startsWith(s + k, "分")) k += strlen("分");
    j = k;
  }
  if (!startsWith(s + j, "了")) return 0;
  j += strlen("了");
  j += bangLen(s + j);
  return j;
}

}  // namespace

void formatTodoTitle(char* dst, const size_t cap, const char* text) {
  if (dst == nullptr || cap == 0) return;
  dst[0] = '\0';
  if (text == nullptr) return;

  const char* p = text;
  // Decoration and the spaces around it alternate: "⏰ 12:20了！", "📚 下午…" —
  // loop until the sentence itself starts.
  for (;;) {
    p = skipSeparator(p);
    const size_t deco = decorationLen(p);
    if (deco == 0) break;
    p += deco;
  }
  const size_t timeLen = leadingTimeClause(p);
  if (timeLen > 0) p += timeLen;
  if (startsWith(p, "该去")) p += strlen("该去");

  // Trailing sentence particles, outermost first: "了！" has to go as one
  // unit or "放学了" loses its 了 and reads wrong. Three passes is more than
  // any live sample needs ("了！" then "！" at most.
  size_t len = strlen(p);
  static constexpr const char* kSuffixes[] = {"了！", "了!", "！", "!", "。", "了"};
  for (int pass = 0; pass < 3 && len > 0; ++pass) {
    size_t cut = 0;
    for (const char* suf : kSuffixes) {
      const size_t n = strlen(suf);
      if (len >= n && memcmp(p + len - n, suf, n) == 0) {
        cut = n;
        break;
      }
    }
    if (cut == 0) break;
    len -= cut;
  }
  copyBounded(dst, cap, p, len);
}

const char* formatTodoClock(char* dst, const size_t cap, const int hour24, const int minute, const char* am,
                            const char* pm) {
  const int h = hour24 >= 0 && hour24 < 24 ? hour24 : 0;
  const char* meridiem = h < 12 ? (am != nullptr ? am : "") : (pm != nullptr ? pm : "");
  if (dst == nullptr || cap == 0) return meridiem;
  int h12 = h % 12;
  if (h12 == 0) h12 = 12;
  const int m = minute >= 0 && minute < 60 ? minute : 0;
  snprintf(dst, cap, "%d:%02d", h12, m);
  return meridiem;
}

CronParse parseCronList(const char* json, const size_t len, CronJob* out, const size_t cap, size_t* outCount,
                        const bool userJobsOnly) {
  if (outCount != nullptr) *outCount = 0;
  if (json == nullptr || len == 0) return CronParse::Error;

  CronParseCtx c;
  c.out = out;
  c.cap = (out != nullptr) ? cap : 0;
  c.userJobsOnly = userJobsOnly;

  JsonCallbacks cb = {};
  cb.ctx = &c;
  cb.onKey = &onKeyCb;
  cb.onString = &onStringCb;
  cb.onStringPart = &onStringPartCb;
  cb.onNumber = &onNumberCb;
  cb.onBool = &onBoolCb;
  cb.onObjectStart = &onObjectStartCb;
  cb.onObjectEnd = &onObjectEndCb;
  cb.onArrayStart = &onArrayStartCb;
  cb.onArrayEnd = &onArrayEndCb;

  StreamingJsonParser parser(cb);
  parser.feed(json, len);
  // The parser reports syntax errors but not truncation: a frame cut short
  // mid-array leaves no error flag. Requiring the nesting depth to have
  // returned to zero is what tells a complete document from a partial one
  // (same completeness test as OpenClawHandshake::parseFrame).
  if (parser.hasError() || c.depth != 0 || !c.sawRoot) return CronParse::Error;
  if (!c.sawJobs) return CronParse::NoJobs;
  if (outCount != nullptr) *outCount = c.count;
  return CronParse::Ok;
}

}  // namespace OpenClaw
