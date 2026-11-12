#pragma once

#include <cstddef>
#include <cstdint>

// Credentials and endpoint for the relay-backed cloud features, held in NVS:
//   - Voice Relay (lib/VoiceRelay) -> relayUrl / relayToken / relayDeviceId
// The web UI that edits them is src/network/html/SettingsPage.html
// (GET/POST /api/cloud).
//
// Why a library: PlatformIO does not put src/ on a library's include path, and
// the feature libraries (VoiceRelay, Markets, Pigeon) need these values too —
// same reason lib/NetBootstrap exists. Why NVS and not the SD card: these are
// secrets, the SD card is removable and gets swapped between devices, and
// settings.json on the card is regenerated independently of credentials. NVS is
// wear-levelled, and this store is only written from an explicit save (web
// POST), never from a loop.
//
// Secret fields are exposed to the UI as presence flags only (hasRelayToken),
// mirroring how the Wi-Fi card reports hasPassword.
class CloudConfig {
 public:
  // Matches the relay's device token limit.
  static constexpr size_t TOKEN_SIZE = 192;
  static constexpr size_t RELAY_URL_SIZE = 192;
  // Matches VoiceRelay::DEVICE_ID_CAP on the device side.
  static constexpr size_t RELAY_DEVICE_ID_SIZE = 40;

  // Voice Relay (server-side ASR + LLM). An empty url means the feature is
  // unconfigured and every relay-backed screen reports it instead of running.
  // relayToken is a secret (presence flag only in the web API); relayDeviceId
  // is the X-Device-Id the relay authenticates and counts per device.
  char relayUrl[RELAY_URL_SIZE] = {};
  char relayToken[TOKEN_SIZE] = {};
  char relayDeviceId[RELAY_DEVICE_ID_SIZE] = {};

  static CloudConfig& getInstance();

  // Read every key from NVS into the members. Returns true when a relay URL is
  // configured.
  bool load();
  // Write every member back. Only ever called from an explicit user save.
  bool save();

 private:
  CloudConfig() = default;
  bool ensureOpen();

  bool started = false;
};

#define CLOUD_CFG CloudConfig::getInstance()
