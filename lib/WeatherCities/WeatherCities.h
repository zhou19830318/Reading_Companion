#pragma once

#include <cstddef>

// Built-in weather-city table for the workbench forecast.
//
// Why a table instead of an IP lookup: every free geolocation service places
// this reader in the wrong city. Measured from a Jiangsu Telecom (Changzhou)
// address: ip-api.com -> Nanjing, ipinfo.io -> Nanjing, ip.sb (IPv4) -> Nanjing
// with coordinates in Henan (34.77,113.72), while ipip.net and ip.sb (IPv6)
// say Changzhou. ip-api's own `as` field even spells out "Changzhou" and its
// city field still says Nanjing. No free service accepts a Chinese city name
// either (open-meteo's geocoder misses 常州/抚州/台北 outright), so "user picks
// a city once" is the only way to get a correct, stable location — and it
// removes the geo HTTP round from the sync entirely.
//
// Data: scripts/gen_weather_cities.py writes WeatherCities.inc — 244
// prefecture-level cities, coordinates from open-meteo's geocoder with an
// OSM/Photon fallback for the four cities that index lacks.
//
// Key: `zh` doubles as the settings key (CrossPointSettings::weatherCityKey).
// It is unique across the table; the pinyin side is not (福州/抚州, 苏州/宿州,
// 泰州/台州, 宜春/伊春 all share a spelling) and so cannot identify a row.
namespace WeatherCities {

struct City {
  const char* zh;  // 中文名 — unique, also the settings key
  const char* en;  // pinyin label for non-Chinese UIs
  float lat;
  float lon;
};

// Row count. Rows are sorted pinyin-wise by `en`, so a picker list can show
// them in this order as-is.
size_t count();

// Row `index`; clamped to the last row when out of range.
const City& at(size_t index);

// Lookup by settings key. nullptr when `key` is null, empty (auto / IP lookup)
// or not in the table (stale key after a table change).
const City* find(const char* key);

// Display label for the current UI: 中文名 in a Chinese UI, pinyin otherwise.
const char* label(const City& city, bool uiIsChinese);

}  // namespace WeatherCities
