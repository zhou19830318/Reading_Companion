#pragma once

#include <GfxRenderer.h>

#include "activities/Activity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

/**
 * Picker for the workbench weather location (CrossPointSettings::weatherCityKey).
 *
 * Row 0 is "auto" — locate the reader by its public IP — and the remaining rows
 * are the built-in cities of lib/WeatherCities in the table's pinyin order.
 * The table exists because no free IP geolocation service puts this reader in
 * the right city; see WeatherCities.h for the measurements.
 */
class CitySelectActivity final : public Activity {
 public:
  explicit CitySelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CitySelect", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();
  void onBack() { finish(); }

  // List row: 0 = auto, i + 1 = WeatherCities::at(i).
  int selectedIndex = 0;
  int totalItems = 1;
  ButtonNavigator buttonNavigator;
};
