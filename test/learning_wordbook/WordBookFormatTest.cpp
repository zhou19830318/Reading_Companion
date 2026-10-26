#include <gtest/gtest.h>

#include "WordBookFormat.h"

using Learning::WordBookFormat::buildLine;
using Learning::WordBookFormat::Entry;
using Learning::WordBookFormat::parseLine;
using Learning::WordBookFormat::State;
using Learning::WordBookFormat::stateChar;
using Learning::WordBookFormat::validWord;

TEST(WordBookFormat, RoundTripsAllStates) {
  for (const State state : {State::Pending, State::Synced, State::Removing}) {
    Entry in;
    in.addedAt = 1750000000;
    in.state = state;
    snprintf(in.word, sizeof(in.word), "%s", "abandon");

    char line[128];
    const size_t len = buildLine(line, sizeof(line), in);
    ASSERT_GT(len, 0u);
    EXPECT_EQ(line[len - 1], '\n');

    Entry out;
    ASSERT_TRUE(parseLine(line, len, out));
    EXPECT_EQ(out.addedAt, in.addedAt);
    EXPECT_EQ(out.state, state);
    EXPECT_STREQ(out.word, "abandon");
  }
}

TEST(WordBookFormat, StateCharMatchesTheWireContract) {
  EXPECT_EQ('p', stateChar(State::Pending));
  EXPECT_EQ('s', stateChar(State::Synced));
  EXPECT_EQ('r', stateChar(State::Removing));

  Entry out;
  const auto parse = [&](const char* s) { return parseLine(s, strlen(s), out); };
  ASSERT_TRUE(parse("WBT1 1750000000 p abandon\n"));
  EXPECT_EQ(State::Pending, out.state);
  ASSERT_TRUE(parse("WBT1 1750000000 s abandon\n"));
  EXPECT_EQ(State::Synced, out.state);
  ASSERT_TRUE(parse("WBT1 1750000000 r abandon\n"));  // removal tombstone
  EXPECT_EQ(State::Removing, out.state);
}

TEST(WordBookFormat, AcceptsJoinersAndZeroTimestamp) {
  Entry in;
  in.addedAt = 0;
  snprintf(in.word, sizeof(in.word), "%s", "well-known");
  char line[128];
  const size_t len = buildLine(line, sizeof(line), in);
  ASSERT_GT(len, 0u);
  Entry out;
  ASSERT_TRUE(parseLine(line, len, out));
  EXPECT_EQ(out.addedAt, 0u);
  EXPECT_STREQ(out.word, "well-known");

  snprintf(in.word, sizeof(in.word), "%s", "it's");
  const size_t len2 = buildLine(line, sizeof(line), in);
  ASSERT_GT(len2, 0u);
  ASSERT_TRUE(parseLine(line, len2, out));
  EXPECT_STREQ(out.word, "it's");
}

TEST(WordBookFormat, RejectsMalformedLines) {
  Entry out;
  const auto parse = [&](const char* s) { return parseLine(s, strlen(s), out); };
  EXPECT_FALSE(parse(""));
  EXPECT_FALSE(parse("WBT1 1 p word"));             // missing newline
  EXPECT_FALSE(parse("WBT2 1 p word\n"));           // wrong tag
  EXPECT_FALSE(parse("WBT1 x p word\n"));           // non-numeric timestamp
  EXPECT_FALSE(parse("WBT1 1 q word\n"));           // bad state
  EXPECT_FALSE(parse("WBT1 1 p \n"));               // empty word
  EXPECT_FALSE(parse("WBT1 1 p two words\n"));      // space in word
  EXPECT_FALSE(parse("WBT1 1 p bad!word\n"));       // invalid character
  EXPECT_FALSE(parse("WBT1 4294967296 p word\n"));  // timestamp overflow
}

TEST(WordBookFormat, RejectsOverlongAndEmptyWords) {
  Entry in;
  in.addedAt = 1;
  memset(in.word, 'a', sizeof(in.word));  // no NUL: strnlen hits the cap
  char line[256];
  EXPECT_EQ(buildLine(line, sizeof(line), in), 0u);

  Entry empty;
  empty.addedAt = 1;
  EXPECT_EQ(buildLine(line, sizeof(line), empty), 0u);
  EXPECT_FALSE(validWord(nullptr));
  EXPECT_FALSE(validWord(""));
  EXPECT_TRUE(validWord("a"));
  EXPECT_TRUE(validWord("abandon"));
  EXPECT_TRUE(validWord("mother-in-law's"));
}
