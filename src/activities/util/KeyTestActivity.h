#pragma once

#include <I18n.h>

#include <array>

#include "activities/Activity.h"
#include "input/KeyGesture.h"

// Hardware key diagnostic (百词斩 §14.1 / §16 acceptance: all seven physical
// keys must be recognised, with their gesture types observable).
//
// One row per logical button; each row is fed by its own KeyGesture so short /
// long / repeat behaviour is visible directly on the e-ink panel. Rows are
// logical (not raw GPIO) on purpose: MappedInputManager is the only sanctioned
// input surface, and the row that lights up IS the mapping under test — the
// physical→logical assignment itself is owned by ButtonRemapActivity.
//
// Power is held awake (suppressPowerSleep) so its press/hold can be observed
// without the device sleeping mid-test; Back short-press exits.
class KeyTestActivity final : public Activity {
 public:
  explicit KeyTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KeyTest", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return true; }
  bool suppressPowerSleep() const override { return true; }

  static constexpr size_t ROW_COUNT = 7;

 private:
  struct Row {
    KeyGesture gesture{MappedInputManager::Button::Back, KeyGesture::Config{}};
    StrId event{};
    uint16_t heldMs = 0;
    bool hasEvent = false;
    bool heldNow = false;
  };

  static constexpr size_t BACK_ROW = 0;

  std::array<Row, ROW_COUNT> rows;

  [[nodiscard]] static StrId gestureStrId(KeyGesture::Event event);
};
