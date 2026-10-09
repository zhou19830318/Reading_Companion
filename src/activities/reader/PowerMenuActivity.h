#pragma once

#include "activities/Activity.h"

// Short-Power quick-action chooser (SETTINGS.shortPwrBtn == VOICE_NOTE): two
// pictorial cards — the AI study-buddy question screen or the voice bookmark
// (see drawChoiceCards) — returned as PowerMenuResult{index}; Back cancels.
// The highlight starts on the card picked last time: a file-scope counter, not
// a stored setting, because a settings write per press would spend SPIFFS
// erase cycles on one lost keypress (Resource Protocol 8) and the choice is
// only a navigation detail.
class PowerMenuActivity final : public Activity {
 public:
  static constexpr int PICK_ASK_AI = 0;
  static constexpr int PICK_VOICE_NOTE = 1;
  static constexpr int PICK_COUNT = 2;

  PowerMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PowerMenu", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  int selectedIndex = 0;
};
