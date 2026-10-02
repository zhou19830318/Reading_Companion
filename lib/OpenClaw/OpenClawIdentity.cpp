#include "OpenClawIdentity.h"

#include <ed25519.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>

#include <cstring>

namespace OpenClaw {

bool identityFromSeedHex(const char* seedHex, Identity& out) {
  out = Identity{};

  uint8_t seed[32];
  if (!decodeHexSeed(seedHex, seed)) return false;

  ed25519_create_keypair(out.publicKey, out.privateKey, seed);

  // deviceId = SHA-256(raw 32-byte public key), hex, lowercase
  // (openclaw_client.c:1307-1311). mbedtls's one-shot entry point changed
  // signature across versions, so use the streaming API exactly like
  // src/network/FirmwareFlasher.cpp does.
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  const bool ok = mbedtls_sha256_starts(&ctx, /*is224=*/0) == 0 &&
                  mbedtls_sha256_update(&ctx, out.publicKey, sizeof(out.publicKey)) == 0 &&
                  mbedtls_sha256_finish(&ctx, hash) == 0;
  mbedtls_sha256_free(&ctx);
  if (!ok) return false;

  hexEncode32(out.deviceId, hash);
  out.valid = true;
  return true;
}

bool generateSeedHex(char* out, size_t cap) {
  if (out == nullptr || cap < 65) return false;
  uint8_t seed[32];
  esp_fill_random(seed, sizeof(seed));
  hexEncode32(out, seed);
  return true;
}

}  // namespace OpenClaw
