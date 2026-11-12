#pragma once

#include <cstddef>
#include <cstdint>

// HTTP transport for the relay's WeRead read-only endpoints.
//
// The relay owns the official Skill API key and returns a small, device-shaped
// JSON view. The device never talks to i.weread.qq.com directly and never sees
// the wrk- key. Every call is blocking; the activity paints a waiting frame
// first, matching the Pigeon/Learning clients.
namespace WeRead {

constexpr size_t BOOK_ID_CAP = 32;
constexpr size_t TITLE_CAP = 96;
constexpr size_t AUTHOR_CAP = 64;
constexpr size_t READER_LINK_CAP = 256;
constexpr size_t RELAY_URL_CAP = 192;
constexpr size_t RELAY_TOKEN_CAP = 192;
constexpr size_t RELAY_DEVICE_ID_CAP = 40;
constexpr size_t MAX_BOOKS = 12;

struct Book {
  char bookId[BOOK_ID_CAP] = {};
  char title[TITLE_CAP] = {};
  char author[AUTHOR_CAP] = {};
  bool finished = false;
  uint32_t lastRead = 0;
};

struct BookDetail {
  char bookId[BOOK_ID_CAP] = {};
  char title[TITLE_CAP] = {};
  char author[AUTHOR_CAP] = {};
  char readerLink[READER_LINK_CAP] = {};
  uint8_t progress = 0;
  bool finished = false;
  uint32_t readingTime = 0;
  uint32_t finishTime = 0;
};

struct Stats {
  uint32_t totalReadTime = 0;
  uint16_t readDays = 0;
  uint32_t dayAverageReadTime = 0;
};

// Server-managed relay credentials. Empty fields mean "keep the current NVS
// value"; the relay returns the token only when it has a registry record.
struct RelayConfig {
  char url[RELAY_URL_CAP] = {};
  char token[RELAY_TOKEN_CAP] = {};
  char deviceId[RELAY_DEVICE_ID_CAP] = {};
};

class WeReadClient {
 public:
  enum class Result : uint8_t {
    Ok,
    BadConfig,  // url/token/device/book id missing or malformed
    NoWifi,
    Time,  // TLS needs a sane clock and SNTP did not settle
    Connect,
    Http,           // non-2xx with no more specific class
    Auth,           // relay token rejected (401/403)
    NotConfigured,  // relay has no WeRead key
    KeyInvalid,     // official gateway rejected the key
    RateLimited,    // 429 from relay or gateway
    Upstream,       // relay/gateway 5xx
    Read,
    Oom,
    NoData,  // 2xx without the expected fields
  };

  static Result fetchShelf(const char* url, const char* token, const char* deviceId, Book* out, size_t cap,
                           size_t& count, size_t& total);

  static Result fetchBook(const char* url, const char* token, const char* deviceId, const char* bookId,
                          BookDetail& out);

  static Result fetchStats(const char* url, const char* token, const char* deviceId, Stats& out);

  static Result fetchConfig(const char* url, const char* token, const char* deviceId, RelayConfig& out);

  static const char* resultName(Result result);
};

}  // namespace WeRead

using WeReadClient = WeRead::WeReadClient;
