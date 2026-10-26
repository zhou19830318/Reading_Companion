#include <gtest/gtest.h>

#include <cstring>

#include "VoiceRelayCodec.h"

// /api/v1/learning/words wire contract (relay/relay/main.py
// learning_add_words): {"words":[...],"source":"..."} in,
// {"ok":true,"accepted":N,"added":N} or {"error":...} out.

namespace {

TEST(LearningCodecBuild, SingleWordMatchesServerContract) {
  const char* words[] = {"abandon"};
  char out[256];
  const size_t n = VoiceRelay::buildAddWordsRequest(out, sizeof(out), words, 1);
  ASSERT_GT(n, 0u);
  EXPECT_STREQ(R"({"words":["abandon"],"source":"lookup"})", out);
  EXPECT_EQ(std::strlen(out), n);
}

TEST(LearningCodecBuild, BatchJoinsAndEscapes) {
  const char* words[] = {"a\"b", "c\\d", "plain"};
  char out[256];
  const size_t n = VoiceRelay::buildAddWordsRequest(out, sizeof(out), words, 3, "word");
  ASSERT_GT(n, 0u);
  EXPECT_STREQ(R"({"words":["a\"b","c\\d","plain"],"source":"word"})", out);
}

TEST(LearningCodecBuild, RejectsInvalidInput) {
  char out[256];
  const char* ok[] = {"word"};
  const char* nullEntry[] = {"word", nullptr};
  const char* emptyEntry[] = {""};
  EXPECT_EQ(0u, VoiceRelay::buildAddWordsRequest(nullptr, sizeof(out), ok, 1));
  EXPECT_EQ(0u, VoiceRelay::buildAddWordsRequest(out, sizeof(out), nullptr, 1));
  EXPECT_EQ(0u, VoiceRelay::buildAddWordsRequest(out, sizeof(out), ok, 0));
  EXPECT_EQ(0u, VoiceRelay::buildAddWordsRequest(out, sizeof(out), nullEntry, 2));
  EXPECT_EQ(0u, VoiceRelay::buildAddWordsRequest(out, sizeof(out), emptyEntry, 1));
  // A frame that does not fit must fail whole, never emit a partial body.
  EXPECT_EQ(0u, VoiceRelay::buildAddWordsRequest(out, 20, ok, 1));
}

TEST(LearningCodecParse, SuccessBody) {
  const char* body = R"({"ok":true,"accepted":3,"added":2})";
  VoiceRelay::AddWordsResult out;
  ASSERT_TRUE(VoiceRelay::parseAddWordsResponse(body, std::strlen(body), out));
  EXPECT_TRUE(out.ok);
  EXPECT_EQ(3, out.accepted);
  EXPECT_EQ(2, out.added);
  EXPECT_EQ(VoiceRelay::ErrorCode::None, out.error);
}

TEST(LearningCodecParse, EmptyBatchEchoIsStillSuccess) {
  const char* body = R"({"ok":true,"accepted":0,"added":0})";
  VoiceRelay::AddWordsResult out;
  ASSERT_TRUE(VoiceRelay::parseAddWordsResponse(body, std::strlen(body), out));
  EXPECT_TRUE(out.ok);
  EXPECT_EQ(0, out.accepted);
  EXPECT_EQ(0, out.added);
}

TEST(LearningCodecParse, ErrorBodyCarriesCode) {
  const char* body = R"({"error":"UNAUTHORIZED"})";
  VoiceRelay::AddWordsResult out;
  EXPECT_FALSE(VoiceRelay::parseAddWordsResponse(body, std::strlen(body), out));
  EXPECT_EQ(VoiceRelay::ErrorCode::Unauthorized, out.error);
  EXPECT_FALSE(out.ok);
  EXPECT_EQ(-1, out.accepted);
}

TEST(LearningCodecParse, GarbageBodiesFail) {
  VoiceRelay::AddWordsResult out;
  EXPECT_FALSE(VoiceRelay::parseAddWordsResponse(nullptr, 0, out));
  const char* notJson = "hello";
  EXPECT_FALSE(VoiceRelay::parseAddWordsResponse(notJson, std::strlen(notJson), out));
}

TEST(LearningCodecContract, DeviceChunkFitsTheServerCap) {
  static_assert(VoiceRelay::LEARNING_CHUNK_WORDS > 0, "chunk must be non-empty");
  static_assert(VoiceRelay::LEARNING_CHUNK_WORDS <= VoiceRelay::LEARNING_MAX_WORDS_PER_REQUEST,
                "one chunk must never exceed the relay batch cap");
}

// ── review endpoint (今日复习) ────────────────────────────────────────────

const char* kQueueBody =
    R"({"count":2,"due":5,"words":[)"
    R"({"word":"abandon","addedAt":"2026-10-20T08:00:00+00:00","source":"lookup","lastRating":null,"lastReviewedAt":null,"dueAt":"2026-10-25T08:00:00+00:00"},)"
    R"({"word":"candid","addedAt":"2026-10-21T08:00:00+00:00","source":"review","lastRating":1,"lastReviewedAt":"2026-10-25T07:00:00+00:00","dueAt":"2026-10-25T08:00:00+00:00"}]})";

TEST(LearningReviewBuild, RequestMatchesServerContract) {
  char out[256];
  const size_t n =
      VoiceRelay::buildReviewRequest(out, sizeof(out), "abandon", 3, "0123456789abcdef0123456789abcdef");
  ASSERT_GT(n, 0u);
  EXPECT_STREQ(R"({"word":"abandon","rating":3,"clientEventId":"0123456789abcdef0123456789abcdef"})", out);
  EXPECT_EQ(std::strlen(out), n);
}

