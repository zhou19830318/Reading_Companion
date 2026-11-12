#pragma once

#include <WeReadClient.h>

#include "activities/Activity.h"

// One WeRead book. Read-only: shows the account's official progress and can
// display the readerLink as a QR code so the phone can take over. Confirm on
// the reader never changes the WeRead account state.
class WeReadBookActivity final : public Activity {
 public:
  WeReadBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const WeRead::Book& book)
      : Activity("WeReadBook", renderer, mappedInput), book_(book) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { Loading, Ready, Failed };

  void load();
  void openQr();

  WeRead::Book book_;
  WeRead::BookDetail detail_;
  State state_ = State::Loading;
  bool started_ = false;
  const char* failText_ = nullptr;
};
