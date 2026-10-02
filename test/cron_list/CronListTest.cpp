#include <CronListFormat.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

using OpenClaw::CronJob;
using OpenClaw::CronParse;
using OpenClaw::formatTodoClock;
using OpenClaw::formatTodoTitle;
using OpenClaw::isSystemCronJob;
using OpenClaw::parseCronList;

namespace {

// A realistic `cron.list` reply frame: the gateway's res envelope with
// payload.jobs[] and the field paths the workbench card reads
// (mirrors the AIWatch reference parse, openclaw_client.c:1009-1070).
constexpr char kReply[] = R"({"type":"res","id":7,"ok":true,"payload":{"jobs":[)"
                          R"({"id":"job-1","name":"Morning alarm","enabled":true,)"
                          R"("schedule":{"kind":"every","everyMs":3600000},)"
                          R"("state":{"nextRunAtMs":1790400000000,"runningAtMs":0,"lastStatus":"ok"}})"
                          R"(,{"id":"job-2","name":"Nightly backup","enabled":false,)"
                          R"("schedule":{"kind":"cron","expr":"0 3 * * *"},)"
                          R"("state":{"nextRunAtMs":0,"runningAtMs":1790400000123,"lastStatus":"running"}})"
                          R"(]}})";

TEST(CronList, ParsesRealisticReply) {
  CronJob jobs[4];
  size_t count = 0;
  ASSERT_EQ(parseCronList(kReply, strlen(kReply), jobs, 4, &count), CronParse::Ok);
  ASSERT_EQ(count, 2u);

  EXPECT_STREQ(jobs[0].id, "job-1");
  EXPECT_STREQ(jobs[0].name, "Morning alarm");
  EXPECT_TRUE(jobs[0].enabled);
  EXPECT_FALSE(jobs[0].running);
  EXPECT_STREQ(jobs[0].schedule, "every 3600000ms");
  EXPECT_EQ(jobs[0].nextRunAtMs, 1790400000000ull);
  EXPECT_STREQ(jobs[0].lastStatus, "ok");

  EXPECT_STREQ(jobs[1].id, "job-2");
  EXPECT_FALSE(jobs[1].enabled);
  EXPECT_TRUE(jobs[1].running);
  EXPECT_STREQ(jobs[1].schedule, "0 3 * * *");
  EXPECT_EQ(jobs[1].nextRunAtMs, 0u);
  EXPECT_STREQ(jobs[1].lastStatus, "running");
}

TEST(CronList, AtKindUsesTheAtField) {
  const char* json = R"({"payload":{"jobs":[{"id":"a","name":"","enabled":true,)"
                     R"("schedule":{"kind":"at","at":"2026-10-02T08:00:00Z"},"state":{}}]}})";
  CronJob jobs[2];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), jobs, 2, &count), CronParse::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(jobs[0].schedule, "2026-10-02T08:00:00Z");
}

// The gateway's key order is not part of the contract: staging fields until
// their container closes is what makes every permutation parse the same.
TEST(CronList, FieldOrderInsideContainersDoesNotMatter) {
  const char* json = R"({"payload":{"jobs":[{"state":{"lastStatus":"idle","nextRunAtMs":5,)"
                     R"("runningAtMs":0},"schedule":{"everyMs":1000,"kind":"every"},)"
                     R"("name":"Reboot","enabled":true,"id":"x1"}]}})";
  CronJob jobs[1];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), jobs, 1, &count), CronParse::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(jobs[0].id, "x1");
  EXPECT_STREQ(jobs[0].name, "Reboot");
  EXPECT_TRUE(jobs[0].enabled);
  EXPECT_STREQ(jobs[0].schedule, "every 1000ms");
  EXPECT_EQ(jobs[0].nextRunAtMs, 5u);
}

TEST(CronList, WellFormedFrameWithoutJobsKeepsTheOldList) {
  const char* json = R"({"type":"event","event":"health","payload":{"uptimeMs":123}})";
  CronJob jobs[2];
  size_t count = 99;
  EXPECT_EQ(parseCronList(json, strlen(json), jobs, 2, &count), CronParse::NoJobs);
  EXPECT_EQ(count, 0u);  // outCount is zeroed; the *cache* policy (keep) is the caller's
}

TEST(CronList, GarbageIsAnError) {
  const char* json = "not json at all";
  size_t count = 5;
  EXPECT_EQ(parseCronList(json, strlen(json), nullptr, 0, &count), CronParse::Error);
  EXPECT_EQ(count, 0u);
  EXPECT_EQ(parseCronList(nullptr, 0, nullptr, 0, &count), CronParse::Error);
  EXPECT_EQ(parseCronList("", 0, nullptr, 0, &count), CronParse::Error);
}

