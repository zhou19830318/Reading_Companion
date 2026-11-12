#pragma once

#include <cstddef>
#include <cstdint>

// Base64 for the image endpoint's inline payload. Header-only: the only caller
// is VoiceRelayImageClient, and the old device-direct STT path (lib/voice)
// that used to own this helper is gone.
namespace VoiceRelay {
namespace base64 {

// Exact output length for inLen input bytes (padding included), excluding the
// terminating NUL.
constexpr size_t encodedSize(size_t inLen) { return ((inLen + 2) / 3) * 4; }

// Encodes in[0..inLen) into out. outCap must be >= encodedSize(inLen) + 1 for
// the NUL terminator. Returns false when it does not fit (nothing written).
// out is NUL-terminated on success; outLen receives the encoded length.
inline bool encode(const uint8_t* in, size_t inLen, char* out, size_t outCap, size_t& outLen) {
  static constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  if (out == nullptr) return false;
  if (inLen > 0 && in == nullptr) return false;

  const size_t need = encodedSize(inLen);
  if (outCap < need + 1) return false;  // room for payload + NUL

  size_t o = 0;
  size_t i = 0;
  for (; i + 3 <= inLen; i += 3) {
    const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8) | in[i + 2];
    out[o++] = ALPHABET[(v >> 18) & 0x3F];
    out[o++] = ALPHABET[(v >> 12) & 0x3F];
    out[o++] = ALPHABET[(v >> 6) & 0x3F];
    out[o++] = ALPHABET[v & 0x3F];
  }

  const size_t rem = inLen - i;
  if (rem == 1) {
    const uint32_t v = static_cast<uint32_t>(in[i]) << 16;
    out[o++] = ALPHABET[(v >> 18) & 0x3F];
    out[o++] = ALPHABET[(v >> 12) & 0x3F];
    out[o++] = '=';
    out[o++] = '=';
  } else if (rem == 2) {
    const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8);
    out[o++] = ALPHABET[(v >> 18) & 0x3F];
    out[o++] = ALPHABET[(v >> 12) & 0x3F];
    out[o++] = ALPHABET[(v >> 6) & 0x3F];
    out[o++] = '=';
  }

  out[o] = '\0';
  outLen = o;
  return true;
}

}  // namespace base64
}  // namespace VoiceRelay
