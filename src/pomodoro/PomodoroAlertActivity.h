#pragma once

#include <I18n.h>

#include "activities/Activity.h"
#include "pomodoro/PomodoroSession.h"

// Full-screen focus-timer alert pushed by the main loop when a work or break
// countdown reaches zero. The panel blinks twice (the OnePage has no usable
// LED) and the first press during the blink only stops it — it never also
// acknowledges the phase. Confirm starts the next phase, Back ends the
// session.
class PomodoroAlertActivity final : public Activity {
 public:
  explicit PomodoroAlertActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const PomodoroSession::Event event)
      : Activity("PomodoroAlert", renderer, mappedInput), event_(event) {}

  void onEnter() override;
  void loop() override;
  // The alert is the whole point of the timer: it must stay visible while the
  // user is away from the device, so auto-sleep never preempts it.
  bool preventAutoSleep() override { return true; }

 private:
  PomodoroSession::Event event_;
  // Set when a press interrupted the blink; the matching release is swallowed
  // so the interrupt gesture does not acknowledge/end the session.
  bool swallowConfirmRelease_ = false;
  bool swallowBackRelease_ = false;

  void drawAlert() const;
  // Returns true when a press interrupted the blink.
  bool blinkAlert();
};