// A frame cut mid-array must not look like an empty list: the depth never
// returns to zero, which is the completeness test the whole parse rests on.
TEST(CronList, TruncatedFrameIsAnError) {
  std::string full(kReply);
  for (size_t cut : {full.size() / 2, full.size() - 2, full.size() - 1}) {
    CronJob jobs[4];
    size_t count = 0;
    EXPECT_EQ(parseCronList(full.data(), cut, jobs, 4, &count), CronParse::Error) << "cut=" << cut;
    EXPECT_EQ(count, 0u);
  }
}

TEST(CronList, EmptyJobsListIsOk) {
  const char* json = R"({"payload":{"jobs":[]}})";
  CronJob jobs[2];
  size_t count = 7;
  EXPECT_EQ(parseCronList(json, strlen(json), jobs, 2, &count), CronParse::Ok);
  EXPECT_EQ(count, 0u);
}

TEST(CronList, CapsAtCapacityAndSkipsTheRest) {
  std::string json = R"({"payload":{"jobs":[)";
  for (int i = 0; i < 12; ++i) {
    if (i > 0) json += ",";
    // Build {"id":"job-i","name":"n","enabled":true} without a JSON lib.
    json += "{\"id\":\"job-" + std::to_string(i) + "\",\"name\":\"n\",\"enabled\":true}";
  }
  json += "]}}";
  CronJob jobs[8];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json.data(), json.size(), jobs, 8, &count), CronParse::Ok);
  EXPECT_EQ(count, 8u);  // 12 offered, capacity 8
  EXPECT_STREQ(jobs[7].id, "job-7");
}

// The name field is 64 bytes; a long value must truncate cleanly (the UI
// truncates again for width) and an over-long value that hits the parser's
// 512-byte token buffer must survive the chunked path without overflow.
TEST(CronList, LongNamesTruncateWithoutOverflow) {
  const std::string longName(600, 'x');
  const std::string json = R"({"payload":{"jobs":[{"id":"a","name":")" + longName + R"(","enabled":true}]}})";
  CronJob jobs[1];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json.data(), json.size(), jobs, 1, &count), CronParse::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(strlen(jobs[0].name), 63u);
  EXPECT_EQ(strspn(jobs[0].name, "x"), 63u);
}

// A real next run is epoch *milliseconds* (~1.8e12): it must survive
// verbatim as 64-bit. The old uint32 + `<= 1<<32` guard zeroed every actual
// value — this pins the fix. 0 stays "none", a negative stamp is nonsense.
TEST(CronList, NextRunIsKeptVerbatimAsSixtyFourBitEpochMs) {
  const char* json = R"({"payload":{"jobs":[{"id":"a","enabled":true,)"
                     R"("state":{"nextRunAtMs":9007199254740991,"runningAtMs":-5}}]}})";
  CronJob jobs[1];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), jobs, 1, &count), CronParse::Ok);
  EXPECT_EQ(jobs[0].nextRunAtMs, 9007199254740991ull);
  EXPECT_FALSE(jobs[0].running);  // negative runningAtMs is not "running"
}

TEST(CronList, MissingNameLeavesItEmptyForTheUiIdFallback) {
  const char* json = R"({"payload":{"jobs":[{"id":"only-id","enabled":false}]}})";
  CronJob jobs[1];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), jobs, 1, &count), CronParse::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(jobs[0].id, "only-id");
  EXPECT_STREQ(jobs[0].name, "");
}

TEST(CronList, Utf8NamesSurviveByteForByte) {
  const char* json = R"({"payload":{"jobs":[{"id":"a","name":"早八点喝水提醒","enabled":true}]}})";
  CronJob jobs[1];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), jobs, 1, &count), CronParse::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(jobs[0].name, "早八点喝水提醒");
}

// The sniff that routes frames here looks for `"jobs"` anywhere; a chat
// frame whose *text* mentions jobs must not invalidate the cache — it has
// no payload.jobs array, so the verdict is NoJobs, not Error.
TEST(CronList, ChatFrameMentioningJobsIsNotAnError) {
  const char* json = R"({"type":"event","event":"chat","payload":{"state":"delta",)"
                     R"("message":{"content":[{"type":"text","text":"the \"jobs\" are fine"}]}}})";
  size_t count = 3;
  EXPECT_EQ(parseCronList(json, strlen(json), nullptr, 0, &count), CronParse::NoJobs);
  EXPECT_EQ(count, 0u);
}

