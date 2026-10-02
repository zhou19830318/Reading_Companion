add_executable(WeatherCitiesTest
  WeatherCitiesTest.cpp
  ${REPO_ROOT}/lib/WeatherCities/WeatherCities.cpp
)

target_include_directories(WeatherCitiesTest PRIVATE
  ${REPO_ROOT}/lib/WeatherCities
)

target_link_libraries(WeatherCitiesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WeatherCitiesTest)
