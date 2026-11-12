#include "WeReadActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <WifiIdleManager.h>

#include <string>

#include "WeReadBookActivity.h"
#include "WeReadStatsActivity.h"
#include "WeReadUi.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WeReadActivity::onEnter() {
  Activity::onEnter();
  WIFI_IDLE.arm();
  WIFI_IDLE.hold();
  state_ = State::Loading;
  started_ = false;
  selectedIndex_ = 0;
  failText_ = nullptr;

  if (!WEREAD_SERVICE.begin()) {
    state_ = State::Failed;
    failText_ = tr(STR_WEREAD_CONFIG);
    requestUpdate();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("WEREAD", "no network, opening Wi-Fi selection");
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

void WeReadActivity::onExit() {
  WIFI_IDLE.release();
  Activity::onExit();
}

void WeReadActivity::load() {
  WEREAD_SERVICE.syncConfig();
  const WeReadClient::Result result = WEREAD_SERVICE.refresh();
  if (result == WeReadClient::Result::Ok) {
    state_ = State::Ready;
    if (selectedIndex_ >= static_cast<int>(WEREAD_SERVICE.bookCount())) selectedIndex_ = 0;
  } else {
    state_ = State::Failed;
    failText_ = WeReadUi::errorText(result);
    LOG_ERR("WEREAD", "shelf load failed: %s", WeReadClient::resultName(result));
  }
  requestUpdate();
}

void WeReadActivity::loop() {
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

  const int count = static_cast<int>(WEREAD_SERVICE.bookCount());
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
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    openStats();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void WeReadActivity::openSelected() {
  if (selectedIndex_ < 0 || static_cast<size_t>(selectedIndex_) >= WEREAD_SERVICE.bookCount()) return;
  const WeRead::Book& book = WEREAD_SERVICE.books()[selectedIndex_];
  LOG_INF("WEREAD", "open book %s", book.bookId);
  activityManager.pushActivity(std::make_unique<WeReadBookActivity>(renderer, mappedInput, book));
}

void WeReadActivity::openStats() {
  activityManager.pushActivity(std::make_unique<WeReadStatsActivity>(renderer, mappedInput));
}

void WeReadActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WEREAD));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const Rect content{0, contentTop, pageWidth, contentHeight};

  if (state_ == State::Loading) {
    GUI.drawPopup(renderer, tr(STR_WEREAD_LOADING));
  } else if (state_ == State::Failed) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2,
                              failText_ != nullptr ? failText_ : tr(STR_WEREAD_FAILED));
  } else if (WEREAD_SERVICE.bookCount() == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_WEREAD_EMPTY));
  } else {
    GUI.drawList(
        renderer, content, static_cast<int>(WEREAD_SERVICE.bookCount()), selectedIndex_,
        [](int index) {
          const WeRead::Book& book = WEREAD_SERVICE.books()[index];
          return std::string(book.title[0] != '\0' ? book.title : book.bookId);
        },
        [](int index) {
          const WeRead::Book& book = WEREAD_SERVICE.books()[index];
          std::string subtitle = book.author;
          if (book.finished) {
            if (!subtitle.empty()) subtitle += " · ";
            subtitle += tr(STR_WEREAD_FINISHED);
          }
          return subtitle;
        });
  }

  if (state_ == State::Failed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == State::Ready && WEREAD_SERVICE.bookCount() > 0) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_WEREAD_STATS), "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
