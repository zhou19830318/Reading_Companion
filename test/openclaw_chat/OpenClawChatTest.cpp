#include <OpenClawChat.h>
#include <StreamingJsonParser.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

// A frame is only ever fed whole, so the parser must see well-formed JSON —
// this is what catches a hand-assembled frame that forgets a brace.
bool isValidJson(const std::string& s) {
  JsonCallbacks noop{};
  StreamingJsonParser parser(noop);
  parser.feed(s.data(), s.size());
  return !parser.hasError();
}

constexpr char kDelta1[] =
    R"({"type":"event","event":"chat","payload":{"state":"delta","runId":"r1","sessionKey":"default",)"
    R"("message":{"content":[{"type":"text","text":"Hello, "}]}}})";

constexpr char kDelta2[] =
    R"({"type":"event","event":"chat","payload":{"state":"delta","runId":"r1","sessionKey":"default",)"
    R"("message":{"content":[{"type":"text","text":"world"}]}}})";

constexpr char kFinalEmptyContent[] = R"({"type":"event","event":"chat","payload":{"state":"final","runId":"r1",)"
                                      R"("message":{"content":[]}}})";

constexpr char kFinalAfterDeltas[] = R"({"type":"event","event":"chat","payload":{"state":"final","runId":"r1",)"
                                     R"("message":{"content":[{"type":"text","text":"PONG"}]}}})";

constexpr char kErrorFrame[] = R"({"type":"event","event":"chat","payload":{"state":"error","runId":"r1",)"
                               R"("errorMessage":"model unavailable"}})";

constexpr char kAbortedFrame[] = R"({"type":"event","event":"chat","payload":{"state":"aborted","runId":"r1"}})";

// An `agent` lifecycle event also carries a message; it must never be mistaken
// for a reply we asked for.
constexpr char kAgentFrame[] = R"({"type":"event","event":"agent","payload":{"stream":"lifecycle","runId":"r1",)"
                               R"("message":{"content":[{"type":"text","text":"not our reply"}]}}})";

TEST(BuildChatSendFrame, ProducesAWellFormedRequest) {
  char buf[OpenClaw::CHAT_BUF_CAP];
  const size_t n = OpenClaw::buildChatSendFrame(buf, sizeof(buf), 7, "ping", "key-1");
  ASSERT_GT(n, 0u);
  const std::string frame(buf, n);

  EXPECT_TRUE(isValidJson(frame));
  EXPECT_NE(frame.find(R"("type":"req")"), std::string::npos);
  EXPECT_NE(frame.find(R"("id":"7")"), std::string::npos);
  EXPECT_NE(frame.find(R"("method":"chat.send")"), std::string::npos);
  EXPECT_NE(frame.find(R"("sessionKey":"crosspoint")"), std::string::npos);
  EXPECT_NE(frame.find(R"("message":"ping")"), std::string::npos);
  EXPECT_NE(frame.find(R"("idempotencyKey":"key-1")"), std::string::npos);
}

TEST(BuildChatSendFrame, EscapesAndRoundTripsSpecialCharacters) {
  char buf[OpenClaw::CHAT_BUF_CAP];
  const char* message = "say \"hi\"\nback\\slash\ttab";
  const size_t n = OpenClaw::buildChatSendFrame(buf, sizeof(buf), 1, message, "k");
  ASSERT_GT(n, 0u);
  const std::string frame(buf, n);
  ASSERT_TRUE(isValidJson(frame));

  // Read the message back out of the frame rather than eyeballing escapes.
  std::string captured;
  char lastKey[32] = {};
  JsonCallbacks cb{};
  struct Ctx {
    std::string* out;
    char* lastKey;
  } ctx{&captured, lastKey};
  cb.ctx = &ctx;
  cb.onKey = [](void* v, const char* key, size_t len) {
    auto* c = static_cast<Ctx*>(v);
    const size_t n2 = len < 31 ? len : 31;
    memcpy(c->lastKey, key, n2);
    c->lastKey[n2] = '\0';
  };
  cb.onString = [](void* v, const char* value, size_t len) {
    auto* c = static_cast<Ctx*>(v);
    if (strcmp(c->lastKey, "message") == 0) *c->out = std::string(value, len);
  };
  StreamingJsonParser parser(cb);
  parser.feed(frame.data(), frame.size());
  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(captured, message);
}

