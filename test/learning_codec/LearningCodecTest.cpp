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

}  // namespace
