#pragma once

#include <WeReadClient.h>

#include "activities/Activity.h"

// Overall WeRead reading statistics from /readdata/detail?mode=overall.
class WeReadStatsActivity final : public Activity {
 public:
  explicit WeReadStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WeReadStats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { Loading, Ready, Failed };

  void load();

  WeRead::Stats stats_;
  State state_ = State::Loading;
  bool started_ = false;
  const char* failText_ = nullptr;
};
