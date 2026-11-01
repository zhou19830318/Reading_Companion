add_executable(PomodoroCodecTest
  PomodoroCodecTest.cpp
  ${REPO_ROOT}/lib/VoiceRelay/VoiceRelayCodec.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(PomodoroCodecTest PRIVATE
  ${REPO_ROOT}/lib/VoiceRelay
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(PomodoroCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PomodoroCodecTest)
