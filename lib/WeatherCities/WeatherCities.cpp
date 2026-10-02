#include "WeatherCities.h"

#include <cstring>

namespace WeatherCities {
namespace {
// clang-format off
// Rows are {"中文名", "pinyin", lat, lon}; generated, keep the script's order.
constexpr City CITIES[] = {
#include "WeatherCities.inc"
};
// clang-format on
static_assert(sizeof(CITIES) / sizeof(CITIES[0]) > 0, "weather-city table is empty");
constexpr size_t CITY_COUNT = sizeof(CITIES) / sizeof(CITIES[0]);
}  // namespace

size_t count() { return CITY_COUNT; }

const City& at(size_t index) {
  if (index >= CITY_COUNT) index = CITY_COUNT - 1;
  return CITIES[index];
}

const City* find(const char* key) {
  // Empty means "auto" (IP lookup) and is the settings default, so it must not
  // scan the table — it would just match nothing anyway, only slower.
  if (key == nullptr || key[0] == '\0') return nullptr;
  for (const City& city : CITIES) {
    if (std::strcmp(city.zh, key) == 0) return &city;
  }
  return nullptr;
}

const char* label(const City& city, bool uiIsChinese) { return uiIsChinese ? city.zh : city.en; }

}  // namespace WeatherCities
