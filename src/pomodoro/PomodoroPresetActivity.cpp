#include "PomodoroPresetActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <memory>

#include "MappedInputManager.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PomodoroPresetActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PomodoroPresetActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex < PomodoroSession::PRESET_COUNT) {
      choosePreset(selectedIndex);
    } else {
      openCustomPicker();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void PomodoroPresetActivity::choosePreset(const int index) {
  const auto& preset = PomodoroSession::PRESETS[index];
  POMODORO.start(preset.workMinutes, preset.breakMinutes);
  finish();
}

void PomodoroPresetActivity::openCustomPicker() {
  startActivityForResult(std::make_unique<IntervalSelectionActivity>(
                             renderer, mappedInput, "PomodoroCustomWork", StrId::STR_FOCUS_LENGTH,
                             StrId::STR_SLEEP_TIMER_STEP_HINT, 25, PomodoroSession::MIN_WORK_MINUTES,
                             PomodoroSession::MAX_WORK_MINUTES, 1, 5, StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, true),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const int work = static_cast<int>(std::get<IntervalResult>(result.data).value);
                             const int breakMinutes = work / 5 < 1 ? 1 : work / 5;
                             POMODORO.start(static_cast<uint8_t>(work), static_cast<uint8_t>(breakMinutes));
                             finish();
                           } else {
                             requestUpdate();
                           }
                         });
}

void PomodoroPresetActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_FOCUS_TIMER));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, ROW_COUNT, selectedIndex,
      [](int index) -> std::string {
        if (index < PomodoroSession::PRESET_COUNT) {
          const auto& preset = PomodoroSession::PRESETS[index];
          char label[40];
          snprintf(label, sizeof(label), tr(STR_FOCUS_PRESET_FORMAT), static_cast<unsigned>(preset.workMinutes),
                   static_cast<unsigned>(preset.breakMinutes));
          return label;
        }
        return tr(STR_FOCUS_CUSTOM);
      },
      nullptr, nullptr, nullptr, false);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
