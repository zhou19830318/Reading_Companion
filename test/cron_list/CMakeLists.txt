add_executable(CronListTest
  CronListTest.cpp
  ${REPO_ROOT}/lib/OpenClaw/CronListFormat.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(CronListTest PRIVATE
  ${REPO_ROOT}/lib/OpenClaw
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(CronListTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CronListTest)
