#include "PigeonDeviceListActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <WifiIdleManager.h>

#include <string>

#include "activities/network/WifiSelectionActivity.h"
#include "activities/pigeon/PigeonConversationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PigeonDeviceListActivity::onEnter() {
  Activity::onEnter();
  WIFI_IDLE.arm();
  WIFI_IDLE.hold();
  state_ = State::Loading;
  started_ = false;
  selectedIndex_ = 0;
  lastRefreshMs_ = 0;
  lastRetryMs_ = millis();
  failText_ = nullptr;

  if (!PIGEON_SERVICE.begin()) {
    state_ = State::Failed;
    failText_ = tr(STR_PIGEON_CONFIG);
    requestUpdate();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("PIGEON", "no network, opening Wi-Fi selection");
    started_ = true;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               state_ = State::Failed;
                               failText_ = tr(STR_PIGEON_FAILED);
                             }
                             started_ = false;
                             requestUpdate();
                           });
    requestUpdate();
    return;
  }
  requestUpdate();
}

void PigeonDeviceListActivity::onExit() {
  WIFI_IDLE.release();
  Activity::onExit();
}

void PigeonDeviceListActivity::load() {
  const PigeonClient::Result result = PIGEON_SERVICE.refresh();
  if (result == PigeonClient::Result::Ok) {
    state_ = State::Ready;
    lastRefreshMs_ = millis();
    if (selectedIndex_ >= static_cast<int>(PIGEON_SERVICE.deviceCount())) selectedIndex_ = 0;
  } else {
    state_ = State::Failed;
    failText_ = tr(STR_PIGEON_FAILED);
    lastRetryMs_ = millis();
    LOG_ERR("PIGEON", "device list load failed: %s", PigeonClient::resultName(result));
  }
  requestUpdate();
}

void PigeonDeviceListActivity::loop() {
  if (state_ == State::Loading && !started_) {
    started_ = true;
    requestUpdateAndWait();
    load();
    return;
  }

  if (state_ == State::Failed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || millis() - lastRetryMs_ >= RETRY_INTERVAL_MS) {
      state_ = State::Loading;
      started_ = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  if (state_ != State::Ready) return;

  if (millis() - lastRefreshMs_ >= REFRESH_INTERVAL_MS) {
    lastRefreshMs_ = millis();
    const PigeonClient::Result result = PIGEON_SERVICE.refresh();
    if (result == PigeonClient::Result::Ok) {
      if (selectedIndex_ >= static_cast<int>(PIGEON_SERVICE.deviceCount())) selectedIndex_ = 0;
      requestUpdate();
    } else {
      LOG_ERR("PIGEON", "heartbeat refresh failed: %s", PigeonClient::resultName(result));
    }
  }

  const int count = static_cast<int>(PIGEON_SERVICE.deviceCount());
  if (count > 0) {
    buttonNavigator_.onNext([this, count] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, count);
      requestUpdate();
    });
    buttonNavigator_.onPrevious([this, count] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, count);
      requestUpdate();
    });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelected();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void PigeonDeviceListActivity::openSelected() {
  if (selectedIndex_ < 0 || static_cast<size_t>(selectedIndex_) >= PIGEON_SERVICE.deviceCount()) return;
  const Pigeon::Device& device = PIGEON_SERVICE.devices()[selectedIndex_];
  LOG_INF("PIGEON", "open conversation with %s (%s)", device.id, device.online ? "online" : "offline");
  activityManager.pushActivity(
      std::make_unique<PigeonConversationActivity>(renderer, mappedInput, device.id, device.name, device.online));
}

void PigeonDeviceListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PIGEON));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const Rect content{0, contentTop, pageWidth, contentHeight};

  if (state_ == State::Loading) {
    GUI.drawPopup(renderer, tr(STR_PIGEON_LOADING));
  } else if (state_ == State::Failed) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2,
                              failText_ != nullptr ? failText_ : tr(STR_PIGEON_FAILED));
  } else if (PIGEON_SERVICE.deviceCount() == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_PIGEON_EMPTY));
  } else {
    GUI.drawList(
        renderer, content, static_cast<int>(PIGEON_SERVICE.deviceCount()), selectedIndex_,
        [](int index) { return std::string(PIGEON_SERVICE.devices()[index].name); },
        [](int index) {
          return std::string(
              I18N.get(PIGEON_SERVICE.devices()[index].online ? StrId::STR_PIGEON_ONLINE : StrId::STR_PIGEON_OFFLINE));
        });
  }

  if (state_ == State::Failed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == State::Ready) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
