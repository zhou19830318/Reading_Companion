#pragma once

#include <I18n.h>
#include <WeReadClient.h>

#include <cstdio>

namespace WeReadUi {

inline const char* errorText(WeReadClient::Result result) {
  switch (result) {
    case WeReadClient::Result::NotConfigured:
      return tr(STR_WEREAD_SERVER_KEY);
    case WeReadClient::Result::KeyInvalid:
      return tr(STR_WEREAD_KEY_INVALID);
    case WeReadClient::Result::Auth:
      return tr(STR_WEREAD_RELAY_AUTH);
    case WeReadClient::Result::NoWifi:
      return tr(STR_WEREAD_NO_WIFI);
    case WeReadClient::Result::RateLimited:
      return tr(STR_WEREAD_RATE_LIMITED);
    case WeReadClient::Result::BadConfig:
      return tr(STR_WEREAD_CONFIG);
    case WeReadClient::Result::Ok:
      return "";
    default:
      return tr(STR_WEREAD_FAILED);
  }
}

inline void formatDuration(uint32_t seconds, char* out, size_t cap) {
  const uint32_t totalMinutes = seconds / 60;
  const uint32_t hours = totalMinutes / 60;
  const uint32_t minutes = totalMinutes % 60;
  if (hours > 0) {
    snprintf(out, cap, "%u %s %u %s", static_cast<unsigned>(hours), tr(STR_WEREAD_HOURS),
             static_cast<unsigned>(minutes), tr(STR_WEREAD_MINUTES));
  } else {
    snprintf(out, cap, "%u %s", static_cast<unsigned>(minutes), tr(STR_WEREAD_MINUTES));
  }
}

}  // namespace WeReadUi
