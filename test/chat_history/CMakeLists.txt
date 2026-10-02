add_executable(ChatHistoryFormatTest
  ChatHistoryFormatTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ChatHistoryFormatTest PRIVATE
  ${REPO_ROOT}/lib/OpenClaw
  ${REPO_ROOT}/lib/Utf8
)

target_include_directories(ChatHistoryFormatTest PRIVATE
  ${REPO_ROOT}/lib/OpenClaw
)

target_link_libraries(ChatHistoryFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChatHistoryFormatTest)
