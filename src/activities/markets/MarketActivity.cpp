#include "MarketActivity.h"

#include <Arduino.h>
#include <CloudConfig.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>
#include <WifiIdleManager.h>

#include <cstdio>
#include <variant>

#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* GROUPS[] = {"cn", "hk", "us", "commodity"};
constexpr const char* PERIODS[] = {"1d", "1mo", "1y"};

const char* groupLabel(int group) {
  switch (group) {
    case 0: return tr(STR_MARKET_CN);
    case 1: return tr(STR_MARKET_HK);
    case 2: return tr(STR_MARKET_US);
    default: return tr(STR_MARKET_COMMODITY);
  }
}

const char* periodLabel(int period) {
  switch (period) {
    case 0: return tr(STR_MARKET_PERIOD_1D);
    case 1: return tr(STR_MARKET_PERIOD_1MO);
    default: return tr(STR_MARKET_PERIOD_1Y);
  }
}

const char* errorLabel(Markets::Client::Result result) {
  switch (result) {
    case Markets::Client::Result::Config: return tr(STR_MARKET_CONFIG);
    case Markets::Client::Result::Auth: return tr(STR_MARKET_AUTH);
    case Markets::Client::Result::InvalidCode: return tr(STR_MARKET_BAD_CODE);
    case Markets::Client::Result::Wifi: return tr(STR_MARKET_NO_WIFI);
    default: return tr(STR_MARKET_FAILED);
  }
}
}  // namespace

void MarketActivity::onEnter() {
  Activity::onEnter();
  CLOUD_CFG.load();
  if (!client.begin()) {
    error = Markets::Client::Result::Memory;
    requestUpdate();
    return;
  }
  queueLoad();
}

void MarketActivity::onExit() {
  Activity::onExit();
}

void MarketActivity::queueLoad() {
  RenderLock lock;
  loading = true;
  error = Markets::Client::Result::Ok;
  requestUpdate();
}

void MarketActivity::load() {
  if (!CLOUD_CFG.relayUrl[0] || !CLOUD_CFG.relayToken[0] || !CLOUD_CFG.relayDeviceId[0]) {
    RenderLock lock;
    error = Markets::Client::Result::Config;
    loading = false;
    requestUpdate();
    return;
  }
  if (!WiFi.isConnected()) {
    auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
    if (!wifi) {
      LOG_ERR("MARKET", "OOM: Wi-Fi selection");
      RenderLock lock;
      error = Markets::Client::Result::Memory;
      loading = false;
      requestUpdate();
      return;
    }
    wifiPending = true;
    startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
      RenderLock lock;
      wifiPending = false;
      if (result.isCancelled || !WiFi.isConnected()) {
        error = Markets::Client::Result::Wifi;
        loading = false;
      }
      requestUpdate();
    });
    return;
  }
  // The loading frame never reads data, so HTTP parsing can fill the fixed
  // snapshot without holding the render mutex across a network operation.
  requestUpdateAndWait();
  WIFI_IDLE.arm();
  WIFI_IDLE.hold();
  // Modem sleep parks the radio between DTIM beacons. On a marginal link a lost
  // beacon can stretch the TCP handshake past the client timeout even though
  // the relay answers in milliseconds from the host; the radio is only up for
  // these few requests, so favour the link.
  WiFi.setSleep(false);
  auto result = client.fetch(detail ? nullptr : GROUPS[group], code, PERIODS[period], lookup, data);
  if (result == Markets::Client::Result::Unavailable && WiFi.isConnected()) {
    // An open() or header-read failure on a connected link is the TCP exchange
    // being lost, not a dead relay (healthz answers in ~40 ms from the same
    // network). One silent retry beats sending the user back to press retry.
    LOG_DBG("MARKET", "fetch failed on a connected link; retrying once");
    result = client.fetch(detail ? nullptr : GROUPS[group], code, PERIODS[period], lookup, data);
  }
  WiFi.setSleep(true);
  WIFI_IDLE.release();
  if (result != Markets::Client::Result::Ok) {
    LOG_ERR("MARKET", "fetch failed: result=%d heap=%u", static_cast<int>(result),
            static_cast<unsigned>(ESP.getFreeHeap()));
  }
  {
    RenderLock lock;
    error = result;
    loading = false;
    if (result == Markets::Client::Result::Ok && detail) {
      snprintf(code, sizeof(code), "%s", data.cards[0].id);
      lookup = false;  // Subsequent periods use the canonical, unambiguous id.
    }
    if (selected >= data.count) selected = 0;
  }
  requestUpdate();
}

