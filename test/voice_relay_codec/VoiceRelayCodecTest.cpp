#include <VoiceRelayCodec.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace VoiceRelay;

namespace {

Event parseOk(const std::string& json) {
  Event e;
  EXPECT_TRUE(parseEvent(json.c_str(), json.size(), e)) << json;
  return e;
}

std::string str(const char* s, size_t n) { return std::string(s, n); }

// ---------------------------------------------------------------- events

TEST(VoiceRelayCodec, ParsesSessionStarted) {
  const Event e = parseOk(
      R"({"type":"session.started","requestId":"req-1","sessionId":"sess-1","serverTime":"2026-10-16T04:54:48Z"})");
  EXPECT_EQ(e.type, EventType::SessionStarted);
  EXPECT_STREQ(e.requestId, "req-1");
  EXPECT_STREQ(e.sessionId, "sess-1");
  EXPECT_STREQ(e.text, "");
  EXPECT_EQ(e.seq, 0u);
}

TEST(VoiceRelayCodec, ParsesAsrPartialWithSeqAndUnicode) {
  const Event e = parseOk(R"({"type":"asr.partial","requestId":"r","sessionId":"s","text":"你好，请帮","seq":3})");
  EXPECT_EQ(e.type, EventType::AsrPartial);
  EXPECT_STREQ(e.text, "你好，请帮");
  EXPECT_EQ(e.seq, 3u);
  EXPECT_FALSE(e.truncated);
  EXPECT_FALSE(e.retryable);
}

TEST(VoiceRelayCodec, ParsesAsrFinalAndLlmStart) {
  const Event f =
      parseOk(R"({"type":"asr.final","requestId":"r","sessionId":"s","text":"你好，推荐一本书。","seq":4})");
  EXPECT_EQ(f.type, EventType::AsrFinal);
  EXPECT_EQ(f.seq, 4u);

  const Event l = parseOk(R"({"type":"llm.start","requestId":"r","sessionId":"s"})");
  EXPECT_EQ(l.type, EventType::LlmStart);
  EXPECT_STREQ(l.text, "");
}

TEST(VoiceRelayCodec, ParsesLlmDeltaAndFinal) {
  const Event d = parseOk(R"({"type":"llm.delta","requestId":"r","sessionId":"s","text":"当然","seq":5})");
  EXPECT_EQ(d.type, EventType::LlmDelta);
  EXPECT_STREQ(d.text, "当然");

  const Event f = parseOk(
      R"({"type":"llm.final","requestId":"r","sessionId":"s","text":"当然可以。","usage":{"inputTokens":40,"outputTokens":9}})");
  EXPECT_EQ(f.type, EventType::LlmFinal);
  EXPECT_STREQ(f.text, "当然可以。");
}

TEST(VoiceRelayCodec, LongTextBeyondTokenBufferStreamsInParts) {
  std::string text;
  for (int i = 0; i < 300; ++i) text += "中";  // 900 bytes > parser token buffer
  const std::string json = R"({"type":"llm.final","text":")" + text + R"(","usage":{"a":1}})";
  const Event e = parseOk(json);
  EXPECT_EQ(e.type, EventType::LlmFinal);
  EXPECT_TRUE(e.truncated);
  EXPECT_EQ(strlen(e.text), 510u);  // 170 full 3-byte codepoints
  EXPECT_EQ(std::string(e.text, 3), "中");
  EXPECT_EQ(std::string(e.text + strlen(e.text) - 3, 3), "中");  // no half codepoint at the end
}

TEST(VoiceRelayCodec, TextOfExactlyCapBytesIsNotTruncated) {
  const std::string text(512, 'a');
  const std::string json = R"({"type":"llm.final","text":")" + text + R"("})";
  const Event e = parseOk(json);
  EXPECT_FALSE(e.truncated);
  EXPECT_EQ(strlen(e.text), 512u);
}

TEST(VoiceRelayCodec, ParsesSessionDoneLatencies) {
  const Event e = parseOk(
      R"({"type":"session.done","requestId":"req-1","sessionId":"sess-1","latencyMs":{"connect":51,"asrFirstPartial":648,"asrFinal":334,"llmFirstToken":336,"llmFinal":699,"visibleTotal":1032}})");
  EXPECT_EQ(e.type, EventType::SessionDone);
  EXPECT_EQ(e.latency.connect, 51);
  EXPECT_EQ(e.latency.asrFirstPartial, 648);
  EXPECT_EQ(e.latency.asrFinal, 334);
  EXPECT_EQ(e.latency.llmFirstToken, 336);
  EXPECT_EQ(e.latency.llmFinal, 699);
  EXPECT_EQ(e.latency.visibleTotal, 1032);
}

TEST(VoiceRelayCodec, AbsentLatencyFieldsStayAtSentinel) {
  const Event e = parseOk(R"({"type":"session.done","requestId":"r","sessionId":"s","latencyMs":{"asrFinal":10}})");
  EXPECT_EQ(e.latency.asrFinal, 10);
  EXPECT_EQ(e.latency.connect, -1);
  EXPECT_EQ(e.latency.llmFinal, -1);
  EXPECT_EQ(e.latency.visibleTotal, -1);
}

TEST(VoiceRelayCodec, ParsesErrorWithWireRetryable) {
  const Event e = parseOk(
      R"({"type":"error","requestId":"req-1","sessionId":"sess-1","code":"ASR_CONNECTION_ERROR","message":"语音识别服务连接失败","retryable":true,"seq":7})");
  EXPECT_EQ(e.type, EventType::Error);
  EXPECT_EQ(e.code, ErrorCode::AsrConnectionError);
  EXPECT_STREQ(e.message, "语音识别服务连接失败");
  EXPECT_TRUE(e.retryable);
  EXPECT_EQ(e.seq, 7u);
}

TEST(VoiceRelayCodec, ErrorWithoutRetryableFallsBackToLocalPolicy) {
  const Event transient =
      parseOk(R"({"type":"error","requestId":"r","sessionId":"s","code":"LLM_CONNECTION_ERROR","message":"m"})");
  EXPECT_TRUE(transient.retryable);

  const Event fatal =
      parseOk(R"({"type":"error","requestId":"r","sessionId":"s","code":"UNAUTHORIZED","message":"m"})");
  EXPECT_FALSE(fatal.retryable);

  const Event unknown =
      parseOk(R"({"type":"error","requestId":"r","sessionId":"s","code":"SOMETHING_NEW","message":"m"})");
  EXPECT_EQ(unknown.code, ErrorCode::Unknown);
  EXPECT_FALSE(unknown.retryable);
}

TEST(VoiceRelayCodec, RetryableTrueOnWireWinsOverLocalPolicy) {
  const Event e = parseOk(
      R"({"type":"error","requestId":"r","sessionId":"s","code":"UNAUTHORIZED","message":"m","retryable":true})");
  EXPECT_TRUE(e.retryable);
}

TEST(VoiceRelayCodec, RejectsMalformedAndUnknownTypeAndResets) {
  Event e;
  const char* garbage = "not json";
  EXPECT_FALSE(parseEvent(garbage, strlen(garbage), e));
  EXPECT_EQ(e.type, EventType::Unknown);
  const char* surprise = R"({"type":"surprise","text":"x"})";
  EXPECT_FALSE(parseEvent(surprise, strlen(surprise), e));
  EXPECT_EQ(e.type, EventType::Unknown);
  EXPECT_FALSE(parseEvent("", 0, e));
  EXPECT_FALSE(parseEvent(nullptr, 10, e));

  // A real frame after the garbage parses cleanly (no state bleed).
  const Event ok = parseOk(R"({"type":"asr.final","requestId":"r","sessionId":"s","text":"好","seq":9})");
  EXPECT_EQ(ok.type, EventType::AsrFinal);
  EXPECT_STREQ(ok.text, "好");
  EXPECT_EQ(ok.seq, 9u);
}

TEST(VoiceRelayCodec, OversizedIdFieldsAreCutNotOverrun) {
  const std::string longId(200, 'a');
  const std::string json = R"({"type":"asr.final","requestId":")" + longId + R"(","sessionId":"s","text":"x"})";
  const Event e = parseOk(json);
  EXPECT_EQ(strlen(e.requestId), REQUEST_ID_CAP - 1);
}

TEST(VoiceRelayCodec, NamesRoundTrip) {
  EXPECT_STREQ(eventTypeName(EventType::AsrPartial), "asr.partial");
  EXPECT_STREQ(eventTypeName(EventType::SessionDone), "session.done");
  EXPECT_STREQ(errorCodeName(ErrorCode::AsrConnectionError), "ASR_CONNECTION_ERROR");
  EXPECT_STREQ(errorCodeName(ErrorCode::ServerOverloaded), "SERVER_OVERLOADED");
  EXPECT_STREQ(errorCodeName(ErrorCode::Unknown), "UNKNOWN");
}

// ---------------------------------------------------------------- builders

TEST(VoiceRelayCodec, SessionStartFrameIsExact) {
  char buf[512];
  const size_t n = buildSessionStart(buf, sizeof(buf), "0123456789abcdef0123456789abcdef", "reading-companion-001");
  const std::string expected =
      R"({"type":"session.start","requestId":"0123456789abcdef0123456789abcdef","deviceId":"reading-companion-001",)"
      R"("audio":{"sampleRate":16000,"channels":1,"format":"pcm_s16le","frameMs":40},"mode":"relay"})";
  EXPECT_EQ(str(buf, n), expected);
}

TEST(VoiceRelayCodec, SessionStartWorstCaseFitsControlFrameCap) {
  // requestId is always 32 hex chars and device ids run up to DEVICE_ID_CAP-1.
  // The client once built session.start from a 192-byte buffer, so every real
  // round failed the frame build and silently fell back to legacy; the worst
  // case must fit CONTROL_FRAME_CAP with margin to spare.
  char request[REQUEST_ID_CAP];
  memset(request, 'a', sizeof(request) - 1);
  request[sizeof(request) - 1] = '\0';
  char device[DEVICE_ID_CAP];
  memset(device, 'd', sizeof(device) - 1);
  device[sizeof(device) - 1] = '\0';

  char buf[CONTROL_FRAME_CAP];
  const size_t n = buildSessionStart(buf, sizeof(buf), request, device);
  ASSERT_GT(n, 0u);
  EXPECT_LE(n, CONTROL_FRAME_CAP - 16u);  // margin beyond the NUL terminator
}

TEST(VoiceRelayCodec, SessionStartDictationModeIsExact) {
  char buf[512];
  const size_t n = buildSessionStart(buf, sizeof(buf), "r1", "d1", "dictation");
  const std::string expected =
      R"({"type":"session.start","requestId":"r1","deviceId":"d1",)"
      R"("audio":{"sampleRate":16000,"channels":1,"format":"pcm_s16le","frameMs":40},"mode":"dictation"})";
  EXPECT_EQ(str(buf, n), expected);
}

TEST(VoiceRelayCodec, ChatRequestIsExact) {
  char buf[256];
  const size_t n = buildChatRequest(buf, sizeof(buf), "r1", "d1", "你好");
  EXPECT_EQ(str(buf, n), R"({"requestId":"r1","deviceId":"d1","text":"你好","stream":false})");
}

TEST(VoiceRelayCodec, ChatRequestWithDictModeAppendsModeField) {
  char buf[256];
  const size_t n = buildChatRequest(buf, sizeof(buf), "r1", "d1", "踌", "dict");
  EXPECT_EQ(str(buf, n), R"({"requestId":"r1","deviceId":"d1","text":"踌","stream":false,"mode":"dict"})");
}

TEST(VoiceRelayCodec, ChatRequestEscapesText) {
  char buf[256];
  const size_t n = buildChatRequest(buf, sizeof(buf), "r1", "d1", "say \"hi\"\\\n");
  const std::string frame = str(buf, n);
  EXPECT_EQ(frame, R"({"requestId":"r1","deviceId":"d1","text":"say \"hi\"\\\u000a","stream":false})");
  EXPECT_EQ(frame.find('\n'), std::string::npos);  // control bytes never go raw
}

