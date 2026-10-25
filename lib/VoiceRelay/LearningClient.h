#pragma once

#include <cstddef>
#include <cstdint>

#include "VoiceRelayCodec.h"  // AddWordsResult, LEARNING_CHUNK_WORDS

// /api/v1/learning word-book sync on the relay: the learning screens'
// transport when the voice-relay setting is on. Same origin derivation
// (ws:// URL -> http:// origin), auth headers (bearer + X-Device-Id) and
// blocking contract as ChatClient — the caller paints a waiting frame first
// and chunks batches by LEARNING_CHUNK_WORDS. Request bytes and response
// parsing live in the host-tested codec (test/learning_codec).
namespace VoiceRelay {

class LearningClient {
 public:
  enum class Result : uint8_t {
    Ok,
    BadConfig,  // url/token/device missing, request did not fit, or 400/404
    NoWifi,
    Time,  // TLS needs a sane clock and SNTP did not settle
    Connect,
    Http,      // non-200 with no more specific class
    Auth,      // 401 or an UNAUTHORIZED / DEVICE_NOT_ALLOWED body
    Upstream,  // 5xx or a server-overload class
    Read,
    Oom,
    NoData,  // 200 without an ok field
  };

  struct Stats {
    int httpStatus = 0;
    size_t responseBytes = 0;
  };

  // POST <origin>/api/v1/learning/words. `words[i]` must be non-empty; the
  // caller chunks (the relay rejects batches over LEARNING_MAX_WORDS_PER_
  // REQUEST with 400). On Ok, `added`/`accepted` mirror the server echo when
  // the optional out-pointers are passed.
  static Result addWords(const char* url, const char* token, const char* deviceId, const char* const* words,
                         size_t count, const char* source = "lookup", int32_t* added = nullptr,
                         int32_t* accepted = nullptr, Stats* stats = nullptr);

  static const char* resultName(Result result);
};

}  // namespace VoiceRelay