TEST(BuildChatSendFrame, RejectsInputsThatCannotProduceAFrame) {
  char buf[OpenClaw::CHAT_BUF_CAP];
  EXPECT_EQ(OpenClaw::buildChatSendFrame(buf, sizeof(buf), 1, "", "k"), 0u);
  EXPECT_EQ(OpenClaw::buildChatSendFrame(buf, sizeof(buf), 1, nullptr, "k"), 0u);
  EXPECT_EQ(OpenClaw::buildChatSendFrame(buf, sizeof(buf), 1, "ping", nullptr), 0u);
  EXPECT_EQ(OpenClaw::buildChatSendFrame(nullptr, sizeof(buf), 1, "ping", "k"), 0u);

  const std::string tooLong(OpenClaw::CHAT_MESSAGE_CAP + 1, 'x');
  EXPECT_EQ(OpenClaw::buildChatSendFrame(buf, sizeof(buf), 1, tooLong.c_str(), "k"), 0u);

  // A cap that cannot hold the scaffolding must not leave a half-written frame.
  char small[32];
  memset(small, 'A', sizeof(small));
  EXPECT_EQ(OpenClaw::buildChatSendFrame(small, sizeof(small), 1, "a long message", "k"), 0u);
}

TEST(ChatReply, AccumulatesDeltasInOrder) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  EXPECT_EQ(reply.feed(kDelta1, strlen(kDelta1)), OpenClaw::ChatFrame::Delta);
  EXPECT_EQ(reply.feed(kDelta2, strlen(kDelta2)), OpenClaw::ChatFrame::Delta);
  EXPECT_STREQ(reply.text(), "Hello, world");
  EXPECT_EQ(reply.textSize(), strlen("Hello, world"));
  EXPECT_FALSE(reply.truncated());
}

TEST(ChatReply, FinalReplacesAccumulatedDeltas) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  reply.feed(kDelta1, strlen(kDelta1));
  reply.feed(kDelta2, strlen(kDelta2));
  EXPECT_EQ(reply.feed(kFinalAfterDeltas, strlen(kFinalAfterDeltas)), OpenClaw::ChatFrame::Final);
  EXPECT_STREQ(reply.text(), "PONG");
}

TEST(ChatReply, FinalWithoutTextKeepsTheDeltas) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  reply.feed(kDelta1, strlen(kDelta1));
  // An empty content array means the final frame carried nothing we could
  // read; blanking the reply would throw away text we already have.
  EXPECT_EQ(reply.feed(kFinalEmptyContent, strlen(kFinalEmptyContent)), OpenClaw::ChatFrame::Final);
  EXPECT_STREQ(reply.text(), "Hello, ");
}

TEST(ChatReply, IgnoresFramesThatAreNotOurReply) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  EXPECT_EQ(reply.feed(kAgentFrame, strlen(kAgentFrame)), OpenClaw::ChatFrame::Ignored);
  EXPECT_STREQ(reply.text(), "");
  EXPECT_STREQ(reply.eventName(), "agent");

  // The connect acknowledgement is a res without an error: not a failure.
  constexpr char kAck[] = R"({"type":"res","id":"3","payload":{"authenticated":true}})";
  EXPECT_EQ(reply.feed(kAck, strlen(kAck)), OpenClaw::ChatFrame::Ignored);
  EXPECT_STREQ(reply.text(), "");
}