TEST(VoiceRelayCodec, ChatRequestRefusesPartialFrame) {
  char buf[32];
  const std::string tooLong(64, 'x');
  EXPECT_EQ(buildChatRequest(buf, sizeof(buf), "r1", "d1", tooLong.c_str()), 0u);
  EXPECT_EQ(buildChatRequest(nullptr, 64, "r", "d", "x"), 0u);
  EXPECT_EQ(buildChatRequest(buf, sizeof(buf), nullptr, "d", "x"), 0u);
  EXPECT_EQ(buildChatRequest(buf, sizeof(buf), "r", nullptr, "x"), 0u);
  EXPECT_EQ(buildChatRequest(buf, sizeof(buf), "r", "d", nullptr), 0u);
}

TEST(VoiceRelayCodec, ParsesChatResponse) {
  char text[CHAT_TEXT_CAP + 1];
  bool truncated = true;
  ErrorCode error = ErrorCode::Unknown;
  const char* body =
      R"({"requestId":"r1","text":"当然可以，推荐《小王子》。","usage":{"inputTokens":40,"outputTokens":9}})";
  EXPECT_TRUE(parseChatResponse(body, strlen(body), text, sizeof(text), truncated, error));
  EXPECT_STREQ(text, "当然可以，推荐《小王子》。");
  EXPECT_FALSE(truncated);
  EXPECT_EQ(error, ErrorCode::None);
}

