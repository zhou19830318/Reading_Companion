add_executable(LearningReviewFormatTest
  ReviewFormatTest.cpp
)

target_include_directories(LearningReviewFormatTest PRIVATE
  ${REPO_ROOT}/lib/Learning
)

target_link_libraries(LearningReviewFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LearningReviewFormatTest)