TEST(ChatReply, ClassificationDoesNotDependOnKeyOrder) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  // payload ahead of event: `event` is still known by the time the document
  // ends, which is when the classification is made.
  constexpr char kReordered[] =
      R"({"type":"event","payload":{"state":"final","message":{"content":[{"text":"PONG"}]}},"event":"chat"})";
  EXPECT_EQ(reply.feed(kReordered, strlen(kReordered)), OpenClaw::ChatFrame::Final);
  EXPECT_STREQ(reply.text(), "PONG");
}

TEST(ChatReply, ReportsGatewayErrors) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  EXPECT_EQ(reply.feed(kErrorFrame, strlen(kErrorFrame)), OpenClaw::ChatFrame::Error);
  EXPECT_STREQ(reply.error(), "model unavailable");
  EXPECT_STREQ(reply.text(), "");

  reply.reset();
  constexpr char kResError[] = R"({"type":"res","id":"5","error":{"code":"BAD_REQUEST","message":"nope"}})";
  EXPECT_EQ(reply.feed(kResError, strlen(kResError)), OpenClaw::ChatFrame::Error);
  EXPECT_STREQ(reply.error(), "nope");

  reply.reset();
  EXPECT_EQ(reply.feed(kAbortedFrame, strlen(kAbortedFrame)), OpenClaw::ChatFrame::Aborted);
  EXPECT_STREQ(reply.text(), "");
}

TEST(ChatReply, FlagsTextThatDoesNotFit) {
  char storage[16];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  constexpr char kLongDelta[] = R"({"type":"event","event":"chat","payload":{"state":"delta",)"
                                R"("message":{"content":[{"type":"text","text":"0123456789abcdefghij"}]}}})";
  EXPECT_EQ(reply.feed(kLongDelta, strlen(kLongDelta)), OpenClaw::ChatFrame::Delta);
  EXPECT_TRUE(reply.truncated());
  EXPECT_EQ(reply.textSize(), 15u);
  EXPECT_STREQ(reply.text(), "0123456789abcde");
}

TEST(ChatReply, ResetAndDetachDropEverything) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  reply.feed(kDelta1, strlen(kDelta1));
  ASSERT_GT(reply.textSize(), 0u);
  reply.reset();
  EXPECT_EQ(reply.textSize(), 0u);
  EXPECT_STREQ(reply.text(), "");
  EXPECT_FALSE(reply.truncated());

  reply.attach(nullptr, 0);
  EXPECT_EQ(reply.feed(kDelta1, strlen(kDelta1)), OpenClaw::ChatFrame::Ignored);
}

TEST(IsAgentFailureReply, MarkerAndNothingElseIsAFailure) {
  constexpr const char* kMarker = "[assistant turn failed before producing content]";
  EXPECT_TRUE(OpenClaw::isAgentFailureReply(""));
  EXPECT_TRUE(OpenClaw::isAgentFailureReply(" \r\n\t"));
  EXPECT_TRUE(OpenClaw::isAgentFailureReply(kMarker));
  // The gateway stacks a copy per in-turn retry; five were seen in one reply.
  EXPECT_TRUE(OpenClaw::isAgentFailureReply((std::string(kMarker) + "\n" + kMarker + "\n" + kMarker).c_str()));
  EXPECT_TRUE(OpenClaw::isAgentFailureReply((std::string(kMarker) + kMarker).c_str()));
}

TEST(IsAgentFailureReply, AnyRealContentWinsOverTheMarker) {
  constexpr const char* kMarker = "[assistant turn failed before producing content]";
  // The usual shape: the marker first, then the answer that did arrive.
  EXPECT_FALSE(OpenClaw::isAgentFailureReply((std::string(kMarker) + "\nPONG").c_str()));
  // ...and the other order, which the marker-stripping shortcut gets wrong.
  EXPECT_FALSE(OpenClaw::isAgentFailureReply((std::string("PONG\n") + kMarker).c_str()));
  // A marker that is cut off is ordinary text, not the marker.
  EXPECT_FALSE(OpenClaw::isAgentFailureReply("[assistant turn failed"));
}

