#include "KeyGesture.h"

#include <Arduino.h>

KeyGesture::KeyGesture(const MappedInputManager::Button button) : KeyGesture(button, Config{}) {}

KeyGesture::Event KeyGesture::update(const MappedInputManager& input) {
  const bool pressEdge = input.wasPressed(button);
  const bool releaseEdge = input.wasReleased(button);

  if (pressEdge) {
    // Fresh press: re-arm the swallow latch and anchor the repeat schedule.
    longFired = false;
    lastRepeatMs = millis();
    return Event::Press;
  }

  if (releaseEdge) {
    const Event event = longFired ? Event::Release : Event::ShortRelease;
    longFired = false;
    return event;
  }

  if (!input.isPressed(button)) {
    return Event::None;
  }

  const unsigned long held = input.getHeldTime();

  // Long-start wins over a coinciding repeat tick on the same frame so the
  // consumer always observes the transition before subsequent repeats.
  if (config.enableLong && !longFired && held >= config.longPressMs) {
    longFired = true;
    return Event::LongStart;
  }

  if (config.enableRepeat && held >= config.repeatStartMs && (millis() - lastRepeatMs) >= config.repeatIntervalMs) {
    lastRepeatMs = millis();
    return Event::Repeat;
  }

  return Event::None;
}
