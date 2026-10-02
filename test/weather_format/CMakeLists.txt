add_executable(WeatherFormatTest
  WeatherFormatTest.cpp
  ${REPO_ROOT}/lib/OpenClaw/WeatherFormat.cpp
  ${REPO_ROOT}/lib/IpGeo/IpGeo.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(WeatherFormatTest PRIVATE
  ${REPO_ROOT}/lib/OpenClaw
  ${REPO_ROOT}/lib/IpGeo
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(WeatherFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WeatherFormatTest)