// Wraps text as a streamed delta frame, escaping what JSON requires.
std::string chatDelta(const std::string& text) {
  std::string esc;
  for (const char c : text) {
    if (c == '\n') {
      esc += "\\n";
    } else if (c == '"' || c == '\\') {
      esc += '\\';
      esc += c;
    } else {
      esc += c;
    }
  }
  return std::string(R"({"type":"event","event":"chat","payload":{"state":"delta","message":)"
                     R"({"content":[{"type":"text","text":")") +
         esc + R"("}]}}})";
}

// Wraps text as the authoritative final frame, escaping what JSON requires.
std::string chatFinal(const std::string& text) {
  std::string esc;
  for (const char c : text) {
    if (c == '\n') {
      esc += "\\n";
    } else if (c == '"' || c == '\\') {
      esc += '\\';
      esc += c;
    } else {
      esc += c;
    }
  }
  return std::string(R"({"type":"event","event":"chat","payload":{"state":"final","message":)"
                     R"({"content":[{"type":"text","text":")") +
         esc + R"("}]}}})";
}

TEST(ChatReply, StripsMarkersAndKeepsTheAnswer) {
  constexpr const char* kMarker = "[assistant turn failed before producing content]";
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  // Measured on device 2026-09-26: two markers, then the answer.
  const std::string raw = std::string(kMarker) + "\n" + kMarker + "\n" + "PONG";
  const std::string frame = chatFinal(raw);
  EXPECT_EQ(reply.feed(frame.data(), frame.size()), OpenClaw::ChatFrame::Final);
  EXPECT_EQ(reply.textSize(), raw.size());

  EXPECT_EQ(reply.stripFailureMarkers(), 2u);
  EXPECT_STREQ(reply.text(), "PONG");
  EXPECT_EQ(reply.textSize(), strlen("PONG"));
  // A second pass has nothing left to do.
  EXPECT_EQ(reply.stripFailureMarkers(), 0u);
}

TEST(ChatReply, StripsMarkersAfterTheAnswerToo) {
  constexpr const char* kMarker = "[assistant turn failed before producing content]";
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  const std::string frame = chatFinal(std::string("PONG\n") + kMarker);
  EXPECT_EQ(reply.feed(frame.data(), frame.size()), OpenClaw::ChatFrame::Final);
  EXPECT_EQ(reply.stripFailureMarkers(), 1u);
  EXPECT_STREQ(reply.text(), "PONG");
}

TEST(ChatReply, MarkerOnlyReplyStripsToEmptyAndFails) {
  constexpr const char* kMarker = "[assistant turn failed before producing content]";
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  const std::string frame = chatFinal(std::string(kMarker) + "\n" + kMarker);
  EXPECT_EQ(reply.feed(frame.data(), frame.size()), OpenClaw::ChatFrame::Final);
  EXPECT_EQ(reply.stripFailureMarkers(), 2u);
  EXPECT_STREQ(reply.text(), "");
  EXPECT_EQ(reply.textSize(), 0u);
  // Empty is exactly what the activity's re-send check wants to see.
  EXPECT_TRUE(OpenClaw::isAgentFailureReply(reply.text()));
}

TEST(ChatReply, StripLeavesAHealthyReplyAlone) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  EXPECT_EQ(reply.feed(kFinalAfterDeltas, strlen(kFinalAfterDeltas)), OpenClaw::ChatFrame::Final);
  EXPECT_EQ(reply.stripFailureMarkers(), 0u);
  EXPECT_STREQ(reply.text(), "PONG");
}

