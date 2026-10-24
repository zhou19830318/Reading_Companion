#pragma once

#include <stdint.h>

#include "MappedInputManager.h"

// Poll-based gesture classifier for a single logical button.
//
// Design rationale (compared against existing patterns in this repo):
// - Release-edge commit (cf. EpubReaderMenuActivity::loop) is preserved as the
//   ShortRelease event: a short press only commits when the finger lifts, and
//   never after a long-press has fired (评分/动作必须在释放后执行, §9.3).
// - Hold-to-act + release-swallow (cf. RecentBooksActivity::longPressFired) is
//   internalized here instead of being re-handwritten in every activity.
// - Continuous repeat (cf. ButtonNavigator) is folded in as Repeat ticks so
//   learning screens need one helper instead of two.
// - Unlike ButtonNavigator there is no static/global input pointer: the
//   MappedInputManager is passed to update() and state lives in the instance,
//   which keeps the class testable and activity-lifetime scoped.
//
// Poll model, not callbacks (unlike the ESP-IDF `button` component): activities
// drive everything from loop(), so update() returns at most one event per frame
// and the caller consumes it immediately.
class KeyGesture final {
 public:
  enum class Event : uint8_t {
    None,          // nothing happened this frame
    Press,         // physical press edge
    LongStart,     // hold crossed longPressMs (fires once, then swallows the release)
    Repeat,        // repeat tick while held (after repeatStartMs, every repeatIntervalMs)
    ShortRelease,  // release without a prior LongStart — the commit edge
    Release,       // release after a LongStart (the short press was swallowed)
  };

  struct Config {
    uint16_t longPressMs = 500;
    uint16_t repeatStartMs = 600;
    uint16_t repeatIntervalMs = 250;
    bool enableLong = true;
    bool enableRepeat = false;
  };

  // Default-args with Config{} would trip the nested-class NSDMI rule if
  // written inline here, so the convenience overload lives in the .cpp.
  explicit KeyGesture(MappedInputManager::Button button);
  KeyGesture(MappedInputManager::Button button, Config config) : button(button), config(config) {}

  // Call once per activity loop(). Returns the single most significant event
  // for this frame (value semantics, no queue — the caller consumes it now).
  Event update(const MappedInputManager& input);

  [[nodiscard]] MappedInputManager::Button getButton() const { return button; }
  [[nodiscard]] bool longPressFired() const { return longFired; }

 private:
  MappedInputManager::Button button;
  Config config;
  bool longFired = false;
  uint32_t lastRepeatMs = 0;
};
