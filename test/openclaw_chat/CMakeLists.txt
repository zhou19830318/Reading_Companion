add_executable(OpenClawChatTest
  OpenClawChatTest.cpp
  ${REPO_ROOT}/lib/OpenClaw/OpenClawChat.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(OpenClawChatTest PRIVATE
  ${REPO_ROOT}/lib/OpenClaw
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(OpenClawChatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpenClawChatTest)
