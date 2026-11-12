#pragma once

#include <WeReadService.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// WeRead shelf. The relay owns the official API key and returns a compact,
// read-only snapshot. Entering the screen is the only refresh point; there is
// no polling and no progress write-back.
class WeReadActivity final : public Activity {
 public:
  explicit WeReadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WeRead", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { Loading, Ready, Failed };

  void load();
  void openSelected();
  void openStats();

  State state_ = State::Loading;
  bool started_ = false;
  int selectedIndex_ = 0;
  const char* failText_ = nullptr;
  ButtonNavigator buttonNavigator_;
};