void MarketActivity::search() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(
      renderer, mappedInput, tr(STR_MARKET_ENTER_CODE), "", Markets::CODE_CAP - 1, InputType::Text);
  if (!keyboard) {
    LOG_ERR("MARKET", "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) return;
    const auto& text = std::get<KeyboardResult>(result.data).text;
    if (text.empty()) return;
    RenderLock lock;
    snprintf(code, sizeof(code), "%s", text.c_str());
    detail = true;
    lookup = true;
    period = 0;
    queueLoad();
  });
}

void MarketActivity::loop() {
  if (wifiPending) return;
  if (loading) {
    load();
    return;
  }
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back)) {
    if (detail) {
      RenderLock lock;
      detail = false;
      lookup = false;
      period = 0;
      queueLoad();
    } else {
      finish();
    }
    return;
  }
  if (!detail && mappedInput.wasReleased(Button::Left)) {
    search();
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    RenderLock lock;
    if (!detail && error == Markets::Client::Result::Ok && data.count) {
      snprintf(code, sizeof(code), "%s", data.cards[selected].id);
      detail = true;
      period = 0;
      lookup = false;
    }
    queueLoad();  // Confirm on a chart or error retries the same request.
    return;
  }
  if (detail) {
    if (mappedInput.wasReleased(Button::Left) || mappedInput.wasReleased(Button::Right)) {
      RenderLock lock;
      period = (period + (mappedInput.wasReleased(Button::Left) ? 2 : 1)) % 3;
      queueLoad();
    }
  } else if (mappedInput.wasReleased(Button::Right)) {
    RenderLock lock;
    group = (group + 1) % 4;
    selected = 0;
    queueLoad();
  } else if (data.count && (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::Down))) {
    RenderLock lock;
    selected = (selected + (mappedInput.wasReleased(Button::Up) ? data.count - 1 : 1)) % data.count;
    requestUpdate();
  }
}

void MarketActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  const int gap = metrics.verticalSpacing;
  const int x = safe.x + metrics.contentSidePadding;
  const int width = safe.width - metrics.contentSidePadding * 2;
  GUI.drawHeader(renderer, Rect{safe.x, safe.y, safe.width, metrics.headerHeight}, tr(STR_MARKETS),
                 detail && error == Markets::Client::Result::Ok ? periodLabel(period) : groupLabel(group));
  const int top = safe.y + metrics.headerHeight + gap;
  const int bottom = safe.y + safe.height - gap;
  if (loading) {
    GUI.drawPopup(renderer, tr(STR_MARKET_LOADING));
  } else if (error != Markets::Client::Result::Ok) {
    GUI.drawHelpText(renderer, Rect{x, top, width, bottom - top}, errorLabel(error));
  } else if (detail && data.count) {
    const int quoteHeight = metrics.listWithSubtitleRowHeight * 2 + gap;
    GUI.drawMarketCard(renderer, Rect{x, top, width, quoteHeight}, data.cards[0], false, false);
    GUI.drawMarketChart(renderer, Rect{x, top + quoteHeight + gap, width, bottom - top - quoteHeight - gap},
                        data.cards[0], period == 0);
  } else {
    const int columns = width > bottom - top ? 2 : 1;
    const int rows = (data.count + columns - 1) / columns;
    const int footer = metrics.listRowHeight;
    if (rows) {
      const int cardWidth = (width - gap * (columns - 1)) / columns;
      // A short group (HK has two cards) must not stretch its cards over the
      // whole screen: cap the row height at the four-row size and leave the
      // remaining space below, so every group keeps the same card shape.
      const int slot = (bottom - top - footer - gap * (rows - 1)) / rows;
      const int fourRows = (bottom - top - footer - gap * 3) / 4;
      const int cardHeight = slot < fourRows ? slot : fourRows;
      for (int i = 0; i < data.count; ++i) {
        GUI.drawMarketCard(renderer,
                           Rect{x + (i % columns) * (cardWidth + gap), top + (i / columns) * (cardHeight + gap),
                                cardWidth, cardHeight},
                           data.cards[i], selected == i);
      }
    }
    GUI.drawHelpText(renderer, Rect{x, bottom - footer, width, footer}, tr(STR_MARKET_DISCLAIMER));
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK),
      error != Markets::Client::Result::Ok ? tr(STR_RETRY) : (detail ? tr(STR_MARKET_REFRESH) : tr(STR_OPEN)),
      detail ? tr(STR_MARKET_PERIOD_PREV) : tr(STR_MARKET_SEARCH),
      detail ? tr(STR_MARKET_PERIOD_NEXT) : tr(STR_MARKET_GROUP));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
