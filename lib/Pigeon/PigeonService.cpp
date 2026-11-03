#include "PigeonService.h"

#include <CloudConfig.h>
#include <Logging.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>

PigeonService& PigeonService::getInstance() {
  static PigeonService instance;
  return instance;
}

bool PigeonService::begin() {
  if (!loaded_) {
    CLOUD_CFG.load();
    loaded_ = true;
  }
  const bool ok = configured();
  if (!ok) {
    lastError_ = "NOT_CONFIGURED";
    LOG_ERR("PIGEON", "relay url/token/device id not configured");
  }
  return ok;
}

bool PigeonService::configured() const {
  return CLOUD_CFG.relayUrl[0] != '\0' && CLOUD_CFG.relayToken[0] != '\0' && CLOUD_CFG.relayDeviceId[0] != '\0';
}

const char* PigeonService::deviceId() const { return CLOUD_CFG.relayDeviceId; }

PigeonClient::Result PigeonService::refresh() {
  if (!configured()) {
    lastError_ = "NOT_CONFIGURED";
    return PigeonClient::Result::BadConfig;
  }
  size_t count = 0;
  const PigeonClient::Result result =
      PigeonClient::heartbeat(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId,
                              CLOUD_CFG.relayDeviceId, devices_, Pigeon::MAX_DEVICES, count, unread_);
  deviceCount_ = count;
  lastError_ = result == PigeonClient::Result::Ok ? "" : PigeonClient::resultName(result);
  if (result != PigeonClient::Result::Ok) {
    LOG_ERR("PIGEON", "heartbeat failed: %s", lastError_);
  }
  return result;
}

void PigeonService::nextEventId() {
  snprintf(eventId_, sizeof(eventId_), "%08x%08x%08x%08x", static_cast<unsigned>(esp_random()),
           static_cast<unsigned>(esp_random()), static_cast<unsigned>(esp_random()),
           static_cast<unsigned>(esp_random()));
}

PigeonClient::Result PigeonService::sendText(const char* peerId, const char* text, Pigeon::Message* out) {
  if (!configured()) {
    lastError_ = "NOT_CONFIGURED";
    return PigeonClient::Result::BadConfig;
  }
  nextEventId();
  Pigeon::Message message;
  const PigeonClient::Result result = PigeonClient::sendText(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken,
                                                             CLOUD_CFG.relayDeviceId, peerId, text, eventId_, message);
  lastError_ = result == PigeonClient::Result::Ok ? "" : PigeonClient::resultName(result);
  if (result == PigeonClient::Result::Ok && out != nullptr) *out = message;
  if (result != PigeonClient::Result::Ok) {
    LOG_ERR("PIGEON", "send failed: %s", lastError_);
  }
  return result;
}

PigeonClient::Result PigeonService::fetchInbox(uint32_t afterId, Pigeon::Message* out, size_t cap, size_t& count,
                                               uint32_t& latestId) {
  if (!configured()) {
    lastError_ = "NOT_CONFIGURED";
    return PigeonClient::Result::BadConfig;
  }
  const PigeonClient::Result result = PigeonClient::fetchInbox(
      CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, afterId, out, cap, count, latestId, unread_);
  lastError_ = result == PigeonClient::Result::Ok ? "" : PigeonClient::resultName(result);
  if (result != PigeonClient::Result::Ok) {
    LOG_ERR("PIGEON", "inbox failed: %s", lastError_);
  }
  return result;
}

PigeonClient::Result PigeonService::fetchHistory(const char* peerId, Pigeon::Message* out, size_t cap, size_t& count) {
  if (!configured()) {
    lastError_ = "NOT_CONFIGURED";
    return PigeonClient::Result::BadConfig;
  }
  const PigeonClient::Result result = PigeonClient::fetchHistory(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken,
                                                                 CLOUD_CFG.relayDeviceId, peerId, out, cap, count);
  lastError_ = result == PigeonClient::Result::Ok ? "" : PigeonClient::resultName(result);
  if (result != PigeonClient::Result::Ok) {
    LOG_ERR("PIGEON", "history failed: %s", lastError_);
  }
  return result;
}

PigeonClient::Result PigeonService::ack(const uint32_t* ids, size_t count) {
  if (!configured()) {
    lastError_ = "NOT_CONFIGURED";
    return PigeonClient::Result::BadConfig;
  }
  const PigeonClient::Result result =
      PigeonClient::ack(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, ids, count);
  lastError_ = result == PigeonClient::Result::Ok ? "" : PigeonClient::resultName(result);
  return result;
}

PigeonClient::Result PigeonService::clearHistory(const char* peerId) {
  if (!configured()) {
    lastError_ = "NOT_CONFIGURED";
    return PigeonClient::Result::BadConfig;
  }
  const PigeonClient::Result result =
      PigeonClient::clearHistory(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, peerId);
  lastError_ = result == PigeonClient::Result::Ok ? "" : PigeonClient::resultName(result);
  if (result != PigeonClient::Result::Ok) {
    LOG_ERR("PIGEON", "clear history failed: %s", lastError_);
  }
  return result;
}
