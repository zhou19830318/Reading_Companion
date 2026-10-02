#include <WeatherCities.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string>

using WeatherCities::at;
using WeatherCities::City;
using WeatherCities::count;
using WeatherCities::find;
using WeatherCities::label;

namespace {

std::string lower(const char* s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
  return out;
}

// 244 prefecture cities: the number scripts/gen_weather_cities.py wrote. The
// generation run reports it too, so a mismatch here means the .inc and the
// hand-written table contract drifted apart.
constexpr size_t kExpectedRows = 244;

TEST(WeatherCities, TableHasTheGeneratedRowCount) { EXPECT_EQ(count(), kExpectedRows); }

TEST(WeatherCities, EveryRowIsWellFormed) {
  for (size_t i = 0; i < count(); ++i) {
    const City& c = at(i);
    ASSERT_NE(c.zh, nullptr);
    ASSERT_NE(c.en, nullptr);
    EXPECT_STRNE(c.zh, "");
    EXPECT_STRNE(c.en, "");
    EXPECT_GE(c.lat, -90.0f);
    EXPECT_LE(c.lat, 90.0f);
    EXPECT_GE(c.lon, -180.0f);
    EXPECT_LE(c.lon, 180.0f);
    // 中文名 only — the settings field is char[16] and the longest key
    // (呼伦贝尔) is 12 bytes.
    EXPECT_LE(std::strlen(c.zh), 15u);
  }
}

TEST(WeatherCities, KeysAreUnique) {
  std::set<std::string> keys;
  for (size_t i = 0; i < count(); ++i) keys.insert(at(i).zh);
  EXPECT_EQ(keys.size(), count());
}

TEST(WeatherCities, RowsAreSortedPinyinWiseSoAPickerNeedsNoSort) {
  for (size_t i = 1; i < count(); ++i) {
    // `<=` because the four pinyin collisions tie here (the generator breaks
    // those ties by 中文名, which the picker does not depend on).
    EXPECT_LE(lower(at(i - 1).en), lower(at(i).en)) << "rows " << i - 1 << " and " << i << " out of order";
  }
}

// The pinyin side is why `zh` is the settings key: 福州/抚州, 苏州/宿州,
// 泰州/台州 and 宜春/伊春 share a spelling, so `en` cannot identify a row.
TEST(WeatherCities, PinyinCollisionsExistAndOnlyOnTheEnglishSide) {
  std::set<std::string> pinyin;
  size_t collisions = 0;
  for (size_t i = 0; i < count(); ++i) {
    const std::string en = lower(at(i).en);
    if (!pinyin.insert(en).second) ++collisions;
  }
  EXPECT_EQ(collisions, 4u);
}

TEST(WeatherCities, FindsAPinnedCityByItsChineseKey) {
  const City* c = find("常州");
  ASSERT_NE(c, nullptr);
  EXPECT_STREQ(c->en, "Changzhou");
  EXPECT_NEAR(c->lat, 31.7736f, 0.05f);
  EXPECT_NEAR(c->lon, 119.9540f, 0.05f);
}

// Weather was forecast for Nanjing under an IP lookup while the reader sits in
// Changzhou; the pinned key must find Changzhou, never fall through to another
// row.
TEST(WeatherCities, ASecondJiangsuCityResolvesToItsOwnCoordinates) {
  const City* nanjing = find("南京");
  ASSERT_NE(nanjing, nullptr);
  const City* changzhou = find("常州");
  ASSERT_NE(changzhou, nullptr);
  EXPECT_NEAR(changzhou->lat - nanjing->lat, -0.2881f, 0.05f);
}

TEST(WeatherCities, EmptyOrNullKeyMeansAutoAndDoesNotMatch) {
  EXPECT_EQ(find(""), nullptr);
  EXPECT_EQ(find(nullptr), nullptr);
}

TEST(WeatherCities, UnknownKeyIsRejected) {
  EXPECT_EQ(find("不存在的城市"), nullptr);
  EXPECT_EQ(find("Springfield"), nullptr);
}

TEST(WeatherCities, OutOfRangeIndexClampsToTheLastRow) {
  EXPECT_STRNE(at(count()).zh, "");
  EXPECT_STRNE(at(count() + 100).zh, "");
}

TEST(WeatherCities, LabelFollowsTheUiLanguage) {
  const City* c = find("常州");
  ASSERT_NE(c, nullptr);
  EXPECT_STREQ(label(*c, true), "常州");
  EXPECT_STREQ(label(*c, false), "Changzhou");
}

}  // namespace
