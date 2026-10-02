add_executable(ProgressSlotsTest
  ProgressSlotsTest.cpp
  ${REPO_ROOT}/src/activities/reader/ProgressSlots.cpp
)

target_include_directories(ProgressSlotsTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ProgressSlotsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressSlotsTest)