TEST(VoiceRelayCodec, ChatResponseEmptyTextIsStillAReply) {
  char text[64];
  bool truncated = false;
  ErrorCode error = ErrorCode::Unknown;
  const char* body = R"({"requestId":"r1","text":""})";
  EXPECT_TRUE(parseChatResponse(body, strlen(body), text, sizeof(text), truncated, error));
  EXPECT_STREQ(text, "");
  EXPECT_EQ(error, ErrorCode::None);
}

TEST(VoiceRelayCodec, ChatResponseLongTextStreamsInParts) {
  std::string reply;
  for (int i = 0; i < 300; ++i) reply += "中";  // 900 bytes > parser token buffer
  const std::string body = R"({"requestId":"r1","text":")" + reply + R"(","usage":{"a":1}})";
  char text[CHAT_TEXT_CAP + 1];
  bool truncated = false;
  ErrorCode error = ErrorCode::None;
  ASSERT_TRUE(parseChatResponse(body.c_str(), body.size(), text, sizeof(text), truncated, error));
  EXPECT_EQ(std::string(text), reply);
  EXPECT_FALSE(truncated);
}

TEST(VoiceRelayCodec, ChatResponseTruncatesAtCodepointBoundary) {
  std::string reply;
  for (int i = 0; i < 20; ++i) reply += "中";  // 60 bytes
  const std::string body = R"({"text":")" + reply + R"("})";
  char text[9];  // 8 usable bytes: two whole 中 plus a cut one
  bool truncated = false;
  ErrorCode error = ErrorCode::None;
  ASSERT_TRUE(parseChatResponse(body.c_str(), body.size(), text, sizeof(text), truncated, error));
  EXPECT_TRUE(truncated);
  EXPECT_EQ(std::string(text), "中中");
}

