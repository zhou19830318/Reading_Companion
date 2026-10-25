add_executable(WordBookFormatTest
  WordBookFormatTest.cpp
)

target_include_directories(WordBookFormatTest PRIVATE
  ${REPO_ROOT}/lib/Learning
)

target_link_libraries(WordBookFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WordBookFormatTest)
