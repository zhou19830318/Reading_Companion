add_executable(OpenClawRedactTest
  OpenClawRedactTest.cpp
)

target_include_directories(OpenClawRedactTest PRIVATE
  ${REPO_ROOT}/lib/OpenClaw
)

target_link_libraries(OpenClawRedactTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpenClawRedactTest)
