#pragma once

#include <cstddef>

#include "OpenClawHandshake.h"

// Device identity generation. This header is device-only: the implementation
// needs mbedtls (SHA-256) and esp_hw_support (hardware RNG), neither of which
// exists in the host test build. The portable half of the pipeline — hex
// decode, keypair derivation, deviceId formatting — lives in
// OpenClawHandshake.{h,cpp} and is covered by test/openclaw_handshake.
namespace OpenClaw {

// Decode a 64-char hex seed, derive the ed25519 keypair, and set deviceId to
// SHA-256(publicKey) as lowercase hex — the exact identity string the server
// checks against the device registry. Returns false (leaving out.valid false)
// when the seed is not 64 hex characters.
bool identityFromSeedHex(const char* seedHex, Identity& out);

// 32 bytes of hardware entropy as a NUL-terminated 65-char lowercase hex
// seed. Returns false if cap < 65.
bool generateSeedHex(char* out, size_t cap);

}  // namespace OpenClaw
