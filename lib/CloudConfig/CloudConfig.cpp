#include <CloudConfig.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_err.h>

#include <cstdio>
#include <cstring>

namespace {
// NVS namespace (max 15 chars). Keys below are also max 15 chars.
constexpr const char* kNamespace = "xpcloud";

constexpr const char* kRelayUrl = "relay_url";
constexpr const char* kRelayToken = "relay_tok";
constexpr const char* kRelayDeviceId = "relay_devid";

Preferences prefs;

// NVS stores no distinction between "unset" and "set to empty": a cleared
// field is removed rather than written as "", so an empty form field really
// does erase the previous value instead of pinning it to "".
bool putStr(const char* key, const char* value) {
  if (value == nullptr || value[0] == '\0') {
    // Preferences::remove() reports ESP_ERR_NVS_NOT_FOUND as failure, and a
    // key that was never written is precisely the state we are asking for.
    // Treating that as an error made every save of a card with an empty field
    // (caPath / sttUrl start empty) return "Failed to write to NVS" while the
    // other keys had already been committed — which is why the web UI looked
    // like it did nothing. Only a real erase failure counts against us.
    if (prefs.remove(key)) return true;
    return !prefs.isKey(key);
  }
  return prefs.putString(key, value) > 0;
}

// Returns true when the key existed and was copied (the length returned by
// Preferences includes the NUL terminator, so 0 means "not found").
bool getStr(const char* key, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  return prefs.getString(key, out, cap) > 0;
}
}  // namespace

CloudConfig& CloudConfig::getInstance() {
  static CloudConfig instance;
  return instance;
}

bool CloudConfig::ensureOpen() {
  if (started) return true;
  if (!prefs.begin(kNamespace, false)) {
    LOG_ERR("CLD", "NVS namespace %s unavailable", kNamespace);
    return false;
  }
  started = true;
  return true;
}

bool CloudConfig::load() {
  if (!ensureOpen()) return false;

  getStr(kRelayUrl, relayUrl, sizeof(relayUrl));
  getStr(kRelayToken, relayToken, sizeof(relayToken));
  getStr(kRelayDeviceId, relayDeviceId, sizeof(relayDeviceId));

  const bool haveRelay = relayUrl[0] != '\0';
  LOG_INF("CLD", "loaded: relay=%s relayToken=%s", haveRelay ? relayUrl : "<unset>",
          relayToken[0] != '\0' ? "set" : "<unset>");
  return haveRelay;
}

bool CloudConfig::save() {
  if (!ensureOpen()) return false;

  bool ok = true;
  ok = putStr(kRelayUrl, relayUrl) && ok;
  ok = putStr(kRelayToken, relayToken) && ok;
  ok = putStr(kRelayDeviceId, relayDeviceId) && ok;

  if (!ok) {
    LOG_ERR("CLD", "NVS write failed");
    return false;
  }

  LOG_INF("CLD", "saved: relay=%s", relayUrl[0] != '\0' ? relayUrl : "<unset>");
  return true;
}
