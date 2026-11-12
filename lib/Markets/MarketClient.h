#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace Markets {

constexpr size_t MAX_CARDS = 4;
constexpr size_t MAX_POINTS = 96;
constexpr size_t CODE_CAP = 17;

struct Point {
  uint32_t time = 0;
  float price = 0;
};

struct Card {
  char id[CODE_CAP] = {};
  char name[192] = {};
  char currency[9] = {};
  char unit[8] = {};
  char source[12] = {};
  char error[24] = {};
  char timezone[32] = {};
  char timeLabel[20] = {};
  char startLabel[20] = {};
  char endLabel[20] = {};
  double price = 0;
  double change = 0;
  double changePct = 0;
  double previousClose = 0;
  double high = 0;
  double low = 0;
  uint32_t asOf = 0;
  bool available = false;
  bool stale = false;
  bool hasChange = false;
  bool hasPrevious = false;
  bool hasRange = false;
  uint8_t pointCount = 0;
  Point points[MAX_POINTS] = {};
};

// Activity-owned, fixed-size snapshot. No per-point heap allocations.
struct Snapshot {
  uint8_t count = 0;
  Card cards[MAX_CARDS];
};

class Client {
 public:
  enum class Result : uint8_t { Ok, Config, Wifi, Auth, InvalidCode, Unavailable, Memory, InvalidData };
  // Allocated once on screen entry, released with the activity.
  bool begin();
  Result fetch(const char* group, const char* code, const char* period, bool lookup, Snapshot& out);

 private:
  static constexpr size_t BODY_CAP = 12288;
  std::unique_ptr<char[]> body;
};

}  // namespace Markets