// The card shows the user's to-dos, not the platform's own schedule. The
// gateway marks a skill-declared job with declarationKey, and names the two
// routines it does not declare — both halves are pinned here because getting
// this backwards either floods the card with 心跳 or hides the user's own
// entries, and neither shows up in a compile.
TEST(CronList, DeclarationKeyIsCaptured) {
  const char* json = R"({"payload":{"jobs":[{"id":"a","name":"heartbeat-main",)"
                     R"("declarationKey":"heartbeat:main","enabled":true}]}})";
  CronJob jobs[1];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), jobs, 1, &count), CronParse::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(jobs[0].declarationKey, "heartbeat:main");
}

TEST(CronList, IsSystemCronJobMatchesBothWaysTheGatewayMarksThem) {
  CronJob declared{};
  snprintf(declared.name, sizeof(declared.name), "wake-up-oct03-830");
  snprintf(declared.declarationKey, sizeof(declared.declarationKey), "heartbeat:main");
  EXPECT_TRUE(isSystemCronJob(declared));

  CronJob byName{};
  snprintf(byName.name, sizeof(byName.name), "Memory Dreaming Promotion");
  EXPECT_TRUE(isSystemCronJob(byName));
  snprintf(byName.name, sizeof(byName.name), "skill-collection-review-main");
  EXPECT_TRUE(isSystemCronJob(byName));

  CronJob mine{};
  snprintf(mine.name, sizeof(mine.name), "wake-up-oct03-830");
  EXPECT_FALSE(isSystemCronJob(mine));
}

TEST(CronList, UserJobsOnlyKeepsWhatTheUserCreated) {
  const char* json = R"({"payload":{"jobs":[)"
                     R"({"id":"1","name":"heartbeat-main","declarationKey":"heartbeat:main","enabled":true},)"
                     R"({"id":"2","name":"Memory Dreaming Promotion","enabled":true},)"
                     R"({"id":"3","name":"wake-up-oct03-830","enabled":true}]}})";
  CronJob all[4];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), all, 4, &count), CronParse::Ok);
  EXPECT_EQ(count, 3u);  // the plain parse is faithful — no filtering

  CronJob mine[4];
  count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), mine, 4, &count, /*userJobsOnly=*/true), CronParse::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(mine[0].id, "3");
  EXPECT_STREQ(mine[0].name, "wake-up-oct03-830");
}

// Filtering after the fact would spend the cap on the routines it then
// throws away; dropping them as they close hands the slot to the next job.
TEST(CronList, UserJobsOnlyReusesTheSlotOfADroppedRoutine) {
  const char* json = R"({"payload":{"jobs":[)"
                     R"({"id":"1","name":"heartbeat-main","declarationKey":"heartbeat:main","enabled":true},)"
                     R"({"id":"2","name":"skill-collection-review-main","enabled":true},)"
                     R"({"id":"3","name":"mine","enabled":true}]}})";
  CronJob jobs[1];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), jobs, 1, &count, true), CronParse::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(jobs[0].id, "3");
}

// payload.text is the card's content column: the only human wording the
// gateway keeps for a job a conversation created (`name` is a generated slug).
TEST(CronList, CapturesJobPayloadText) {
  const char* json = R"({"payload":{"jobs":[)"
                     R"({"id":"a","name":"pick-up-child-oct03-1220","enabled":true,)"
                     R"("payload":{"kind":"systemEvent","text":"12:20了！该去接孩子放学了！"}}]}})";
  CronJob jobs[1];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), jobs, 1, &count), CronParse::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(jobs[0].text, "12:20了！该去接孩子放学了！");
}

// The envelope's "payload" (which wraps jobs[]) and a job's own "payload"
// ({kind,text}) are different containers — telling them apart by parent is
// what keeps the root frame parsing after job payloads are captured.
TEST(CronList, JobPayloadIsNotTheEnvelopePayload) {
  const char* json = R"({"type":"res","id":"7","payload":{"jobs":[)"
                     R"({"id":"a","payload":{"kind":"systemEvent","text":"该喝水了！"},)"
                     R"("state":{"nextRunAtMs":5}}]}})";
  CronJob jobs[1];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), jobs, 1, &count), CronParse::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(jobs[0].text, "该喝水了！");
  EXPECT_EQ(jobs[0].nextRunAtMs, 5u);
}

