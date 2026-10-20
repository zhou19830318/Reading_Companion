#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// The AI hub: one chooser between the two AI front-ends (chat and image
// generation). Home's 助手 entry lands here; each row pushes its activity so
// Back returns to the hub. A chooser instead of two home rows keeps the home
// menu's one-entry-per-feature shape (HomeActivity::menuItems).
class AiHubActivity final : public Activity {
 public:
  explicit AiHubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AiHub", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int ROW_COUNT = 2;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
};
