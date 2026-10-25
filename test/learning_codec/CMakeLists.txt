add_executable(LearningCodecTest
  LearningCodecTest.cpp
  ${REPO_ROOT}/lib/VoiceRelay/VoiceRelayCodec.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(LearningCodecTest PRIVATE
  ${REPO_ROOT}/lib/VoiceRelay
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(LearningCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LearningCodecTest)
