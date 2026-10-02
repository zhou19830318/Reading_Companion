#include "CitySelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WeatherCities.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace {
constexpr int AUTO_ITEM = 0;
}  // namespace

void CitySelectActivity::onEnter() {
  Activity::onEnter();

  totalItems = static_cast<int>(WeatherCities::count()) + 1;  // + the auto row
  selectedIndex = AUTO_ITEM;
  if (SETTINGS.weatherCityKey[0] != '\0') {
    for (int i = 1; i < totalItems; ++i) {
      const char* key = WeatherCities::at(static_cast<size_t>(i - 1)).zh;
      if (std::strcmp(key, SETTINGS.weatherCityKey) == 0) {
        selectedIndex = i;
        break;
      }
    }
    // A key the table no longer holds (stale settings) falls back to the auto
    // row rather than to an arbitrary city.
  }

  requestUpdate();
}

void CitySelectActivity::onExit() { Activity::onExit(); }

void CitySelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // 245 rows: the release step moves one row, holding the key jumps a page
  // (same split LanguageSelectActivity uses).
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void CitySelectActivity::handleSelection() {
  if (selectedIndex == AUTO_ITEM) {
    SETTINGS.weatherCityKey[0] = '\0';
  } else {
    const WeatherCities::City& city = WeatherCities::at(static_cast<size_t>(selectedIndex - 1));
    std::snprintf(SETTINGS.weatherCityKey, sizeof(SETTINGS.weatherCityKey), "%s", city.zh);
  }
  // No saveToFile() here: SettingsActivity's ACTION result handler persists the
  // settings on return from every picker it launched (SettingsActivity.cpp,
  // `auto resultHandler = ... SETTINGS.saveToFile()`).
  onBack();
}

void CitySelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WEATHER_CITY));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int selected = selectedIndex;
  // Chinese UIs show 中文名 (which is also the settings key); everyone else gets
  // the pinyin label, so the list reads the same as the rest of the UI.
  const bool chinese = I18n::isChinese(I18N.getLanguage());

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalItems, selectedIndex,
      [chinese](int index) -> std::string {
        if (index == AUTO_ITEM) return std::string(tr(STR_WEATHER_CITY_AUTO));
        const WeatherCities::City& city = WeatherCities::at(static_cast<size_t>(index - 1));
        return std::string(WeatherCities::label(city, chinese));
      },
      nullptr, nullptr,
      [selected](int index) -> std::string { return index == selected ? std::string(tr(STR_SELECTED)) : ""; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
