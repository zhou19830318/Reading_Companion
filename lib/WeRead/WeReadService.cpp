#include "WeReadService.h"

#include <CloudConfig.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

WeReadService& WeReadService::getInstance() {
  static WeReadService instance;
  return instance;
}

bool WeReadService::begin() {
  if (!loaded_) {
    CLOUD_CFG.load();
    loaded_ = true;
  }
  const bool ok = configured();
  if (!ok) {
    lastError_ = "NOT_CONFIGURED";
    LOG_ERR("WEREAD", "relay url/token/device id not configured");
  }
  return ok;
}

bool WeReadService::configured() const {
  return CLOUD_CFG.relayUrl[0] != '\0' && CLOUD_CFG.relayToken[0] != '\0' && CLOUD_CFG.relayDeviceId[0] != '\0';
}

WeReadClient::Result WeReadService::syncConfig() {
  if (!configured()) return WeReadClient::Result::BadConfig;

  auto config = makeUniqueNoThrow<WeRead::RelayConfig>();
  if (!config) {
    LOG_ERR("WEREAD", "OOM: relay config");
    return WeReadClient::Result::Oom;
  }
  const WeReadClient::Result result =
      WeReadClient::fetchConfig(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, *config);
  if (result != WeReadClient::Result::Ok) {
    LOG_DBG("WEREAD", "relay config pull skipped: %s", WeReadClient::resultName(result));
    return result;
  }

  bool changed = false;
  if (config->url[0] != '\0' && strcmp(config->url, CLOUD_CFG.relayUrl) != 0) {
    snprintf(CLOUD_CFG.relayUrl, sizeof(CLOUD_CFG.relayUrl), "%s", config->url);
    changed = true;
  }
  if (config->token[0] != '\0' && strcmp(config->token, CLOUD_CFG.relayToken) != 0) {
    snprintf(CLOUD_CFG.relayToken, sizeof(CLOUD_CFG.relayToken), "%s", config->token);
    changed = true;
  }
  if (config->deviceId[0] != '\0' && strcmp(config->deviceId, CLOUD_CFG.relayDeviceId) != 0) {
    snprintf(CLOUD_CFG.relayDeviceId, sizeof(CLOUD_CFG.relayDeviceId), "%s", config->deviceId);
    changed = true;
  }
  if (changed) {
    if (!CLOUD_CFG.save()) {
      LOG_ERR("WEREAD", "relay config NVS save failed");
      return WeReadClient::Result::BadConfig;
    }
    LOG_INF("WEREAD", "relay config updated from server");
  }
  return WeReadClient::Result::Ok;
}

WeReadClient::Result WeReadService::refresh() {
  if (!configured()) {
    lastError_ = "NOT_CONFIGURED";
    return WeReadClient::Result::BadConfig;
  }
  size_t count = 0;
  size_t total = 0;
  const WeReadClient::Result result = WeReadClient::fetchShelf(
      CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, books_, WeRead::MAX_BOOKS, count, total);
  bookCount_ = count;
  total_ = total;
  lastError_ = result == WeReadClient::Result::Ok ? "" : WeReadClient::resultName(result);
  if (result != WeReadClient::Result::Ok) {
    LOG_ERR("WEREAD", "shelf failed: %s", lastError_);
  }
  return result;
}

WeReadClient::Result WeReadService::fetchBook(const char* bookId, WeRead::BookDetail& out) {
  if (!configured()) {
    lastError_ = "NOT_CONFIGURED";
    return WeReadClient::Result::BadConfig;
  }
  const WeReadClient::Result result =
      WeReadClient::fetchBook(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, bookId, out);
  lastError_ = result == WeReadClient::Result::Ok ? "" : WeReadClient::resultName(result);
  if (result != WeReadClient::Result::Ok) {
    LOG_ERR("WEREAD", "book failed: %s", lastError_);
  }
  return result;
}

WeReadClient::Result WeReadService::fetchStats(WeRead::Stats& out) {
  if (!configured()) {
    lastError_ = "NOT_CONFIGURED";
    return WeReadClient::Result::BadConfig;
  }
  const WeReadClient::Result result =
      WeReadClient::fetchStats(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, out);
  lastError_ = result == WeReadClient::Result::Ok ? "" : WeReadClient::resultName(result);
  if (result != WeReadClient::Result::Ok) {
    LOG_ERR("WEREAD", "stats failed: %s", lastError_);
  }
  return result;
}
