#pragma once

#include <cstddef>
#include <cstdint>

// Credentials and endpoints for the two outbound cloud features, held in NVS:
//   - MiMo cloud STT   (lib/voice/SttClient)  -> sttKey / sttUrl
//   - OpenClaw client  (OpenClawActivity)     -> host / port / path / tls / protocol / token
// The web UI that edits them is src/network/html/SettingsPage.html
// (GET/POST /api/cloud).
//
// Why a library: PlatformIO does not put src/ on a library's include path, and
// lib/voice/SttClient.cpp needs these values too — same reason lib/NetBootstrap
// exists. Why NVS and not the SD card: these are secrets, the SD card is
// removable and gets swapped between devices, and settings.json on the card is
// regenerated independently of credentials. NVS is wear-levelled, and this
// store is only written from an explicit save (web POST), never from a loop.
//
// Secret fields are exposed to the UI as presence flags only (hasToken /
// hasSttKey), mirroring how the Wi-Fi card reports hasPassword.
class CloudConfig {
 public:
  static constexpr size_t HOST_SIZE = 64;
  static constexpr size_t PATH_SIZE = 96;
  static constexpr size_t PROTO_SIZE = 40;
  // 192 matches AIWatch's token[192] / device_token[192]; a shorter buffer
  // silently truncates on write, and a truncated token is a token mismatch
  // the gateway reports without ever mentioning truncation.
  static constexpr size_t TOKEN_SIZE = 192;
  static constexpr size_t STT_KEY_SIZE = 96;
  static constexpr size_t STT_URL_SIZE = 192;
  static constexpr size_t DEVICE_KEY_SIZE = 65;

  // OpenClaw server. host is the only mandatory field; the rest carry the
  // values an empty NVS would want anyway.
  char host[HOST_SIZE] = {};
  uint16_t port = 443;
  char path[PATH_SIZE] = "/";
  bool tls = true;
  char protocol[PROTO_SIZE] = "v2.openclaw.io";
  char token[TOKEN_SIZE] = {};
  // Optional path on the SD card to a PEM root for a self-signed endpoint.
  // Empty means "validate against the ESP-IDF certificate bundle".
  char caPath[PATH_SIZE] = {};

  // OpenClaw device credentials. Both are written by the firmware itself,
  // never by the web form, so they are omitted from GET /api/cloud and
  // ignored in POST /api/cloud — load() before save() is what keeps them.
  //   deviceKeyHex: 64 lowercase hex chars, the ed25519 seed. Deriving the
  //     keypair from it is deterministic, so this single value regenerates
  //     publicKey and deviceId on every boot.
  //   deviceToken:  device-scoped token issued by the gateway in the
  //     authenticated connect response; the fallback when token is empty.
  char deviceToken[TOKEN_SIZE] = {};
  char deviceKeyHex[DEVICE_KEY_SIZE] = {};

  // MiMo cloud speech-to-text.
  char sttKey[STT_KEY_SIZE] = {};
  char sttUrl[STT_URL_SIZE] = {};

  static CloudConfig& getInstance();

  // Read every key from NVS into the members. Returns true when at least one
  // of the two services has a value configured.
  bool load();
  // Write every member back. Only ever called from an explicit user save.
  bool save();

  // Persist one internally generated credential without rewriting the rest of
  // the store — the device key is created by the firmware and the device token
  // arrives from the gateway, so neither comes from a user save. Both are
  // no-ops when the value is already stored, which keeps a reconnect from
  // burning an NVS erase cycle. load() must have run first for the
  // unchanged-check to see the stored value.
  bool setDeviceKey(const char* seedHex);
  bool setDeviceToken(const char* token);

 private:
  CloudConfig() = default;
  bool ensureOpen();

  bool started = false;
};

#define CLOUD_CFG CloudConfig::getInstance()