TEST(VoiceRelayCodec, ChatResponseParsesStrokesPayload) {
  char text[64];
  char strokes[64];
  bool truncated = false;
  ErrorCode error = ErrorCode::None;
  const char* body = R"({"requestId":"r1","text":"【字】的","strokes":"1,2;3,4|5,6","usage":{"a":1}})";
  EXPECT_TRUE(parseChatResponse(body, strlen(body), text, sizeof(text), truncated, error, strokes, sizeof(strokes)));
  EXPECT_STREQ(text, "【字】的");
  EXPECT_STREQ(strokes, "1,2;3,4|5,6");
  EXPECT_FALSE(truncated);
  EXPECT_EQ(error, ErrorCode::None);
}

TEST(VoiceRelayCodec, ChatResponseWithoutStrokesLeavesTheBufferEmpty) {
  char text[64];
  char strokes[64] = "dirty";
  bool truncated = false;
  ErrorCode error = ErrorCode::None;
  const char* body = R"({"text":"word"})";
  EXPECT_TRUE(parseChatResponse(body, strlen(body), text, sizeof(text), truncated, error, strokes, sizeof(strokes)));
  EXPECT_STREQ(strokes, "");
}

TEST(VoiceRelayCodec, ChatResponseIgnoresStrokesWhenNoBufferIsPassed) {
  char text[64];
  bool truncated = false;
  ErrorCode error = ErrorCode::None;
  const char* body = R"({"text":"的","strokes":"1,2;3,4|5,6"})";
  // Defaults keep every pre-stroke call site compiling and crash-free.
  EXPECT_TRUE(parseChatResponse(body, strlen(body), text, sizeof(text), truncated, error));
  EXPECT_STREQ(text, "的");
}

TEST(VoiceRelayCodec, ChatResponseDropsAnOverflowingStrokesPayload) {
  char text[64];
  char strokes[8];
  bool truncated = false;
  ErrorCode error = ErrorCode::None;
  const std::string body = R"({"text":"x","strokes":")" + std::string(20, '1') + R"("})";
  EXPECT_TRUE(
      parseChatResponse(body.c_str(), body.size(), text, sizeof(text), truncated, error, strokes, sizeof(strokes)));
  EXPECT_STREQ(strokes, "");  // a partial payload would draw the wrong glyph
  EXPECT_STREQ(text, "x");
}

