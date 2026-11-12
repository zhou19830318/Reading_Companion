#include "WeReadStatsActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <WeReadService.h>
#include <WiFi.h>
#include <WifiIdleManager.h>

#include "WeReadUi.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WeReadStatsActivity::onEnter() {
  Activity::onEnter();
  WIFI_IDLE.arm();
  WIFI_IDLE.hold();
  state_ = State::Loading;
  started_ = false;
  failText_ = nullptr;
  stats_ = WeRead::Stats{};

  if (!WEREAD_SERVICE.begin()) {
    state_ = State::Failed;
    failText_ = tr(STR_WEREAD_CONFIG);
    requestUpdate();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    started_ = true;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                               state_ = State::Failed;
                               failText_ = tr(STR_WEREAD_FAILED);
                             }
                             started_ = false;
                             requestUpdate();
                           });
    requestUpdate();
    return;
  }
  requestUpdate();
}

void WeReadStatsActivity::onExit() {
  WIFI_IDLE.release();
  Activity::onExit();
}

void WeReadStatsActivity::load() {
  const WeReadClient::Result result = WEREAD_SERVICE.fetchStats(stats_);
  if (result == WeReadClient::Result::Ok) {
    state_ = State::Ready;
  } else {
    state_ = State::Failed;
    failText_ = WeReadUi::errorText(result);
    LOG_ERR("WEREAD", "stats load failed: %s", WeReadClient::resultName(result));
  }
  requestUpdate();
}

void WeReadStatsActivity::loop() {
  if (state_ == State::Loading && !started_) {
    started_ = true;
    requestUpdateAndWait();
    load();
    return;
  }

  if (state_ == State::Failed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state_ = State::Loading;
      started_ = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  if (state_ == State::Ready && mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
}

void WeReadStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WEREAD_STATS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int x = metrics.contentSidePadding;
  const int right = pageWidth - metrics.contentSidePadding;

  if (state_ == State::Loading) {
    GUI.drawPopup(renderer, tr(STR_WEREAD_STATS_LOADING));
  } else if (state_ == State::Failed) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2,
                              failText_ != nullptr ? failText_ : tr(STR_WEREAD_STATS_FAILED));
  } else {
    char total[48] = {};
    char average[48] = {};
    WeReadUi::formatDuration(stats_.totalReadTime, total, sizeof(total));
    WeReadUi::formatDuration(stats_.dayAverageReadTime, average, sizeof(average));

    char days[32] = {};
    snprintf(days, sizeof(days), "%u %s", static_cast<unsigned>(stats_.readDays), tr(STR_WEREAD_DAYS));

    int y = contentTop + 40;
    const int rowGap = 56;
    auto drawRow = [&](const char* label, const char* value, int rowY) {
      renderer.drawText(UI_10_FONT_ID, x, rowY, label, true);
      const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, right - valueWidth, rowY, value, true);
    };
    drawRow(tr(STR_WEREAD_TOTAL_TIME), total, y);
    drawRow(tr(STR_WEREAD_READ_DAYS), days, y + rowGap);
    drawRow(tr(STR_WEREAD_DAILY_AVG), average, y + rowGap * 2);

    renderer.drawCenteredText(SMALL_FONT_ID, contentTop + contentHeight - 24, tr(STR_WEREAD_READONLY));
  }

  if (state_ == State::Failed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
