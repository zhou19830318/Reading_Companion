add_executable(VoiceRelayCodecTest
  VoiceRelayCodecTest.cpp
  ${REPO_ROOT}/lib/VoiceRelay/VoiceRelayCodec.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(VoiceRelayCodecTest PRIVATE
  ${REPO_ROOT}/lib/VoiceRelay
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(VoiceRelayCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VoiceRelayCodecTest)
