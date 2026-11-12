#include "WeReadBookActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <WeReadService.h>
#include <WiFi.h>
#include <WifiIdleManager.h>

#include "WeReadUi.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WeReadBookActivity::onEnter() {
  Activity::onEnter();
  WIFI_IDLE.arm();
  WIFI_IDLE.hold();
  state_ = State::Loading;
  started_ = false;
  failText_ = nullptr;
  detail_ = WeRead::BookDetail{};

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

void WeReadBookActivity::onExit() {
  WIFI_IDLE.release();
  Activity::onExit();
}

void WeReadBookActivity::load() {
  const WeReadClient::Result result = WEREAD_SERVICE.fetchBook(book_.bookId, detail_);
  if (result == WeReadClient::Result::Ok) {
    state_ = State::Ready;
  } else {
    state_ = State::Failed;
    failText_ = WeReadUi::errorText(result);
    LOG_ERR("WEREAD", "book load failed: %s", WeReadClient::resultName(result));
  }
  requestUpdate();
}

void WeReadBookActivity::loop() {
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

  if (state_ != State::Ready) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && detail_.readerLink[0] != '\0') {
    openQr();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
}

void WeReadBookActivity::openQr() {
  LOG_INF("WEREAD", "show readerLink QR for %s", detail_.bookId);
  startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, detail_.readerLink),
                         [](const ActivityResult&) {});
}

void WeReadBookActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const char* title = detail_.title[0] != '\0' ? detail_.title : book_.title;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 title[0] != '\0' ? title : tr(STR_WEREAD));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int x = metrics.contentSidePadding;
  const int lineWidth = pageWidth - x * 2;

  if (state_ == State::Loading) {
    GUI.drawPopup(renderer, tr(STR_WEREAD_LOADING));
  } else if (state_ == State::Failed) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2,
                              failText_ != nullptr ? failText_ : tr(STR_WEREAD_FAILED));
  } else {
    int y = contentTop + 8;
    const char* author = detail_.author[0] != '\0' ? detail_.author : book_.author;
    if (author[0] != '\0') {
      renderer.drawCenteredText(UI_12_FONT_ID, y, author);
      y += 32;
    }

    if (detail_.finished) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_WEREAD_FINISHED));
      y += 32;
    } else {
      renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_WEREAD_PROGRESS), true);
      y += 22;
      GUI.drawProgressBar(renderer, Rect{x, y, lineWidth, 14}, detail_.progress, 100);
      y += 48;
    }

    if (detail_.readingTime > 0) {
      char duration[48] = {};
      WeReadUi::formatDuration(detail_.readingTime, duration, sizeof(duration));
      char line[96] = {};
      snprintf(line, sizeof(line), "%s: %s", tr(STR_WEREAD_READING_TIME), duration);
      renderer.drawText(UI_10_FONT_ID, x, y, line, true);
      y += 26;
    }

    if (detail_.readerLink[0] != '\0') {
      renderer.drawCenteredText(SMALL_FONT_ID, contentTop + contentHeight - 44, tr(STR_WEREAD_SCAN_HINT));
    } else {
      renderer.drawCenteredText(SMALL_FONT_ID, contentTop + contentHeight - 44, tr(STR_WEREAD_NO_LINK));
    }
    renderer.drawCenteredText(SMALL_FONT_ID, contentTop + contentHeight - 24, tr(STR_WEREAD_READONLY));
  }

  if (state_ == State::Failed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == State::Ready) {
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), detail_.readerLink[0] != '\0' ? tr(STR_WEREAD_SCAN) : "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