TEST(ChatReply, LongStoryReplySurvives) {
  // Measured on device 2026-09-27: a story-style reply arrives as ONE text
  // item of 2–3 KB, above the parser's 512-byte token buffer. Before the
  // chunked-string path existed, the item was dropped whole — delta AND final
  // carried "reply 0 bytes" — and the round trip collapsed into the
  // failure-marker retry. This is the regression test for that session.
  // ~115 bytes of CJK prefix plus 1200 three-byte characters lands the story
  // above the parser's token buffer (so the chunk path is exercised) but
  // below CHAT_BUF_CAP (so the whole text fits the reply buffer).
  const std::string story =
      "好好好，再来一个。\n\n从前有个画家，画了一辈子，没有一幅画卖出去。\n\n有一天他画了" + std::string(1200, '文');
  ASSERT_GT(story.size(), StreamingJsonParser::TOKEN_BUF_SIZE);
  ASSERT_LT(story.size(), OpenClaw::CHAT_BUF_CAP);

  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  // Deltas stream the first chunk, the final frame replaces with the whole
  // authoritative text — both above 512 bytes, both previously dropped.
  const std::string head = story.substr(0, 700);
  EXPECT_EQ(reply.feed(chatDelta(head).data(), chatDelta(head).size()), OpenClaw::ChatFrame::Delta);
  EXPECT_EQ(reply.textSize(), head.size());

  const std::string frame = chatFinal(story);
  EXPECT_EQ(reply.feed(frame.data(), frame.size()), OpenClaw::ChatFrame::Final);
  EXPECT_EQ(reply.textSize(), story.size());
  EXPECT_EQ(std::string(reply.text(), reply.textSize()), story);
  EXPECT_FALSE(reply.truncated());
  EXPECT_FALSE(OpenClaw::isAgentFailureReply(reply.text()));
}

TEST(ChatReply, ReplyLongerThanTheBufferIsFlaggedTruncated) {
  // One item larger than CHAT_BUF_CAP itself cannot fit; keep the prefix and
  // flag it so the panel can say "(truncated)" instead of cutting silently.
  const std::string big(OpenClaw::CHAT_BUF_CAP + 300, 'a');

  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));

  const std::string frame = chatFinal(big);
  EXPECT_EQ(reply.feed(frame.data(), frame.size()), OpenClaw::ChatFrame::Final);
  EXPECT_EQ(reply.textSize(), OpenClaw::CHAT_BUF_CAP - 1);
  EXPECT_TRUE(reply.truncated());
  // The prefix is real content, not the failure marker.
  EXPECT_FALSE(OpenClaw::isAgentFailureReply(reply.text()));
}

// ── stripMarkdownMarkers ──────────────────────────────────────────────────

// Feeds text through the real parser (so len_ tracks like on device), then
// strips failure markers and markdown; returns the cleaned text.
std::string cleaned(const std::string& text) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));
  const std::string frame = chatFinal(text);
  EXPECT_EQ(reply.feed(frame.data(), frame.size()), OpenClaw::ChatFrame::Final);
  reply.stripFailureMarkers();
  reply.stripMarkdownMarkers();
  return std::string(reply.text());
}

TEST(StripMarkdown, RemovesBoldEmphasis) {
  // The device-measured shape (voice_tx3.log): CJK prose around a **bold** time.
  EXPECT_EQ(cleaned("搞定！明早 **8:00** 叫你"), "搞定！明早 8:00 叫你");
}

TEST(StripMarkdown, RemovesItalicEmphasis) { EXPECT_EQ(cleaned("a *em* b"), "a em b"); }

TEST(StripMarkdown, RemovesInlineCode) { EXPECT_EQ(cleaned("run `ls -l` now"), "run ls -l now"); }

TEST(StripMarkdown, RemovesFences) { EXPECT_EQ(cleaned("```\ncode line\n```\nafter"), "\ncode line\n\nafter"); }

TEST(StripMarkdown, NoPairsMeansEveryByteSurvives) {
  // Unclosed ** must not eat its own second asterisk as a closer, and 2*3 is
  // a product (its would-be closer never comes).
  EXPECT_EQ(cleaned("**8:00 and 2*3 stay"), "**8:00 and 2*3 stay");
}

