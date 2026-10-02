#include <SttCodec.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace SttCodec;

namespace {

std::string b64Decode(const std::string& in) {
  static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0;
  int valb = -8;
  for (char c : in) {
    if (c == '=') break;
    const size_t p = kAlphabet.find(c);
    if (p == std::string::npos) break;
    val = (val << 6) + static_cast<int>(p);
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

// Header + deterministic PCM filler, exactly what SttClient would upload.
std::vector<uint8_t> makeWav(size_t pcmBytes) {
  std::vector<uint8_t> wav(WAV_HEADER_SIZE + pcmBytes);
  WavHeader h;
  buildWavHeader(h, static_cast<uint32_t>(pcmBytes), DEFAULT_SAMPLE_RATE);
  memcpy(wav.data(), &h, WAV_HEADER_SIZE);
  for (size_t i = 0; i < pcmBytes; ++i) {
    wav[WAV_HEADER_SIZE + i] = static_cast<uint8_t>(i * 31 + 7);
  }
  return wav;
}

constexpr const char* kStream =
    "event: message\r\n"
    "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"},\"finish_reason\":null}]}\r\n"
    "\r\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},\"finish_reason\":null}]}\n"
    "\n"
    "data:{\"choices\":[{\"delta\":{\"content\":\", \"},\"finish_reason\":null}]}\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"world\"},\"finish_reason\":\"stop\"}]}\n"
    "data: [DONE]\n";

}  // namespace

// ── WAV ──────────────────────────────────────────────────────────────────

TEST(WavHeader, FieldValues) {
  WavHeader h;
  buildWavHeader(h, 160000, 16000);

  EXPECT_EQ(memcmp(h.riff, "RIFF", 4), 0);
  EXPECT_EQ(memcmp(h.wave, "WAVE", 4), 0);
  EXPECT_EQ(memcmp(h.fmtId, "fmt ", 4), 0);
  EXPECT_EQ(memcmp(h.dataId, "data", 4), 0);
  EXPECT_EQ(h.fileSize, 44 - 8 + 160000);
  EXPECT_EQ(h.fmtSize, 16u);
  EXPECT_EQ(h.audioFmt, 1u);
  EXPECT_EQ(h.numChannels, 1u);
  EXPECT_EQ(h.sampleRate, 16000u);
  EXPECT_EQ(h.byteRate, 16000u * 2);
  EXPECT_EQ(h.blockAlign, 2u);
  EXPECT_EQ(h.bitsPerSample, 16u);
  EXPECT_EQ(h.dataSize, 160000u);
}

TEST(WavHeader, EmptyCaptureIsValid) {
  WavHeader h;
  buildWavHeader(h, 0, DEFAULT_SAMPLE_RATE);
  EXPECT_EQ(h.fileSize, 36u);
  EXPECT_EQ(h.dataSize, 0u);
  EXPECT_EQ(h.byteRate, DEFAULT_SAMPLE_RATE * 2);
}

// ── base64 ───────────────────────────────────────────────────────────────

TEST(Base64, KnownVectors) {
  struct Case {
    const char* in;
    const char* want;
  };
  static const Case kCases[] = {
      {"", ""},
      {"f", "Zg=="},
      {"fo", "Zm8="},
      {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="},
      {"fooba", "Zm9vYmE="},
      {"foobar", "Zm9vYmFy"},
  };
  for (const auto& c : kCases) {
    char out[64] = {};
    size_t len = 0;
    ASSERT_TRUE(base64Encode(reinterpret_cast<const uint8_t*>(c.in), strlen(c.in), out, sizeof(out), len)) << c.in;
    EXPECT_EQ(std::string(out, len), std::string(c.want)) << c.in;
    EXPECT_EQ(len, strlen(c.want));
    EXPECT_EQ(len, base64EncodedSize(strlen(c.in)));
  }
}

TEST(Base64, BinaryAndPadding) {
  const uint8_t one[] = {0x00};
  const uint8_t two[] = {0xFF, 0xFF};
  const uint8_t three[] = {0xFB, 0xFF, 0xBF};
  char out[16] = {};
  size_t len = 0;

  ASSERT_TRUE(base64Encode(one, 1, out, sizeof(out), len));
  EXPECT_EQ(std::string(out), "AA==");
  ASSERT_TRUE(base64Encode(two, 2, out, sizeof(out), len));
  EXPECT_EQ(std::string(out), "//8=");
  ASSERT_TRUE(base64Encode(three, 3, out, sizeof(out), len));
  EXPECT_EQ(std::string(out), "+/+/");
}

TEST(Base64, RejectsBufferWithoutRoomForNul) {
  const uint8_t in[] = {'f', 'o', 'o'};
  const size_t need = base64EncodedSize(3);  // 4
  char tight[4];
  size_t len = 0;
  EXPECT_FALSE(base64Encode(in, 3, tight, need, len));     // no room for '\0'
  EXPECT_TRUE(base64Encode(in, 3, tight, need + 1, len));  // exactly enough
  EXPECT_EQ(len, 4u);
  EXPECT_STREQ(tight, "Zm9v");

  char oneByte[1];
  EXPECT_FALSE(base64Encode(in, 0, oneByte, 0, len));  // empty input still needs the NUL
}

// ── request body ─────────────────────────────────────────────────────────

TEST(RequestBody, MatchesDeclaredSize) {
  const std::vector<uint8_t> wav = makeWav(160000);
  const size_t declared = requestBodySize(wav.size(), DEFAULT_MODEL);
  ASSERT_GT(declared, 0u);

  std::vector<char> out(declared + 1, static_cast<char>(0xAA));
  const size_t written = buildRequestBody(out.data(), out.size(), DEFAULT_MODEL, wav.data(), wav.size());
  ASSERT_EQ(written, declared) << "declared size and written size disagree";
  EXPECT_EQ(out[written], '\0') << "missing NUL terminator";
}

TEST(RequestBody, RoundTripsWav) {
  const std::vector<uint8_t> wav = makeWav(3000);
  const size_t declared = requestBodySize(wav.size(), DEFAULT_MODEL);
  std::vector<char> out(declared + 1);
  const size_t written = buildRequestBody(out.data(), out.size(), DEFAULT_MODEL, wav.data(), wav.size());
  ASSERT_GT(written, 0u);

  const std::string body(out.data(), written);
  const size_t prefixAt = body.find(DATA_URL_PREFIX);
  ASSERT_NE(prefixAt, std::string::npos);
  const size_t dataAt = prefixAt + strlen(DATA_URL_PREFIX);
  const size_t quoteAt = body.find('"', dataAt);
  ASSERT_NE(quoteAt, std::string::npos);

  const std::string decoded = b64Decode(body.substr(dataAt, quoteAt - dataAt));
  ASSERT_EQ(decoded.size(), wav.size()) << "decoded length mismatch";
  EXPECT_EQ(memcmp(decoded.data(), wav.data(), wav.size()), 0) << "WAV bytes corrupted in transit";
}

TEST(RequestBody, ContainsWireStructure) {
  const std::vector<uint8_t> wav = makeWav(16);
  const size_t declared = requestBodySize(wav.size(), "mimo-v2.5-asr");
  std::vector<char> out(declared + 1);
  const size_t written = buildRequestBody(out.data(), out.size(), "mimo-v2.5-asr", wav.data(), wav.size());
  ASSERT_GT(written, 0u);
  const std::string body(out.data(), written);

  EXPECT_EQ(body.rfind("{\"model\":\"mimo-v2.5-asr\",", 0), 0u);
  EXPECT_NE(body.find("\"type\":\"input_audio\""), std::string::npos);
  EXPECT_NE(body.find("\"asr_options\":{\"language\":\"auto\"}"), std::string::npos);
  EXPECT_NE(body.find("\"stream\":true}"), std::string::npos);
  EXPECT_EQ(body.back(), '}');
}

TEST(RequestBody, RejectsTooSmallBuffer) {
  const std::vector<uint8_t> wav = makeWav(300);
  const size_t declared = requestBodySize(wav.size(), DEFAULT_MODEL);
  std::vector<char> out(declared);  // one byte short of the terminator
  EXPECT_EQ(buildRequestBody(out.data(), out.size(), DEFAULT_MODEL, wav.data(), wav.size()), 0u);
}

TEST(RequestBody, NullArgumentsRejected) {
  const std::vector<uint8_t> wav = makeWav(64);
  char out[4096];
  EXPECT_EQ(buildRequestBody(out, sizeof(out), nullptr, wav.data(), wav.size()), 0u);
  EXPECT_EQ(buildRequestBody(out, sizeof(out), DEFAULT_MODEL, nullptr, wav.size()), 0u);
  EXPECT_EQ(buildRequestBody(nullptr, sizeof(out), DEFAULT_MODEL, wav.data(), wav.size()), 0u);
  EXPECT_EQ(buildRequestBody(out, 0, DEFAULT_MODEL, wav.data(), wav.size()), 0u);
  EXPECT_EQ(requestBodySize(wav.size(), nullptr), 0u);
}

// ── SSE transcript ───────────────────────────────────────────────────────

TEST(Sse, ExtractsTranscriptFromRealisticStream) {
  SseTranscriptParser p;
  EXPECT_TRUE(p.feed(kStream, strlen(kStream)));  // ends with data: [DONE]

  EXPECT_STREQ(p.text(), "Hello, world");
  EXPECT_EQ(p.textSize(), 12u);
  EXPECT_TRUE(p.done());
  EXPECT_EQ(p.eventCount(), 3);  // role-only event contributes no text
  EXPECT_FALSE(p.lineOverflow());
}

TEST(Sse, HandlesArbitraryChunkBoundaries) {
  const size_t total = strlen(kStream);
  for (size_t split = 0; split <= total; ++split) {
    SseTranscriptParser p;
    p.feed(kStream, split);
    p.feed(kStream + split, total - split);
    EXPECT_STREQ(p.text(), "Hello, world") << "split at " << split;
    EXPECT_TRUE(p.done()) << "split at " << split;
    EXPECT_EQ(p.eventCount(), 3) << "split at " << split;
  }
}

TEST(Sse, FinishReasonAloneEndsStream) {
  const char* s = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}\n";
  SseTranscriptParser p;
  EXPECT_TRUE(p.feed(s, strlen(s))) << "finish_reason=stop must end the stream";
  EXPECT_STREQ(p.text(), "hi");
  EXPECT_TRUE(p.done());
}

TEST(Sse, IgnoresNonDataAndCommentLines) {
  const char* s =
      ": ping\n"
      "event: message\n"
      "id: 42\n"
      "retry: 1000\n"
      "data:\n"
      "data: \n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"kept\"},\"finish_reason\":null}]}\n";
  SseTranscriptParser p;
  p.feed(s, strlen(s));
  EXPECT_STREQ(p.text(), "kept");
  EXPECT_EQ(p.eventCount(), 1);
}

TEST(Sse, FlushesFinalLineWithoutTrailingNewline) {
  const char* s = "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\ndata: [DONE]";
  SseTranscriptParser p;
  EXPECT_FALSE(p.feed(s, strlen(s))) << "unterminated [DONE] is not seen yet";
  EXPECT_TRUE(p.finish()) << "[DONE] without a trailing newline must still count";
  EXPECT_STREQ(p.text(), "ok");
}

TEST(Sse, DecodesJsonEscapesInContent) {
  const char* s = "data: {\"choices\":[{\"delta\":{\"content\":\"a\\\"b\\\\c\\td\"}}]}\n";
  SseTranscriptParser p;
  p.feed(s, strlen(s));
  EXPECT_STREQ(p.text(), "a\"b\\c\td");
}

TEST(Sse, IgnoresContentOutsideDelta) {
  // A non-streaming answer would put the text under message.content; we only
  // accept choices[].delta.content, matching the upstream client.
  const char* s = "data: {\"choices\":[{\"message\":{\"content\":\"nope\"}}]}\n";
  SseTranscriptParser p;
  p.feed(s, strlen(s));
  EXPECT_EQ(p.textSize(), 0u);
  EXPECT_EQ(p.eventCount(), 0);
  EXPECT_FALSE(p.done());
}

TEST(Sse, TruncatesTranscriptAtCapacity) {
  const std::string token(400, 'a');
  std::string s = "data: {\"choices\":[{\"delta\":{\"content\":\"" + token + "\"}}]}\n";
  s += "data: {\"choices\":[{\"delta\":{\"content\":\"" + token + "\"}}]}\n";
  s += "data: [DONE]\n";

  SseTranscriptParser p;
  p.feed(s.c_str(), s.size());
  EXPECT_EQ(p.textSize(), TEXT_CAPACITY);
  EXPECT_EQ(p.text()[TEXT_CAPACITY], '\0');
  EXPECT_EQ(p.eventCount(), 2);
  EXPECT_TRUE(p.done());
  EXPECT_EQ(std::string(p.text(), TEXT_CAPACITY), std::string(TEXT_CAPACITY, 'a'));
}

TEST(Sse, DropsOverlongLineAndFlagsIt) {
  std::string s = "data: ";
  s.append(SseTranscriptParser::LINE_CAPACITY + 100, 'x');
  s += "\n";

  SseTranscriptParser p;
  p.feed(s.c_str(), s.size());
  EXPECT_TRUE(p.lineOverflow()) << "over-long line must be flagged";
  EXPECT_EQ(p.textSize(), 0u) << "a truncated line must never be parsed as JSON";
  EXPECT_EQ(p.eventCount(), 0);
  EXPECT_FALSE(p.done());

  // The flag is sticky: finishing the stream must not clear it.
  p.finish();
  EXPECT_TRUE(p.lineOverflow());

  // A later, well-formed line is still accepted.
  const char* next = "data: {\"choices\":[{\"delta\":{\"content\":\"lost\"}}]}\n";
  p.feed(next, strlen(next));
  EXPECT_TRUE(p.lineOverflow());
  EXPECT_STREQ(p.text(), "lost");
}

TEST(Sse, ResetClearsEverything) {
  SseTranscriptParser p;
  p.feed(kStream, strlen(kStream));
  ASSERT_TRUE(p.done());

  p.reset();
  EXPECT_FALSE(p.done());
  EXPECT_EQ(p.textSize(), 0u);
  EXPECT_EQ(p.eventCount(), 0);
  EXPECT_FALSE(p.lineOverflow());
  EXPECT_STREQ(p.text(), "");

  // ...and the parser is reusable afterwards.
  const char* again = "data: {\"choices\":[{\"delta\":{\"content\":\"second\"}}]}\ndata: [DONE]\n";
  p.feed(again, strlen(again));
  EXPECT_STREQ(p.text(), "second");
  EXPECT_TRUE(p.done());
}

// ── streaming request body ───────────────────────────────────────────────

// Head + chunked base64 + tail must be byte-identical to buildRequestBody():
// that is the whole point of the streaming path (a 7 KB working set producing
// the same wire bytes as a 215 KB one-shot body).
std::string streamBody(const std::vector<uint8_t>& wav, const char* model) {
  char head[512];
  const size_t headLen = writeBodyHead(head, sizeof(head), model);
  if (headLen == 0) return "";

  std::string body(head, headLen);

  WavBase64Writer writer;
  WavHeader hdr;
  buildWavHeader(hdr, static_cast<uint32_t>(wav.size() - WAV_HEADER_SIZE), DEFAULT_SAMPLE_RATE);
  if (!writer.begin(&hdr, WAV_HEADER_SIZE, wav.data() + WAV_HEADER_SIZE, wav.size() - WAV_HEADER_SIZE)) return "";

  const char* chunk = nullptr;
  size_t chunkLen = 0;
  while (writer.next(&chunk, &chunkLen)) body.append(chunk, chunkLen);

  char tail[256];
  const size_t tailLen = writeBodyTail(tail, sizeof(tail));
  if (tailLen == 0) return "";
  body.append(tail, tailLen);
  return body;
}

TEST(StreamBody, HeadAndTailAccountForDeclaredSize) {
  const std::vector<uint8_t> wav = makeWav(9001);
  const size_t declared = requestBodySize(wav.size(), DEFAULT_MODEL);

  char head[512];
  const size_t headLen = writeBodyHead(head, sizeof(head), DEFAULT_MODEL);
  char tail[256];
  const size_t tailLen = writeBodyTail(tail, sizeof(tail));
  ASSERT_GT(headLen, 0u);
  ASSERT_GT(tailLen, 0u);
  EXPECT_EQ(headLen + base64EncodedSize(wav.size()) + tailLen, declared);
}

TEST(StreamBody, RejectsNullBuffersAndNullModel) {
  char buf[512];
  EXPECT_EQ(writeBodyHead(buf, sizeof(buf), nullptr), 0u);
  EXPECT_EQ(writeBodyHead(nullptr, sizeof(buf), DEFAULT_MODEL), 0u);
  EXPECT_EQ(writeBodyTail(nullptr, 256), 0u);
  char tiny[4];
  EXPECT_EQ(writeBodyTail(tiny, sizeof(tiny)), 0u);
}

TEST(WavBase64Writer, ConcatenatedChunksMatchOneShotBase64) {
  for (const size_t pcmBytes : {0u, 1u, 2u, 3u, 44u, 2999u, 3000u, 3001u, 44u * 3, 9001u, 40001u}) {
    const std::vector<uint8_t> wav = makeWav(pcmBytes);
    WavHeader hdr;
    buildWavHeader(hdr, static_cast<uint32_t>(pcmBytes), DEFAULT_SAMPLE_RATE);

    WavBase64Writer writer;
    ASSERT_TRUE(writer.begin(&hdr, WAV_HEADER_SIZE, wav.data() + WAV_HEADER_SIZE, pcmBytes)) << pcmBytes;
    EXPECT_EQ(writer.total(), wav.size()) << pcmBytes;

    std::string got;
    int chunks = 0;
    const char* chunk = nullptr;
    size_t chunkLen = 0;
    while (writer.next(&chunk, &chunkLen)) {
      ++chunks;
      ASSERT_LE(chunkLen, WavBase64Writer::CHUNK_OUT) << pcmBytes;
      got.append(chunk, chunkLen);
      // All but the final chunk are whole 3-byte input groups → no '=' padding.
      if (writer.consumed() < writer.total()) {
        EXPECT_EQ(chunkLen % 4, 0u) << pcmBytes;
        EXPECT_EQ(chunkLen, WavBase64Writer::CHUNK_OUT) << pcmBytes;
        EXPECT_NE(chunk[chunkLen - 1], '=') << "unexpected padding mid-stream at " << pcmBytes;
      }
    }

    EXPECT_TRUE(writer.finished()) << pcmBytes;
    EXPECT_EQ(writer.consumed(), wav.size()) << pcmBytes;
    EXPECT_EQ(got.size(), base64EncodedSize(wav.size())) << pcmBytes;
    EXPECT_EQ(chunks, static_cast<int>((wav.size() + WavBase64Writer::CHUNK_IN - 1) / WavBase64Writer::CHUNK_IN))
        << pcmBytes;

    // Round-trip back to bytes: the stream must carry header+PCM intact.
    const std::string decoded = b64Decode(got);
    ASSERT_EQ(decoded.size(), wav.size()) << pcmBytes;
    EXPECT_EQ(memcmp(decoded.data(), wav.data(), wav.size()), 0) << "chunk boundaries corrupted at " << pcmBytes;
    EXPECT_FALSE(writer.next(&chunk, &chunkLen)) << "next() past the end at " << pcmBytes;
  }
}

TEST(WavBase64Writer, ProducesIdenticalRequestBody) {
  for (const size_t pcmBytes : {1u, 44u, 3000u, 160000u}) {
    const std::vector<uint8_t> wav = makeWav(pcmBytes);
    const size_t declared = requestBodySize(wav.size(), DEFAULT_MODEL);
    std::vector<char> oneShot(declared + 1);
    const size_t written = buildRequestBody(oneShot.data(), oneShot.size(), DEFAULT_MODEL, wav.data(), wav.size());
    ASSERT_EQ(written, declared) << pcmBytes;

    const std::string streamed = streamBody(wav, DEFAULT_MODEL);
    ASSERT_FALSE(streamed.empty()) << pcmBytes;
    ASSERT_EQ(streamed.size(), declared) << pcmBytes;
    EXPECT_EQ(streamed, std::string(oneShot.data(), written)) << "streamed body differs at pcm=" << pcmBytes;
  }
}

TEST(WavBase64Writer, RejectsBadArguments) {
  WavBase64Writer writer;
  const char* chunk = nullptr;
  size_t chunkLen = 0;

  // next() before begin()
  EXPECT_FALSE(writer.next(&chunk, &chunkLen));
  EXPECT_FALSE(writer.finished());  // total == 0

  uint8_t pcm[8] = {};
  WavHeader hdr;
  buildWavHeader(hdr, sizeof(pcm), DEFAULT_SAMPLE_RATE);

  EXPECT_FALSE(writer.begin(&hdr, WavBase64Writer::CHUNK_OUT, pcm, sizeof(pcm)));  // header > MAX_HEADER
  EXPECT_FALSE(writer.begin(nullptr, WAV_HEADER_SIZE, pcm, sizeof(pcm)));
  EXPECT_FALSE(writer.begin(&hdr, WAV_HEADER_SIZE, nullptr, sizeof(pcm)));
  ASSERT_TRUE(writer.begin(&hdr, WAV_HEADER_SIZE, pcm, sizeof(pcm)));

  // Arguments are checked before any state is touched.
  EXPECT_FALSE(writer.next(nullptr, &chunkLen));
  EXPECT_FALSE(writer.next(&chunk, nullptr));
}
