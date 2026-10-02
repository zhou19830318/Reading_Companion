add_executable(SttCodecTest
  SttCodecTest.cpp
  ${REPO_ROOT}/lib/voice/SttCodec.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(SttCodecTest PRIVATE
  ${REPO_ROOT}/lib/voice
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(SttCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SttCodecTest)