TEST(StripMarkdown, UnderscoreFlavoursStayUntouched) {
  // snake_case and file names are far more common in replies than _em_ —
  // corrupting them is worse than showing the odd underscore.
  EXPECT_EQ(cleaned("file_name.txt and _em_ stay"), "file_name.txt and _em_ stay");
}

TEST(StripMarkdown, EmphasisNeverSpansLines) {
  // The asterisks sit on different lines: no pairing, both survive.
  EXPECT_EQ(cleaned("*a\nb* c"), "*a\nb* c");
}

TEST(StripMarkdown, BoldDoesNotFallBackToSingleAsteriskClose) {
  // "**8:00** and 2*3": the ** pair closes normally; the product sign after
  // it must survive (a ** opener may not close with a single *).
  EXPECT_EQ(cleaned("**8:00** and 2*3"), "8:00 and 2*3");
}

TEST(StripMarkdown, WorksOnDeviceReplyShape) {
  // The exact reply measured on device 2026-09-28 (voice_tx3.log) — the
  // regression test for the M0-3 fix.
  EXPECT_EQ(cleaned("搞定！⏰ 明早 **8:00** 会叫你起床。\n\n晚安，好好休息！😴"),
            "搞定！⏰ 明早 8:00 会叫你起床。\n\n晚安，好好休息！😴");
}

// ── stripStrippableSymbols ────────────────────────────────────────────────

// The full pipeline Session::handleChatFrame runs on a reply: failure
// markers → markdown → strippable symbols.
std::string fullyCleaned(const std::string& text) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));
  const std::string frame = chatFinal(text);
  EXPECT_EQ(reply.feed(frame.data(), frame.size()), OpenClaw::ChatFrame::Final);
  reply.stripFailureMarkers();
  reply.stripMarkdownMarkers();
  reply.stripStrippableSymbols();
  return std::string(reply.text());
}

TEST(StripSymbols, RemovesDeviceReplyEmoji) {
  // Screenshot evidence 2026-09-29: ⏰ and 😴 painted as .notdef boxes on the
  // voice panel (builtin Noto/Ubuntu and the SD LXGW fallback carry no emoji).
  EXPECT_EQ(fullyCleaned("搞定！⏰ 明早 **8:00** 会叫你起床。\n\n晚安，好好休息！😴"),
            "搞定！ 明早 8:00 会叫你起床。\n\n晚安，好好休息！");
}

TEST(StripSymbols, RemovesEmojiPresentationSequences) {
  // Base + VS16 + ZWJ + skin tone: every member is in a strippable range, so
  // the whole sequence disappears without stray leftovers.
  EXPECT_EQ(fullyCleaned("a❤️‍🔥b"), "ab");
  EXPECT_EQ(fullyCleaned("x👍🏽y"), "xy");
}

TEST(StripSymbols, KeepsCjkAsciiAndSpacing) { EXPECT_EQ(fullyCleaned("中文abc 123!"), "中文abc 123!"); }

TEST(StripSymbols, ReturnsBytesRemoved) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));
  const std::string frame = chatFinal("a⏰b");
  ASSERT_EQ(reply.feed(frame.data(), frame.size()), OpenClaw::ChatFrame::Final);
  EXPECT_EQ(reply.stripStrippableSymbols(), 3u);  // ⏰ is 3 UTF-8 bytes
  EXPECT_STREQ(reply.text(), "ab");
  EXPECT_EQ(reply.stripStrippableSymbols(), 0u);  // idempotent
}

TEST(StripSymbols, EmptyReplyIsUntouched) {
  char storage[OpenClaw::CHAT_BUF_CAP];
  OpenClaw::ChatReply reply(storage, sizeof(storage));
  EXPECT_EQ(reply.stripStrippableSymbols(), 0u);
  EXPECT_STREQ(reply.text(), "");
}

}  // namespace