TEST(VoiceRelayCodec, ChatResponseLongStrokesStreamInParts) {
  std::string payload;
  payload.reserve(1200);
  for (int i = 0; i < 280; ++i) payload += "9,9;";  // 1119 bytes: > token buffer, < STROKES_CAP
  payload.pop_back();
  const std::string body = R"({"text":"x","strokes":")" + payload + R"("})";
  char text[64];
  char strokes[1152];
  bool truncated = false;
  ErrorCode error = ErrorCode::None;
  ASSERT_TRUE(
      parseChatResponse(body.c_str(), body.size(), text, sizeof(text), truncated, error, strokes, sizeof(strokes)));
  EXPECT_EQ(std::string(strokes), payload);
}

TEST(VoiceRelayCodec, ParsesChatErrorBodies) {
  char text[64];
  bool truncated = false;
  ErrorCode error = ErrorCode::None;

  const char* unauthorized = R"({"error":"UNAUTHORIZED"})";
  EXPECT_FALSE(parseChatResponse(unauthorized, strlen(unauthorized), text, sizeof(text), truncated, error));
  EXPECT_EQ(error, ErrorCode::Unauthorized);

  error = ErrorCode::None;
  const char* llmError = R"({"code":"LLM_RATE_LIMITED"})";
  EXPECT_FALSE(parseChatResponse(llmError, strlen(llmError), text, sizeof(text), truncated, error));
  EXPECT_EQ(error, ErrorCode::LlmRateLimited);

  error = ErrorCode::None;
  const char* disabled = R"({"error":"disabled"})";
  EXPECT_FALSE(parseChatResponse(disabled, strlen(disabled), text, sizeof(text), truncated, error));
  EXPECT_EQ(error, ErrorCode::Unknown);
}

// ------------------------------------------------------------- image (t2i)

TEST(VoiceRelayCodec, ImageRequestBuildsExpectedFrame) {
  char buf[CONTROL_FRAME_CAP];
  const size_t n = buildImageRequest(buf, sizeof(buf), "r1", "reading-companion-001", "画一只水墨猫");
  ASSERT_GT(n, 0u);
  EXPECT_EQ(str(buf, n), R"({"requestId":"r1","deviceId":"reading-companion-001","text":"画一只水墨猫"})");
}

TEST(VoiceRelayCodec, ImageRequestEscapesAndRefusesPartialFrames) {
  char buf[CONTROL_FRAME_CAP];
  const size_t n = buildImageRequest(buf, sizeof(buf), "r1", "d1", "say \"hi\"\\\u000a");
  ASSERT_GT(n, 0u);
  EXPECT_EQ(str(buf, n), R"({"requestId":"r1","deviceId":"d1","text":"say \"hi\"\\\u000a"})");
  EXPECT_EQ(str(buf, n).find('\n'), std::string::npos);  // control bytes never go raw

  char small[32];
  const std::string tooLong(64, 'x');
  EXPECT_EQ(buildImageRequest(small, sizeof(small), "r1", "d1", tooLong.c_str()), 0u);
  EXPECT_EQ(buildImageRequest(nullptr, 64, "r", "d", "x"), 0u);
  EXPECT_EQ(buildImageRequest(buf, sizeof(buf), nullptr, "d", "x"), 0u);
  EXPECT_EQ(buildImageRequest(buf, sizeof(buf), "r", nullptr, "x"), 0u);
  EXPECT_EQ(buildImageRequest(buf, sizeof(buf), "r", "d", nullptr), 0u);
}

TEST(VoiceRelayCodec, ImageRequestCarriesOptionalStyleAndSize) {
  char buf[CONTROL_FRAME_CAP];
  // nullptr / empty keep the historical byte layout (server defaults apply).
  const size_t bare = buildImageRequest(buf, sizeof(buf), "r1", "d1", "山间小屋");
  ASSERT_GT(bare, 0u);
  EXPECT_EQ(str(buf, bare), R"({"requestId":"r1","deviceId":"d1","text":"山间小屋"})");

  const size_t full = buildImageRequest(buf, sizeof(buf), "r1", "d1", "山间小屋", "chinese_ink", "1280*768");
  ASSERT_GT(full, 0u);
  EXPECT_EQ(str(buf, full),
            R"({"requestId":"r1","deviceId":"d1","text":"山间小屋","style":"chinese_ink","size":"1280*768"})");

  const size_t styleOnly = buildImageRequest(buf, sizeof(buf), "r1", "d1", "山间小屋", "sketch", nullptr);
  ASSERT_GT(styleOnly, 0u);
  EXPECT_EQ(str(buf, styleOnly), R"({"requestId":"r1","deviceId":"d1","text":"山间小屋","style":"sketch"})");

  const size_t emptyStyle = buildImageRequest(buf, sizeof(buf), "r1", "d1", "山间小屋", "", "768*1280");
  ASSERT_GT(emptyStyle, 0u);
  EXPECT_EQ(str(buf, emptyStyle), R"({"requestId":"r1","deviceId":"d1","text":"山间小屋","size":"768*1280"})");
}

