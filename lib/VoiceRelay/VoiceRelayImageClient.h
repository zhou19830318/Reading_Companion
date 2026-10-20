#pragma once

#include <cstddef>
#include <cstdint>

#include "VoiceRelayCodec.h"  // ImageMeta

// /api/v1/image round trip on the relay: the AI 生图 screen's transport when
// the voice-relay setting is on. Same origin derivation, auth headers (bearer
// + X-Device-Id) and blocking contract as ChatClient. Two calls run back to
// back from the activity loop: generate() holds the POST for as long as the
// provider needs to render, download() streams the finished asset straight to
// the SD card. edit() is the 修改 flow: the panel BMP travels inside the POST
// as base64 and the provider renders an image-to-image result.
namespace VoiceRelay {

class ImageClient {
 public:
  enum class Result : uint8_t {
    Ok,
    BadConfig,  // url/token/device missing, request did not fit, or 404 (endpoint disabled)
    NoWifi,
    Time,  // TLS needs a sane clock and SNTP did not settle
    Connect,
    Http,           // non-200 with no more specific class
    Auth,           // 401 or an UNAUTHORIZED / DEVICE_NOT_ALLOWED body
    Rejected,       // T2I_REJECTED — the provider refused the prompt
    NotConfigured,  // T2I_NOT_CONFIGURED — server has no image backend
    Upstream,       // T2I_UPSTREAM / provider-class failure / server 5xx
    Timeout,        // T2I_TIMEOUT (server) or a read timeout on the wire
    Read,
    Oom,
    Download,  // asset GET failed; any partial file has been removed
    File,      // SD write failed; any partial file has been removed
    NoImage,   // 200 without an image object
  };

  struct Stats {
    int httpStatus = 0;
    size_t responseBytes = 0;
  };

  // POST <origin>/api/v1/image. On Ok, `meta` is the stored asset and
  // `phrase` (optional) the waiting phrase. One image can take the provider
  // tens of seconds and the server holds the request open — the caller
  // paints a static waiting frame first, exactly like ChatClient::ask.
  // `style` / `size` (optional) are the DashScope V1 style enum value and a
  // "W*H" size override; nullptr/empty keeps the server defaults.
  static Result generate(const char* url, const char* token, const char* deviceId, const char* text, ImageMeta& meta,
                         char* phrase, size_t phraseCap, Stats* stats = nullptr, const char* style = nullptr,
                         const char* size = nullptr);

  // Edit mode (设备 修改): reads the BMP at `imagePath` from the SD card,
  // base64-encodes it into the request, and asks the relay for an
  // image-to-image render of `text` on top of it. Same blocking/waiting
  // contract and response shape as generate().
  static Result edit(const char* url, const char* token, const char* deviceId, const char* text, const char* imagePath,
                     ImageMeta& meta, char* phrase, size_t phraseCap, Stats* stats = nullptr);

  // GET <origin>/api/v1/image/<meta.id> streamed to `path`. The caller
  // ensures the parent directory exists. The read buffer is small and each
  // chunk is written as it arrives — no 48 KB staging buffer. On any failure
  // the partial file is removed, so `path` exists only when Ok is returned.
  static Result download(const char* url, const char* token, const char* deviceId, const ImageMeta& meta,
                         const char* path);

  static const char* resultName(Result result);
};

}  // namespace VoiceRelay
