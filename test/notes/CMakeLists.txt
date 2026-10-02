add_executable(NoteFormatTest
  NoteFormatTest.cpp
)

target_include_directories(NoteFormatTest PRIVATE
  ${REPO_ROOT}/lib/Notes
)

target_link_libraries(NoteFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(NoteFormatTest)