TEST(VoiceRelayCodec, ImageEditRequestEmbedsBase64Image) {
  char buf[512];
  const size_t n = buildImageEditRequest(buf, sizeof(buf), "r1", "d1", "改成夜景", "QUJDRA==", 480, 800);
  ASSERT_GT(n, 0u);
  EXPECT_EQ(str(buf, n),
            R"({"requestId":"r1","deviceId":"d1","text":"改成夜景","image":{"data":"QUJDRA==","format":"bmp",)"
            R"("width":480,"height":800}})");

  EXPECT_EQ(buildImageEditRequest(buf, sizeof(buf), "r1", "d1", "x", "", 480, 800), 0u);
  EXPECT_EQ(buildImageEditRequest(buf, sizeof(buf), "r1", "d1", "x", "QUJDRA==", 0, 800), 0u);
  EXPECT_EQ(buildImageEditRequest(buf, sizeof(buf), "r1", "d1", "x", "QUJDRA==", 480, -1), 0u);
  EXPECT_EQ(buildImageEditRequest(buf, sizeof(buf), "r1", "d1", "", "QUJDRA==", 480, 800), 0u);
}

TEST(VoiceRelayCodec, ParsesImageResponse) {
  ImageMeta meta;
  char phrase[128];
  ErrorCode error = ErrorCode::Unknown;
  const char* body =
      R"({"requestId":"r1","text":"墨色正在纸上晕开……","image":{"id":"0123456789abcdef","url":"/api/v1/image/0123456789abcdef","width":480,"height":800,"bytes":48062,"format":"bmp"},"usage":{"seconds":9.9}})";
  EXPECT_TRUE(parseImageResponse(body, strlen(body), meta, phrase, sizeof(phrase), error));
  EXPECT_STREQ(meta.id, "0123456789abcdef");
  EXPECT_EQ(meta.width, 480);
  EXPECT_EQ(meta.height, 800);
  EXPECT_EQ(meta.bytes, 48062);
  EXPECT_STREQ(phrase, "墨色正在纸上晕开……");
  EXPECT_EQ(error, ErrorCode::None);
}

