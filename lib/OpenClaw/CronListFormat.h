#pragma once

#include <cstddef>
#include <cstdint>

// ── gateway cron jobs (the workbench to-do card) ────────────────────────
// One entry of the gateway's `cron.list`. These are scheduled jobs, not
// checkboxes, so "done" in the UI means enabled rather than completed.
// Field paths mirror the gateway's cron.list reply: payload.jobs[].id /
// .name / .enabled / .payload.{kind,text} / .schedule.{kind,expr} /
// .state.{nextRunAtMs,runningAtMs,lastStatus}. Verified against the AIWatch
// reference parse (openclaw_client.c:1009-1070) and a live reply.
//
// Namespace scope, not a Session member: the workbench also reads this type
// without ever owning a Session, and an in-class array of an incomplete
// nested type is not allowed while the enclosing class is still being
// defined.
namespace OpenClaw {

struct CronJob {
  char id[40] = {};
  char name[64] = {};
  // The skill that declared this job ("heartbeat:main"), empty for the jobs a
  // conversation creates. The card shows user to-dos only, so this is what
  // separates the gateway's own routines from the ones the user asked for —
  // 48 B/job (8 jobs = 384 B DRAM) buys a schema rule instead of a name list.
  char declarationKey[48] = {};
  bool enabled = false;
  bool running = false;
  char schedule[40] = {};  // "every 1h" / cron expr / at
  char lastStatus[12] = {};
  // Epoch ms — 64-bit on purpose: a real ms timestamp (~1.8e12) never fits
  // a uint32 window, and the old `nextRun <= 1<<32` guard silently zeroed
  // every actual value (caught by test/cron_list). 0 = none.
  uint64_t nextRunAtMs = 0;
  // payload.text — the sentence the voice session wrote for this to-do
  // ("12:20了！该去接孩子放学了！"). The only human wording the gateway keeps:
  // `name` is a generated slug (`pick-up-child-oct03-1220`), which is why the
  // card went English when the old name dictionary went away. 64 B/job
  // (8 jobs = 512 B DRAM) covers every live sample with margin; longer text
  // is cut at capture and cut again for the card's width — one bounded buffer
  // instead of re-parsing a 6.5 KB frame on every 60 s repaint.
  char text[64] = {};
};

// What a cron.list reply frame turned out to be — the three cases the
// caller's cache policy distinguishes (Session::storeCronJobs).
enum class CronParse : uint8_t {
  Error,   // not JSON / truncated — caller invalidates its cache
  NoJobs,  // well-formed but no payload.jobs array — caller keeps what it has
  Ok,      // payload.jobs parsed (an empty list is legitimately Ok)
};

// Parses one complete `cron.list` reply into `out[0..cap)`. Streaming/SAX
// (StreamingJsonParser) rather than a DOM: the parse runs inside the
// WebSocket dispatch, where a heap document per reply is exactly what the
// rest of this file avoids. Fields are captured order-independently — the
// gateway's key order is not part of the contract.
//
// Returns NoJobs when the frame carries no payload.jobs array, Error when
// the document is malformed or cut short (the depth never returns to zero,
// the same completeness test OpenClawHandshake::parseFrame uses), else Ok
// with *outCount set (<= cap; jobs beyond cap are skipped). `out` may be
// null only when cap is 0.
//
// With userJobsOnly the gateway's own routines are dropped as they close
// (isSystemCronJob) instead of after the fact: the slot they would have
// occupied stays free, so a reply listing 8 routines still fills cap with
// the user's to-dos rather than filtering down to nothing.
//
// Pure C++ — no ESP-IDF, no Arduino — so test/cron_list compiles the same
// translation unit the firmware links.
CronParse parseCronList(const char* json, size_t len, CronJob* out, size_t cap, size_t* outCount,
                        bool userJobsOnly = false);

// The gateway's own scheduled routines — heartbeat, skill review, memory
// dreaming — which the to-do card must not show: they are the platform
// talking to itself, not something the user set. Two rules, because the
// gateway marks them two ways: a job a skill declared carries a
// declarationKey, and the ones it does not declare are named outright
// (checked against the live cron.list — `wake-up-oct03-830`, created by
// voice, has neither).
bool isSystemCronJob(const CronJob& job);

// The card's content column, shortened for a line that also has to fit
// `[ ]1.` and the due time: strips a leading time clause ("12:20了！" /
// "下午四点半了！"), the "该去" filler the gateway's phrasing adds, and a
// trailing "了！"/"！"/"。" — "12:20了！该去接孩子放学了！" reads as
// "接孩子放学". Everything else is kept verbatim: this is a fixed pattern
// match over the phrasing the automations tool is known to emit, not a
// Chinese grammar parser, so an unfamiliar sentence passes through whole.
// `text` may be null/empty; the result is always NUL-terminated.
void formatTodoTitle(char* dst, size_t cap, const char* text);

// Writes "9:00" / "12:20" for the due time and returns the meridiem word the
// caller passed for that hour — the day word ("今天"/"明天"), the clock and
// the meridiem are joined by the caller through STR_TODO_WHEN_FMT, because
// Chinese prints the period in front of the digits and English after them.
// hour24 is 0..23: 0..11 return `am`, 12..23 return `pm`, and the 12-hour
// number wraps 0 to 12 (midnight reads 12:05, noon 12:05). Never returns
// null; an out-of-range hour or minute is clamped rather than passed through.
const char* formatTodoClock(char* dst, size_t cap, int hour24, int minute, const char* am, const char* pm);

}  // namespace OpenClaw