TEST(LearningReviewBuild, EscapesWordAndRejectsBadInput) {
  char out[256];
  ASSERT_GT(VoiceRelay::buildReviewRequest(out, sizeof(out), "a\"b", 1, "id"), 0u);
  EXPECT_STREQ(R"({"word":"a\"b","rating":1,"clientEventId":"id"})", out);

  EXPECT_EQ(0u, VoiceRelay::buildReviewRequest(nullptr, sizeof(out), "word", 1, "id"));
  EXPECT_EQ(0u, VoiceRelay::buildReviewRequest(out, sizeof(out), "", 1, "id"));
  EXPECT_EQ(0u, VoiceRelay::buildReviewRequest(out, sizeof(out), "word", 0, "id"));
  EXPECT_EQ(0u, VoiceRelay::buildReviewRequest(out, sizeof(out), "word", 5, "id"));
  EXPECT_EQ(0u, VoiceRelay::buildReviewRequest(out, sizeof(out), "word", 1, ""));
  EXPECT_EQ(0u, VoiceRelay::buildReviewRequest(out, 20, "word", 1, "id"));
}

TEST(LearningReviewParse, QueueEnvelopeAndItems) {
  VoiceRelay::ReviewItem items[8];
  size_t stored = 0;
  VoiceRelay::ReviewQueueResult result;
  ASSERT_TRUE(VoiceRelay::parseReviewQueueResponse(kQueueBody, std::strlen(kQueueBody), items, 8, stored, result));
  EXPECT_EQ(2u, stored);
  EXPECT_EQ(2, result.count);
  EXPECT_EQ(5, result.due);
  EXPECT_STREQ("abandon", items[0].word);
  EXPECT_EQ(-1, items[0].lastRating);
  EXPECT_STREQ("candid", items[1].word);
  EXPECT_EQ(1, items[1].lastRating);
}

TEST(LearningReviewParse, QueueStopsAtCapacityButStillReadsCounts) {
  VoiceRelay::ReviewItem items[1];
  size_t stored = 0;
  VoiceRelay::ReviewQueueResult result;
  ASSERT_TRUE(VoiceRelay::parseReviewQueueResponse(kQueueBody, std::strlen(kQueueBody), items, 1, stored, result));
  EXPECT_EQ(1u, stored);
  EXPECT_STREQ("abandon", items[0].word);
  EXPECT_EQ(2, result.count);
  EXPECT_EQ(5, result.due);
}

TEST(LearningReviewParse, EmptyQueueIsValid) {
  const char* body = R"({"count":0,"due":0,"words":[]})";
  VoiceRelay::ReviewItem items[4];
  size_t stored = 1;
  VoiceRelay::ReviewQueueResult result;
  ASSERT_TRUE(VoiceRelay::parseReviewQueueResponse(body, std::strlen(body), items, 4, stored, result));
  EXPECT_EQ(0u, stored);
  EXPECT_EQ(0, result.count);
  EXPECT_EQ(0, result.due);
}

TEST(LearningReviewParse, QueueErrorAndGarbageFail) {
  const char* errorBody = R"({"error":"UNAUTHORIZED"})";
  VoiceRelay::ReviewItem items[4];
  size_t stored = 0;
  VoiceRelay::ReviewQueueResult result;
  EXPECT_FALSE(
      VoiceRelay::parseReviewQueueResponse(errorBody, std::strlen(errorBody), items, 4, stored, result));
  EXPECT_EQ(0u, stored);
  EXPECT_EQ(-1, result.due);

  EXPECT_FALSE(VoiceRelay::parseReviewQueueResponse(nullptr, 0, items, 4, stored, result));
  const char* notJson = "hello";
  EXPECT_FALSE(VoiceRelay::parseReviewQueueResponse(notJson, std::strlen(notJson), items, 4, stored, result));
}

TEST(LearningReviewParse, SubmitEchoAndDuplicate) {
  VoiceRelay::ReviewSubmitResult out;
  const char* first = R"({"ok":true,"duplicate":false})";
  ASSERT_TRUE(VoiceRelay::parseReviewResponse(first, std::strlen(first), out));
  EXPECT_TRUE(out.ok);
  EXPECT_FALSE(out.duplicate);

  const char* retry = R"({"ok":true,"duplicate":true})";
  ASSERT_TRUE(VoiceRelay::parseReviewResponse(retry, std::strlen(retry), out));
  EXPECT_TRUE(out.ok);
  EXPECT_TRUE(out.duplicate);

  const char* bad = R"({"error":"invalid_review"})";
  EXPECT_FALSE(VoiceRelay::parseReviewResponse(bad, std::strlen(bad), out));
  EXPECT_EQ(VoiceRelay::ErrorCode::Unknown, out.error);  // unrecognised token, 400 still maps to BadConfig
}

TEST(LearningReviewParse, SubmitErrorCarriesAuthToken) {
  VoiceRelay::ReviewSubmitResult out;
  const char* body = R"({"error":"DEVICE_NOT_ALLOWED"})";
  EXPECT_FALSE(VoiceRelay::parseReviewResponse(body, std::strlen(body), out));
  EXPECT_EQ(VoiceRelay::ErrorCode::DeviceNotAllowed, out.error);
}

}  // namespace
