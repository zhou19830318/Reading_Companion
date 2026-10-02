#include <CloudConfig.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_err.h>

#include <cstdio>
#include <cstring>

namespace {
// NVS namespace (max 15 chars). Keys below are also max 15 chars.
constexpr const char* kNamespace = "xpcloud";

constexpr const char* kOcHost = "oc_host";
constexpr const char* kOcPort = "oc_port";
constexpr const char* kOcPath = "oc_path";
constexpr const char* kOcTls = "oc_tls";
constexpr const char* kOcProto = "oc_proto";
constexpr const char* kOcToken = "oc_token";
constexpr const char* kOcCa = "oc_ca";
constexpr const char* kOcDevToken = "oc_dvtok";
constexpr const char* kOcDevKey = "oc_devkey";
constexpr const char* kSttKey = "stt_key";
constexpr const char* kSttUrl = "stt_url";

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

  getStr(kOcHost, host, sizeof(host));
  getStr(kOcPath, path, sizeof(path));
  getStr(kOcProto, protocol, sizeof(protocol));
  getStr(kOcToken, token, sizeof(token));
  getStr(kOcCa, caPath, sizeof(caPath));
  getStr(kOcDevToken, deviceToken, sizeof(deviceToken));
  getStr(kOcDevKey, deviceKeyHex, sizeof(deviceKeyHex));
  getStr(kSttKey, sttKey, sizeof(sttKey));
  getStr(kSttUrl, sttUrl, sizeof(sttUrl));
  port = prefs.getUShort(kOcPort, 443);
  tls = prefs.getBool(kOcTls, true);

  if (path[0] == '\0') snprintf(path, sizeof(path), "/");
  if (protocol[0] == '\0') snprintf(protocol, sizeof(protocol), "v2.openclaw.io");

  const bool haveOpenClaw = host[0] != '\0';
  const bool haveStt = sttKey[0] != '\0';
  LOG_INF("CLD", "loaded: openclaw=%s stt=%s sttUrl=%s", haveOpenClaw ? host : "<unset>", haveStt ? "set" : "<unset>",
          sttUrl[0] != '\0' ? sttUrl : "<default>");
  return haveOpenClaw || haveStt;
}

bool CloudConfig::save() {
  if (!ensureOpen()) return false;

  bool ok = true;
  ok = putStr(kOcHost, host) && ok;
  ok = putStr(kOcPath, path) && ok;
  ok = putStr(kOcProto, protocol) && ok;
  ok = putStr(kOcToken, token) && ok;
  ok = putStr(kOcCa, caPath) && ok;
  ok = putStr(kOcDevToken, deviceToken) && ok;
  ok = putStr(kOcDevKey, deviceKeyHex) && ok;
  ok = putStr(kSttKey, sttKey) && ok;
  ok = putStr(kSttUrl, sttUrl) && ok;
  ok = (prefs.putUShort(kOcPort, port) > 0) && ok;
  ok = prefs.putBool(kOcTls, tls) && ok;

  if (!ok) {
    LOG_ERR("CLD", "NVS write failed");
    return false;
  }

  LOG_INF("CLD", "saved: openclaw=%s:%u tls=%d", host[0] != '\0' ? host : "<unset>", port, tls ? 1 : 0);
  return true;
}

bool CloudConfig::setDeviceKey(const char* seedHex) {
  if (!ensureOpen() || seedHex == nullptr || seedHex[0] == '\0') return false;
  char current[DEVICE_KEY_SIZE];
  if (getStr(kOcDevKey, current, sizeof(current)) && strcmp(current, seedHex) == 0) return true;
  snprintf(deviceKeyHex, sizeof(deviceKeyHex), "%s", seedHex);
  const bool ok = putStr(kOcDevKey, deviceKeyHex);
  if (!ok) LOG_ERR("CLD", "device key write failed");
  return ok;
}

bool CloudConfig::setDeviceToken(const char* token) {
  if (!ensureOpen() || token == nullptr || token[0] == '\0') return false;
  char current[TOKEN_SIZE];
  if (getStr(kOcDevToken, current, sizeof(current)) && strcmp(current, token) == 0) return true;
  snprintf(deviceToken, sizeof(deviceToken), "%s", token);
  const bool ok = putStr(kOcDevToken, deviceToken);
  if (!ok) LOG_ERR("CLD", "device token write failed");
  return ok;
}
