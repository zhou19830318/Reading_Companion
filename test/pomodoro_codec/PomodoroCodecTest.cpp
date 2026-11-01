#include <gtest/gtest.h>

#include <cstring>

#include "VoiceRelayCodec.h"

// /api/v1/pomodoro/sessions wire contract (relay/relay/main.py
// pomodoro_sessions): {"sessions":[...]} in, {"ok":true,"accepted":N,
// "stored":N} or {"error":...} out. The relay validates id format, kind,
// timestamps and minutes again, so the builder must never emit a frame the
// server would 422 on.

namespace {

constexpr const char* ID1 = "0123456789abcdef0123456789abcdef";
constexpr const char* ID2 = "fedcba9876543210fedcba9876543210";

VoiceRelay::PomodoroSessionWire makeSession(const char* id, const char* kind = "work", uint16_t minutes = 25,
                                            bool completed = true) {
  VoiceRelay::PomodoroSessionWire session;
  session.clientEventId = id;
  session.kind = kind;
  session.startedAt = "2026-10-30T09:00:00";
  session.endedAt = "2026-10-30T09:25:00";
  session.minutes = minutes;
  session.completed = completed;
  return session;
}

TEST(PomodoroCodecBuild, SingleSessionMatchesServerContract) {
  const VoiceRelay::PomodoroSessionWire sessions[] = {makeSession(ID1)};
  char out[512];
  const size_t n = VoiceRelay::buildPomodoroSessionsRequest(out, sizeof(out), sessions, 1);
  ASSERT_GT(n, 0u);
  EXPECT_STREQ(
      R"({"sessions":[{"clientEventId":"0123456789abcdef0123456789abcdef","kind":"work","startedAt":"2026-10-30T09:00:00","endedAt":"2026-10-30T09:25:00","minutes":25,"completed":true}]})",
      out);
  EXPECT_EQ(std::strlen(out), n);
}

TEST(PomodoroCodecBuild, BatchKeepsOrderAndPartialCompletion) {
  const VoiceRelay::PomodoroSessionWire sessions[] = {makeSession(ID1, "work", 45),
                                                      makeSession(ID2, "long_break", 15, false)};
  char out[1024];
  const size_t n = VoiceRelay::buildPomodoroSessionsRequest(out, sizeof(out), sessions, 2);
  ASSERT_GT(n, 0u);
  EXPECT_STREQ(
      R"({"sessions":[{"clientEventId":"0123456789abcdef0123456789abcdef","kind":"work","startedAt":"2026-10-30T09:00:00","endedAt":"2026-10-30T09:25:00","minutes":45,"completed":true},{"clientEventId":"fedcba9876543210fedcba9876543210","kind":"long_break","startedAt":"2026-10-30T09:00:00","endedAt":"2026-10-30T09:25:00","minutes":15,"completed":false}]})",
      out);
}

TEST(PomodoroCodecBuild, AcceptsEveryValidKind) {
  const char* kinds[] = {"work", "break", "long_break"};
  char out[512];
  for (const char* kind : kinds) {
    const VoiceRelay::PomodoroSessionWire sessions[] = {makeSession(ID1, kind)};
    EXPECT_GT(VoiceRelay::buildPomodoroSessionsRequest(out, sizeof(out), sessions, 1), 0u) << kind;
  }
}

TEST(PomodoroCodecBuild, RejectsInvalidEventId) {
  char out[512];
  const char* bad[] = {
      "",                                   // empty
      "0123456789abcdef0123456789abcde",    // 31 chars
      "0123456789abcdef0123456789abcdef0",  // 33 chars
      "0123456789ABCDEF0123456789ABCDEF",   // uppercase
      "0123456789abcdef0123456789abcdeg",   // non-hex
  };
  for (const char* id : bad) {
    const VoiceRelay::PomodoroSessionWire sessions[] = {makeSession(id)};
    EXPECT_EQ(0u, VoiceRelay::buildPomodoroSessionsRequest(out, sizeof(out), sessions, 1)) << id;
  }
  const VoiceRelay::PomodoroSessionWire nullId[] = {makeSession(nullptr)};
  EXPECT_EQ(0u, VoiceRelay::buildPomodoroSessionsRequest(out, sizeof(out), nullId, 1));
}

TEST(PomodoroCodecBuild, RejectsInvalidFields) {
  char out[512];
  const char* badKinds[] = {"", "WORK", "rest", "long break"};
  for (const char* kind : badKinds) {
    const VoiceRelay::PomodoroSessionWire sessions[] = {makeSession(ID1, kind)};
    EXPECT_EQ(0u, VoiceRelay::buildPomodoroSessionsRequest(out, sizeof(out), sessions, 1)) << kind;
  }
  const uint16_t badMinutes[] = {0, 181, 65535};
  for (const uint16_t minutes : badMinutes) {
    const VoiceRelay::PomodoroSessionWire sessions[] = {makeSession(ID1, "work", minutes)};
    EXPECT_EQ(0u, VoiceRelay::buildPomodoroSessionsRequest(out, sizeof(out), sessions, 1)) << minutes;
  }
  VoiceRelay::PomodoroSessionWire noStart = makeSession(ID1);
  noStart.startedAt = "";
  EXPECT_EQ(0u, VoiceRelay::buildPomodoroSessionsRequest(out, sizeof(out), &noStart, 1));
  VoiceRelay::PomodoroSessionWire noEnd = makeSession(ID1);
  noEnd.endedAt = nullptr;
  EXPECT_EQ(0u, VoiceRelay::buildPomodoroSessionsRequest(out, sizeof(out), &noEnd, 1));
}

TEST(PomodoroCodecBuild, RejectsBadBatchAndSmallBuffer) {
  char out[512];
  const VoiceRelay::PomodoroSessionWire one[] = {makeSession(ID1)};
  EXPECT_EQ(0u, VoiceRelay::buildPomodoroSessionsRequest(nullptr, sizeof(out), one, 1));
  EXPECT_EQ(0u, VoiceRelay::buildPomodoroSessionsRequest(out, sizeof(out), nullptr, 1));
  EXPECT_EQ(0u, VoiceRelay::buildPomodoroSessionsRequest(out, sizeof(out), one, 0));

  VoiceRelay::PomodoroSessionWire many[VoiceRelay::POMODORO_MAX_SESSION_BATCH + 1];
  for (auto& session : many) session = makeSession(ID1);
  char big[8192];
  EXPECT_EQ(
      0u, VoiceRelay::buildPomodoroSessionsRequest(big, sizeof(big), many, VoiceRelay::POMODORO_MAX_SESSION_BATCH + 1));
  EXPECT_GT(VoiceRelay::buildPomodoroSessionsRequest(big, sizeof(big), many, VoiceRelay::POMODORO_MAX_SESSION_BATCH),
            0u);

  // A frame that does not fit fails whole — never a partial body.
  EXPECT_EQ(0u, VoiceRelay::buildPomodoroSessionsRequest(out, 16, one, 1));
}

TEST(PomodoroCodecParse, OkBodyReportsAccepted) {
  VoiceRelay::PomodoroSyncResult result;
  const char* body = R"({"ok":true,"accepted":3,"stored":2})";
  ASSERT_TRUE(VoiceRelay::parsePomodoroSessionsResponse(body, std::strlen(body), result));
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(3, result.accepted);
  EXPECT_EQ(VoiceRelay::ErrorCode::None, result.error);
}

TEST(PomodoroCodecParse, OkWithoutAcceptedDoesNotBlockAck) {
  VoiceRelay::PomodoroSyncResult result;
  const char* body = R"({"ok":true})";
  ASSERT_TRUE(VoiceRelay::parsePomodoroSessionsResponse(body, std::strlen(body), result));
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(-1, result.accepted);
}

TEST(PomodoroCodecParse, ErrorBodySurfacesToken) {
  VoiceRelay::PomodoroSyncResult result;
  const char* body = R"({"error":"UNAUTHORIZED"})";
  ASSERT_FALSE(VoiceRelay::parsePomodoroSessionsResponse(body, std::strlen(body), result));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(VoiceRelay::ErrorCode::Unauthorized, result.error);

  const char* unknown = R"({"error":"invalid_session"})";
  ASSERT_FALSE(VoiceRelay::parsePomodoroSessionsResponse(unknown, std::strlen(unknown), result));
  EXPECT_EQ(VoiceRelay::ErrorCode::Unknown, result.error);
}

TEST(PomodoroCodecParse, OkFalseIsFailure) {
  VoiceRelay::PomodoroSyncResult result;
  const char* body = R"({"ok":false,"error":"SERVER_OVERLOADED"})";
  ASSERT_TRUE(VoiceRelay::parsePomodoroSessionsResponse(body, std::strlen(body), result));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(VoiceRelay::ErrorCode::ServerOverloaded, result.error);
}

TEST(PomodoroCodecParse, GarbageIsFailure) {
  VoiceRelay::PomodoroSyncResult result;
  const char* body = R"({"sessions":[]})";
  EXPECT_FALSE(VoiceRelay::parsePomodoroSessionsResponse(body, std::strlen(body), result));
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(VoiceRelay::parsePomodoroSessionsResponse(nullptr, 0, result));
  EXPECT_FALSE(VoiceRelay::parsePomodoroSessionsResponse("", 0, result));
}

}  // namespace
