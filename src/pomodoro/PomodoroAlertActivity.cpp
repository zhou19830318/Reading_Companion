#include "PomodoroAlertActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "pomodoro/PomodoroSync.h"

void PomodoroAlertActivity::onEnter() {
  Activity::onEnter();
  drawAlert();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  blinkAlert();
  // Opportunistic relay upload: only when Wi-Fi is already up (this firmware
  // never connects silently) and the relay is configured. Runs after the
  // alert is visible, so the blocking round trip cannot delay the warning.
  PomodoroSync::flushIfOnline();
}

void PomodoroAlertActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (swallowConfirmRelease_) {
      swallowConfirmRelease_ = false;
      return;
    }
    POMODORO.acknowledge();
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (swallowBackRelease_) {
      swallowBackRelease_ = false;
      return;
    }
    POMODORO.endSession();
    finish();
  }
}

void PomodoroAlertActivity::drawAlert() const {
  const bool workDone = event_ == PomodoroSession::Event::WorkDone;

  renderer.clearScreen();
  const int screenHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  renderer.drawCenteredText(UI_12_FONT_ID, screenHeight / 2 - lineHeight,
                            workDone ? tr(STR_FOCUS_WORK_DONE) : tr(STR_FOCUS_BREAK_DONE), true, EpdFontFamily::BOLD);

  char nextLine[48];
  if (workDone) {
    snprintf(nextLine, sizeof(nextLine), tr(STR_FOCUS_NEXT_BREAK), static_cast<unsigned>(POMODORO.nextBreakMinutes()));
  } else {
    snprintf(nextLine, sizeof(nextLine), tr(STR_FOCUS_NEXT_WORK), static_cast<unsigned>(POMODORO.workMinutes()));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2 + 6, nextLine);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
  // OnePage lays the four hints out as Back, Left, Right, Confirm — the
  // confirm label lives in btn4, so all four slots must be forwarded.
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

bool PomodoroAlertActivity::blinkAlert() {
  constexpr unsigned long BLINK_MS = 300;
  for (int round = 0; round < 2; ++round) {
    delay(BLINK_MS);
    mappedInput.update();
    if (mappedInput.wasAnyPressed()) {
      // The panel is still on the normal frame; only a held button needs its
      // release swallowed so the interrupt does not also acknowledge.
      swallowConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
      swallowBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
      return true;
    }
    renderer.invertScreen();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    delay(BLINK_MS);
    mappedInput.update();
    if (mappedInput.wasAnyPressed()) {
      swallowConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
      swallowBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
      // Restore the readable frame before handing control back.
      renderer.invertScreen();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return true;
    }
    renderer.invertScreen();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
  return false;
}
