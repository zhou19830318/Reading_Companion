#pragma once

#include <I18n.h>

#include "activities/Activity.h"
#include "pomodoro/PomodoroSession.h"
#include "util/ButtonNavigator.h"

// Focus-timer preset picker opened from the reader menu: the three built-in
// work/break presets plus a "Custom..." row that opens the shared interval
// picker for the work length (break = work / 5, rounded down, at least 1).
class PomodoroPresetActivity final : public Activity {
 public:
  explicit PomodoroPresetActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PomodoroPreset", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int ROW_COUNT = PomodoroSession::PRESET_COUNT + 1;  // presets + custom

  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  void choosePreset(int index);
  void openCustomPicker();
};
