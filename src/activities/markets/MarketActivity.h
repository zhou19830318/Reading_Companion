#pragma once

#include <MarketClient.h>

#include "activities/Activity.h"

class MarketActivity final : public Activity {
 public:
  MarketActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Markets", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return loading; }

 private:
  Markets::Client client;
  Markets::Snapshot data;
  uint8_t group = 0;
  uint8_t selected = 0;
  uint8_t period = 0;
  char code[Markets::CODE_CAP] = {};
  bool detail = false;
  bool lookup = false;
  bool loading = false;
  bool wifiPending = false;
  Markets::Client::Result error = Markets::Client::Result::Ok;

  void queueLoad();
  void load();
  void search();
};