TEST(VoiceRelayCodec, ImageResponseErrorBodiesSetT2iCodes) {
  const std::pair<const char*, ErrorCode> cases[] = {
      {"T2I_EMPTY_PROMPT", ErrorCode::T2iEmptyPrompt},
      {"T2I_REJECTED", ErrorCode::T2iRejected},
      {"T2I_NOT_CONFIGURED", ErrorCode::T2iNotConfigured},
      {"T2I_UPSTREAM", ErrorCode::T2iUpstream},
      {"T2I_TIMEOUT", ErrorCode::T2iTimeout},
      {"T2I_BAD_PARAM", ErrorCode::T2iBadParam},
  };
  for (const auto& [code, expected] : cases) {
    const std::string body = std::string(R"({"requestId":"r1","code":")") + code + R"("})";
    ImageMeta meta;
    char phrase[64];
    ErrorCode error = ErrorCode::None;
    EXPECT_FALSE(parseImageResponse(body.c_str(), body.size(), meta, phrase, sizeof(phrase), error)) << code;
    EXPECT_EQ(error, expected) << code;
    EXPECT_STREQ(meta.id, "");
  }
}

TEST(VoiceRelayCodec, ImageResponseWithoutImageObjectFailsCleanly) {
  ImageMeta meta;
  char phrase[64];
  ErrorCode error = ErrorCode::None;
  const char* body = R"({"requestId":"r1","text":"ok"})";
  EXPECT_FALSE(parseImageResponse(body, strlen(body), meta, phrase, sizeof(phrase), error));
  EXPECT_EQ(error, ErrorCode::None);
  EXPECT_STREQ(meta.id, "");
}

TEST(VoiceRelayCodec, ImageResponseLongPhraseStreamsInParts) {
  std::string phrase;
  for (int i = 0; i < 300; ++i) phrase += "中";  // 900 bytes > parser token buffer
  const std::string body =
      R"({"text":")" + phrase + R"(","image":{"id":"0123456789abcdef","width":480,"height":800,"bytes":48062}})";
  ImageMeta meta;
  char text[IMAGE_TEXT_CAP + 1];
  ErrorCode error = ErrorCode::None;
  ASSERT_TRUE(parseImageResponse(body.c_str(), body.size(), meta, text, sizeof(text), error));
  EXPECT_STREQ(meta.id, "0123456789abcdef");
  EXPECT_EQ(strlen(text), 510u);  // 170 full 3-byte codepoints, no half at the end
  EXPECT_EQ(std::string(text + strlen(text) - 3, 3), "中");
}

TEST(VoiceRelayCodec, ImageResponseOversizedIdIsDroppedInFull) {
  ImageMeta meta;
  char phrase[64];
  ErrorCode error = ErrorCode::None;
  // One char past cap: the whole id is refused, never cut to fit.
  const char* body = R"({"image":{"id":"0123456789abcdefg","width":480,"height":800,"bytes":1}})";
  EXPECT_FALSE(parseImageResponse(body, strlen(body), meta, phrase, sizeof(phrase), error));
  EXPECT_STREQ(meta.id, "");
}

TEST(VoiceRelayCodec, ChatAndImageParsersDoNotShareState) {
  char text[64];
  bool truncated = false;
  ErrorCode error = ErrorCode::None;
  const char* image = R"({"text":"x","image":{"id":"0123456789abcdef","width":1,"height":1,"bytes":1}})";
  ImageMeta meta;
  ASSERT_TRUE(parseImageResponse(image, strlen(image), meta, nullptr, 0, error));

  // A chat body parsed after an image body must not carry its fields over.
  const char* chat = R"({"requestId":"r1","text":"reply"})";
  EXPECT_TRUE(parseChatResponse(chat, strlen(chat), text, sizeof(text), truncated, error));
  EXPECT_STREQ(text, "reply");
  EXPECT_EQ(error, ErrorCode::None);
  EXPECT_FALSE(truncated);
}

TEST(VoiceRelayCodec, ScopedFramesAreExact) {
  char buf[256];
  size_t n = buildAudioStart(buf, sizeof(buf), "r1", "s1");
  EXPECT_EQ(str(buf, n), R"({"type":"audio.start","requestId":"r1","sessionId":"s1"})");

  n = buildAudioEnd(buf, sizeof(buf), "r1", "s1");
  EXPECT_EQ(str(buf, n), R"({"type":"audio.end","requestId":"r1","sessionId":"s1","reason":"vad_silence"})");

  n = buildSessionCancel(buf, sizeof(buf), "r1", "s1");
  EXPECT_EQ(str(buf, n), R"({"type":"session.cancel","requestId":"r1","sessionId":"s1","reason":"user_cancel"})");

  n = buildAudioEnd(buf, sizeof(buf), "r1", "s1", "manual");
  EXPECT_EQ(str(buf, n), R"({"type":"audio.end","requestId":"r1","sessionId":"s1","reason":"manual"})");
}

TEST(VoiceRelayCodec, BuildersRefuseToWritePartialFrames) {
  char buf[16];
  EXPECT_EQ(buildSessionStart(buf, sizeof(buf), "r", "d"), 0u);
  EXPECT_EQ(buildAudioStart(buf, sizeof(buf), "r", "s"), 0u);
  EXPECT_EQ(buildSessionStart(nullptr, 64, "r", "d"), 0u);
  EXPECT_EQ(buildAudioStart(buf, sizeof(buf), nullptr, "s"), 0u);
  EXPECT_EQ(buildAudioStart(buf, sizeof(buf), "r", nullptr), 0u);
}

TEST(VoiceRelayCodec, DeviceIdIsJsonEscaped) {
  char buf[256];
  const size_t n = buildSessionStart(buf, sizeof(buf), "r", "a\"b\\c");
  const std::string frame(buf, n);
  EXPECT_NE(frame.find(R"("deviceId":"a\"b\\c")"), std::string::npos);
  EXPECT_EQ(frame.find("\"deviceId\":\"a\"b"), std::string::npos);  // no raw quote escapes into the frame
}

TEST(VoiceRelayCodec, FormatRequestIdIs32LowercaseHex) {
  char buf[REQUEST_ID_CAP];
  formatRequestId(buf, 0xdeadbeef, 0x00c0ffee, 0x12345678, 0x9abcdef0);
  EXPECT_EQ(strlen(buf), 32u);
  EXPECT_STREQ(buf, "deadbeef00c0ffee123456789abcdef0");
}

// ---------------------------------------------------------------- framer

TEST(VoiceRelayCodec, FramerEmitsExactFramesAcrossSplitChunks) {
  std::vector<uint8_t> pcm(3200);  // 100 ms recorder chunk
  for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<uint8_t>(i & 0xFF);
  std::vector<std::vector<uint8_t>> frames;
  PcmFramer framer;
  PcmFramer::FrameFn collect = [](const uint8_t* f, size_t len, void* ctx) {
    static_cast<std::vector<std::vector<uint8_t>>*>(ctx)->emplace_back(f, f + len);
  };

  framer.feed(pcm.data(), pcm.size(), collect, &frames);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(framer.pending(), 640u);

  framer.feed(pcm.data(), pcm.size(), collect, &frames);
  ASSERT_EQ(frames.size(), 5u);
  EXPECT_EQ(framer.pending(), 0u);

  // Concatenation equals the original stream cut at 1280-byte boundaries.
  std::vector<uint8_t> joined;
  for (const auto& f : frames) {
    EXPECT_EQ(f.size(), FRAME_BYTES);
    joined.insert(joined.end(), f.begin(), f.end());
  }
  std::vector<uint8_t> expected;
  for (int i = 0; i < 5; ++i) expected.insert(expected.end(), pcm.begin(), pcm.end());
  ASSERT_EQ(joined.size(), 6400u);
  EXPECT_TRUE(memcmp(joined.data(), expected.data(), joined.size()) == 0);
}

TEST(VoiceRelayCodec, FramerHandlesTinyFeedsAndResets) {
  PcmFramer framer;
  int frames = 0;
  PcmFramer::FrameFn count = [](const uint8_t*, size_t, void* ctx) { ++*static_cast<int*>(ctx); };

  uint8_t byte = 0x7F;
  for (int i = 0; i < 1280; ++i) framer.feed(&byte, 1, count, &frames);
  EXPECT_EQ(frames, 1);
  EXPECT_EQ(framer.pending(), 0u);

  framer.feed(&byte, 1, count, &frames);
  EXPECT_EQ(framer.pending(), 1u);
  framer.reset();
  EXPECT_EQ(framer.pending(), 0u);
  EXPECT_EQ(frames, 1);

  framer.feed(nullptr, 10, count, &frames);
  framer.feed(&byte, 0, count, &frames);
  framer.feed(&byte, 1, nullptr, &frames);
  EXPECT_EQ(frames, 1);
}

// ---------------------------------------------------------------- URL

TEST(VoiceRelayCodec, ParsesRelayUrls) {
  const ParsedUrl ws = parseRelayUrl("ws://192.0.2.10:8080/api/v1/voice/stream");
  ASSERT_TRUE(ws.valid);
  EXPECT_FALSE(ws.tls);
  EXPECT_STREQ(ws.host, "192.0.2.10");
  EXPECT_EQ(ws.port, 8080);
  EXPECT_STREQ(ws.path, "/api/v1/voice/stream");

  const ParsedUrl plain = parseRelayUrl("ws://relay.local");
  ASSERT_TRUE(plain.valid);
  EXPECT_EQ(plain.port, 80);
  EXPECT_STREQ(plain.path, "/");

  const ParsedUrl secure = parseRelayUrl("wss://voice.example.com/relay");
  ASSERT_TRUE(secure.valid);
  EXPECT_TRUE(secure.tls);
  EXPECT_EQ(secure.port, 443);
  EXPECT_STREQ(secure.path, "/relay");
}

TEST(VoiceRelayCodec, RejectsBadUrls) {
  EXPECT_FALSE(parseRelayUrl(nullptr).valid);
  EXPECT_FALSE(parseRelayUrl("").valid);
  EXPECT_FALSE(parseRelayUrl("http://host").valid);
  EXPECT_FALSE(parseRelayUrl("ws://").valid);
  EXPECT_FALSE(parseRelayUrl("ws://host:0").valid);
  EXPECT_FALSE(parseRelayUrl("ws://host:abc").valid);
  EXPECT_FALSE(parseRelayUrl("ws://host:70000").valid);
  EXPECT_FALSE(parseRelayUrl("ws://host:8080 junk").valid);
  EXPECT_FALSE(parseRelayUrl(std::string("ws://" + std::string(80, 'h')).c_str()).valid);
  EXPECT_FALSE(parseRelayUrl(std::string("ws://host/" + std::string(120, 'p')).c_str()).valid);
}

}  // namespace