TEST(CronList, JobWithoutPayloadLeavesTextEmpty) {
  const char* json = R"({"payload":{"jobs":[{"id":"h","name":"heartbeat-main","enabled":true}]}})";
  CronJob jobs[1];
  size_t count = 0;
  ASSERT_EQ(parseCronList(json, strlen(json), jobs, 1, &count), CronParse::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(jobs[0].text, "");
}

TEST(CronList, TodoTitleShortensTheKnownPhrasing) {
  char buf[96];
  formatTodoTitle(buf, sizeof(buf), "12:20了！该去接孩子放学了！");
  EXPECT_STREQ(buf, "接孩子放学");
  formatTodoTitle(buf, sizeof(buf), "下午四点半了！该去上课了！");
  EXPECT_STREQ(buf, "上课");
  formatTodoTitle(buf, sizeof(buf), "闹钟响了！该起床啦，假期第三天，早安！");
  EXPECT_STREQ(buf, "闹钟响了！该起床啦，假期第三天，早安");
  // Nothing recognisable: passed through untouched.
  formatTodoTitle(buf, sizeof(buf), "去机场接孩子");
  EXPECT_STREQ(buf, "去机场接孩子");
  // A digit run that is not a clock ("2026年…") keeps its prefix — only the
  // trailing sentence particle goes.
  formatTodoTitle(buf, sizeof(buf), "2026年会费到期了！");
  EXPECT_STREQ(buf, "2026年会费到期");
}

TEST(CronList, TodoTitleHandlesEmptyAndBoundsTheCopy) {
  char buf[96];
  formatTodoTitle(buf, sizeof(buf), nullptr);
  EXPECT_STREQ(buf, "");
  formatTodoTitle(buf, sizeof(buf), "");
  EXPECT_STREQ(buf, "");

  char small[12];
  formatTodoTitle(small, sizeof(small), "这是一段很长的提醒内容");
  EXPECT_EQ(strlen(small), 11u);  // bounded, always NUL-terminated
}

TEST(CronList, TodoTitleToleratesPunctuationVariants) {
  char buf[96];
  // Full-width colon — voice transcripts turn "12:20" into "12：20".
  formatTodoTitle(buf, sizeof(buf), "12：20了！该去接孩子放学了！");
  EXPECT_STREQ(buf, "接孩子放学");
  // Leading ASCII / ideographic space in front of the clock.
  formatTodoTitle(buf, sizeof(buf), " 12:20了！该去接孩子放学了！");
  EXPECT_STREQ(buf, "接孩子放学");
  // Concatenated literal: a hex escape would otherwise swallow the "12".
  formatTodoTitle(buf, sizeof(buf),
                  "\xE3\x80\x80"
                  "12:20了！该去接孩子放学了！");
  EXPECT_STREQ(buf, "接孩子放学");
  // Space between the digits and 了.
  formatTodoTitle(buf, sizeof(buf), "12:20 了！该去接孩子放学了！");
  EXPECT_STREQ(buf, "接孩子放学");
}

TEST(CronList, TodoTitleDropsTheLeadingSticker) {
  char buf[96];
  // Live payloads open with an emoji and a space ("⏰ 12:20了！…"): the emoji
  // has no glyph in the UI faces and the row starts at the words.
  formatTodoTitle(buf, sizeof(buf), "⏰ 12:20了！该去接孩子放学了！");
  EXPECT_STREQ(buf, "接孩子放学");
  formatTodoTitle(buf, sizeof(buf), "📚 下午四点半了！该去上课了！");
  EXPECT_STREQ(buf, "上课");
  formatTodoTitle(buf, sizeof(buf), "🎬 晚上六点半了！该去看电影啦！");
  EXPECT_STREQ(buf, "看电影啦");
  // One inside the title is not decoration and stays.
  formatTodoTitle(buf, sizeof(buf), "带⏰的提醒");
  EXPECT_STREQ(buf, "带⏰的提醒");
}

TEST(CronList, TodoClockUsesTheTwelveHourClock) {
  char buf[16];
  EXPECT_STREQ(formatTodoClock(buf, sizeof(buf), 9, 0, "上午", "下午"), "上午");
  EXPECT_STREQ(buf, "9:00");
  EXPECT_STREQ(formatTodoClock(buf, sizeof(buf), 12, 20, "AM", "PM"), "PM");
  EXPECT_STREQ(buf, "12:20");
  EXPECT_STREQ(formatTodoClock(buf, sizeof(buf), 0, 5, "AM", "PM"), "AM");
  EXPECT_STREQ(buf, "12:05");  // midnight is 12, not 0
  EXPECT_STREQ(formatTodoClock(buf, sizeof(buf), 16, 30, "AM", "PM"), "PM");
  EXPECT_STREQ(buf, "4:30");
  EXPECT_STREQ(formatTodoClock(buf, sizeof(buf), 11, 59, "AM", "PM"), "AM");
  EXPECT_STREQ(buf, "11:59");
}

}  // namespace
