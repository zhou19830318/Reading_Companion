#pragma once

#include <PigeonService.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// 飞鸽传书 device list: heartbeat fills the peer list from the relay's durable
// device registry, so a registered peer stays selectable while offline. The
// conversation is store-and-forward; Confirm opens it in either state.
class PigeonDeviceListActivity final : public Activity {
 public:
  explicit PigeonDeviceListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PigeonDevices", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { Loading, Ready, Failed };

  static constexpr uint32_t REFRESH_INTERVAL_MS = 10000;
  static constexpr uint32_t RETRY_INTERVAL_MS = 15000;

  void load();
  void openSelected();

  State state_ = State::Loading;
  bool started_ = false;
  int selectedIndex_ = 0;
  uint32_t lastRefreshMs_ = 0;
  uint32_t lastRetryMs_ = 0;
  const char* failText_ = nullptr;
  ButtonNavigator buttonNavigator_;
};
