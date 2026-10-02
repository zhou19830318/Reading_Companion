#include <OpenClawRedact.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

using OpenClaw::CREDENTIAL_REDACTED;
using OpenClaw::payloadCarriesCredential;
using OpenClaw::safeEcho;

bool carries(const std::string& s) { return payloadCarriesCredential(s.data(), s.size()); }

// Frames the gateway actually sends, in the shape the firmware receives them.
constexpr char kChallenge[] = R"({"type":"event","event":"connect.challenge","payload":)"
                              R"({"nonce":"5467c1df-d0bc-4be4-b230-7bef402bee38","ts":1790419266853}})";

constexpr char kAuthResponse[] = R"({"type":"res","id":"1","payload":{"hello":"ok","auth":)"
                                 R"({"deviceToken":"4f2c9d1e-6a0b-4a1f-9d5e-1b2c3d4e5f60"}}})";

constexpr char kConnectRequest[] = R"({"type":"req","id":"1","method":"connect","payload":)"
                                   R"({"device":{"deviceId":"c24d1a4a"},"auth":{"token":"sk-live-abc"}}})";

// Server text that merely talks about a token has to stay readable — this is
// the message that identified the token mismatch on the device.
constexpr char kErrorMessage[] =
    "unauthorized: gateway token mismatch (set gateway.remote.token "
    "to match gateway.auth.token)";

TEST(OpenClawRedact, DetectsIssuedDeviceToken) { EXPECT_TRUE(carries(kAuthResponse)); }

TEST(OpenClawRedact, DetectsSharedToken) { EXPECT_TRUE(carries(kConnectRequest)); }

TEST(OpenClawRedact, DetectsCredentialKeysCaseInsensitively) {
  EXPECT_TRUE(carries(R"({"payload":{"accessToken":"abc"}})"));
  EXPECT_TRUE(carries(R"({"payload":{"DeviceToken":"abc"}})"));
}

TEST(OpenClawRedact, DetectsEveryCredKey) {
  EXPECT_TRUE(carries(R"({"password":"abc"})"));
  EXPECT_TRUE(carries(R"({"secret":"abc"})"));
  EXPECT_TRUE(carries(R"({"apiKey":"abc"})"));
  EXPECT_TRUE(carries(R"({"api_key":"abc"})"));
  EXPECT_TRUE(carries(R"({"authorization":"Bearer abc"})"));
  EXPECT_TRUE(carries(R"({"credential":"abc"})"));
}

TEST(OpenClawRedact, ToleratesWhitespaceAroundTheColon) {
  EXPECT_TRUE(carries(R"({"token" : "abc"})"));
  EXPECT_TRUE(carries(R"({"token": "abc"})"));
}

TEST(OpenClawRedact, ChallengeFrameIsNotACredential) {
  EXPECT_FALSE(carries(kChallenge));
  EXPECT_FALSE(carries(R"({"type":"event","event":"health","payload":{"uptime":12}})"));
}

TEST(OpenClawRedact, ErrorMessageThatMentionsATokenStaysReadable) {
  EXPECT_FALSE(carries(kErrorMessage));
  EXPECT_FALSE(carries(R"({"error":"token expired"})"));
}

TEST(OpenClawRedact, KeyWithoutStringValueIsNotACredential) {
  EXPECT_FALSE(carries(R"({"token":123})"));
  EXPECT_FALSE(carries(R"({"tokenCount":4})"));
  EXPECT_FALSE(carries(R"({"token")"));
}

TEST(OpenClawRedact, EmptyInputIsNotACredential) {
  EXPECT_FALSE(payloadCarriesCredential(nullptr, 0));
  EXPECT_FALSE(payloadCarriesCredential("", 0));
}

TEST(OpenClawRedact, SafeEchoPassesPlainPayloadThrough) {
  const OpenClaw::Echo echo = safeEcho(kChallenge, sizeof(kChallenge) - 1);
  EXPECT_EQ(echo.text, kChallenge);
  EXPECT_EQ(echo.len, static_cast<int>(sizeof(kChallenge) - 1));
}

TEST(OpenClawRedact, SafeEchoSubstitutesCredential) {
  const OpenClaw::Echo echo = safeEcho(kAuthResponse, sizeof(kAuthResponse) - 1);
  EXPECT_STREQ(echo.text, CREDENTIAL_REDACTED);
  EXPECT_EQ(echo.len, static_cast<int>(strlen(CREDENTIAL_REDACTED)));
}

TEST(OpenClawRedact, SafeEchoHandlesNullTerminatedStrings) {
  const OpenClaw::Echo plain = safeEcho(kErrorMessage);
  EXPECT_EQ(plain.text, kErrorMessage);
  EXPECT_EQ(plain.len, static_cast<int>(strlen(kErrorMessage)));

  const OpenClaw::Echo empty = safeEcho(static_cast<const char*>(nullptr));
  EXPECT_STREQ(empty.text, "");
  EXPECT_EQ(empty.len, 0);
}

TEST(OpenClawRedact, TruncatedPayloadDoesNotFault) {
  // A frame cut mid-key or mid-value must be handled within n bytes.
  const std::string cut = std::string(kAuthResponse, 40);
  EXPECT_NO_THROW(payloadCarriesCredential(cut.data(), cut.size()));
  const std::string noValue = std::string(R"({"deviceToken":)");
  EXPECT_FALSE(payloadCarriesCredential(noValue.data(), noValue.size()));
}

}  // namespace
