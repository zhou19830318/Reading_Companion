#include <gtest/gtest.h>

#include <cstring>

#include "ReviewFormat.h"

// SD review-store line formats (百词斩 M2):
//   RVW1 <rating> <clientEventId> <word>\n   pending review events
//   RVS1 <due> <checkedAt>\n                 cached queue size for the home menu

namespace {

using Learning::ReviewFormat::Event;

TEST(ReviewEventFormat, RoundTrip) {
  Event in;
  in.rating = 3;
  std::strcpy(in.clientEventId, "0123456789abcdef0123456789abcdef");
  std::strcpy(in.word, "abandon");

  char line[128];
  const size_t n = Learning::ReviewFormat::buildEventLine(line, sizeof(line), in);
  ASSERT_GT(n, 0u);
  EXPECT_EQ(std::strlen(line), n);
  EXPECT_STREQ("RVW1 3 0123456789abcdef0123456789abcdef abandon\n", line);

  Event out;
  ASSERT_TRUE(Learning::ReviewFormat::parseEventLine(line, n, out));
  EXPECT_EQ(3, out.rating);
  EXPECT_STREQ(in.clientEventId, out.clientEventId);
  EXPECT_STREQ(in.word, out.word);
}

TEST(ReviewEventFormat, AllRatingsAccepted) {
  for (uint8_t rating = 1; rating <= 4; ++rating) {
    Event in;
    in.rating = rating;
    std::strcpy(in.clientEventId, "abc123");
    std::strcpy(in.word, "candid");
    char line[128];
    ASSERT_GT(Learning::ReviewFormat::buildEventLine(line, sizeof(line), in), 0u) << static_cast<int>(rating);
    Event out;
    ASSERT_TRUE(Learning::ReviewFormat::parseEventLine(line, std::strlen(line), out));
    EXPECT_EQ(rating, out.rating);
  }
}

TEST(ReviewEventFormat, BuildRejectsMalformedEvents) {
  char line[128];
  Event e;
  std::strcpy(e.clientEventId, "abc123");
  std::strcpy(e.word, "word");

  e.rating = 0;
  EXPECT_EQ(0u, Learning::ReviewFormat::buildEventLine(line, sizeof(line), e));
  e.rating = 5;
  EXPECT_EQ(0u, Learning::ReviewFormat::buildEventLine(line, sizeof(line), e));
  e.rating = 2;

  std::strcpy(e.clientEventId, "ABC123");
  EXPECT_EQ(0u, Learning::ReviewFormat::buildEventLine(line, sizeof(line), e));
  std::strcpy(e.clientEventId, "");
  EXPECT_EQ(0u, Learning::ReviewFormat::buildEventLine(line, sizeof(line), e));
  std::strcpy(e.clientEventId, "abc123");

  std::strcpy(e.word, "two words");
  EXPECT_EQ(0u, Learning::ReviewFormat::buildEventLine(line, sizeof(line), e));
  std::strcpy(e.word, "");
  EXPECT_EQ(0u, Learning::ReviewFormat::buildEventLine(line, sizeof(line), e));

  std::strcpy(e.word, "word");
  EXPECT_EQ(0u, Learning::ReviewFormat::buildEventLine(nullptr, sizeof(line), e));
  EXPECT_EQ(0u, Learning::ReviewFormat::buildEventLine(line, 10, e));  // does not fit
}

TEST(ReviewEventFormat, ParseRejectsTornAndForeignLines) {
  Event out;
  const char* noNewline = "RVW1 3 abc123 word";
  EXPECT_FALSE(Learning::ReviewFormat::parseEventLine(noNewline, std::strlen(noNewline), out));

  const char* foreign = "WBT1 17 p word\n";
  EXPECT_FALSE(Learning::ReviewFormat::parseEventLine(foreign, std::strlen(foreign), out));

  const char* badRating = "RVW1 9 abc123 word\n";
  EXPECT_FALSE(Learning::ReviewFormat::parseEventLine(badRating, std::strlen(badRating), out));

  const char* badWord = "RVW1 3 abc123 two words\n";
  EXPECT_FALSE(Learning::ReviewFormat::parseEventLine(badWord, std::strlen(badWord), out));

  const char* badId = "RVW1 3 XYZ word\n";
  EXPECT_FALSE(Learning::ReviewFormat::parseEventLine(badId, std::strlen(badId), out));

  // An overlong line arrives as the whole 128-byte buffer with len == cap.
  char overlong[128];
  std::memset(overlong, 'a', sizeof(overlong));
  EXPECT_FALSE(Learning::ReviewFormat::parseEventLine(overlong, sizeof(overlong), out));
}

TEST(ReviewStateFormat, RoundTrip) {
  char line[64];
  const size_t n = Learning::ReviewFormat::buildStateLine(line, sizeof(line), 12, 1761384000);
  ASSERT_GT(n, 0u);
  EXPECT_STREQ("RVS1 12 1761384000\n", line);

  int32_t due = -1;
  uint32_t checkedAt = 0;
  ASSERT_TRUE(Learning::ReviewFormat::parseStateLine(line, n, due, checkedAt));
  EXPECT_EQ(12, due);
  EXPECT_EQ(1761384000u, checkedAt);
}

TEST(ReviewStateFormat, RejectsMalformedState) {
  char line[64];
  int32_t due = 0;
  uint32_t checkedAt = 0;
  EXPECT_EQ(0u, Learning::ReviewFormat::buildStateLine(line, sizeof(line), -1, 0));

  const char* negative = "RVS1 -1 0\n";
  EXPECT_FALSE(Learning::ReviewFormat::parseStateLine(negative, std::strlen(negative), due, checkedAt));

  const char* missingAt = "RVS1 3\n";
  EXPECT_FALSE(Learning::ReviewFormat::parseStateLine(missingAt, std::strlen(missingAt), due, checkedAt));

  const char* trailing = "RVS1 3 4 5\n";
  EXPECT_FALSE(Learning::ReviewFormat::parseStateLine(trailing, std::strlen(trailing), due, checkedAt));

  const char* noNewline = "RVS1 3 4";
  EXPECT_FALSE(Learning::ReviewFormat::parseStateLine(noNewline, std::strlen(noNewline), due, checkedAt));
}

TEST(ReviewQueueFormat, RoundTrip) {
  Learning::ReviewFormat::QueueEntry in;
  in.lastRating = 1;
  std::strcpy(in.word, "candid");
  char line[128];
  const size_t n = Learning::ReviewFormat::buildQueueLine(line, sizeof(line), in);
  ASSERT_GT(n, 0u);
  EXPECT_STREQ("RVQ1 1 candid\n", line);

  Learning::ReviewFormat::QueueEntry out;
  ASSERT_TRUE(Learning::ReviewFormat::parseQueueLine(line, n, out));
  EXPECT_EQ(1, out.lastRating);
  EXPECT_STREQ("candid", out.word);
}

TEST(ReviewQueueFormat, NeverReviewedUsesMinusOne) {
  Learning::ReviewFormat::QueueEntry in;
  std::strcpy(in.word, "abandon");
  char line[128];
  const size_t n = Learning::ReviewFormat::buildQueueLine(line, sizeof(line), in);
  ASSERT_GT(n, 0u);
  EXPECT_STREQ("RVQ1 -1 abandon\n", line);

  Learning::ReviewFormat::QueueEntry out;
  out.lastRating = 3;
  ASSERT_TRUE(Learning::ReviewFormat::parseQueueLine(line, n, out));
  EXPECT_EQ(-1, out.lastRating);
  EXPECT_STREQ("abandon", out.word);
}

TEST(ReviewQueueFormat, RejectsMalformedLines) {
  Learning::ReviewFormat::QueueEntry out;
  const char* foreign = "RVW1 3 abc123 word\n";
  EXPECT_FALSE(Learning::ReviewFormat::parseQueueLine(foreign, std::strlen(foreign), out));
  const char* badRating = "RVQ1 7 word\n";
  EXPECT_FALSE(Learning::ReviewFormat::parseQueueLine(badRating, std::strlen(badRating), out));
  const char* badNegative = "RVQ1 -2 word\n";
  EXPECT_FALSE(Learning::ReviewFormat::parseQueueLine(badNegative, std::strlen(badNegative), out));
  const char* noNewline = "RVQ1 1 word";
  EXPECT_FALSE(Learning::ReviewFormat::parseQueueLine(noNewline, std::strlen(noNewline), out));

  Learning::ReviewFormat::QueueEntry in;
  std::strcpy(in.word, "word");
  in.lastRating = 5;
  char line[128];
  EXPECT_EQ(0u, Learning::ReviewFormat::buildQueueLine(line, sizeof(line), in));
}

TEST(ReviewCardPath, BuildsOnlyForValidWords) {
  char path[96];
  ASSERT_TRUE(Learning::ReviewFormat::cardPath(path, sizeof(path), "abandon"));
  EXPECT_STREQ("/.crosspoint/learning/cards/abandon.txt", path);

  ASSERT_TRUE(Learning::ReviewFormat::cardPath(path, sizeof(path), "can't"));
  EXPECT_STREQ("/.crosspoint/learning/cards/can't.txt", path);

  EXPECT_FALSE(Learning::ReviewFormat::cardPath(path, sizeof(path), "../etc/passwd"));
  EXPECT_FALSE(Learning::ReviewFormat::cardPath(path, sizeof(path), "two words"));
  EXPECT_FALSE(Learning::ReviewFormat::cardPath(path, 10, "abandon"));
}

}  // namespace
