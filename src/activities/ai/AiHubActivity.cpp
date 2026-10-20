#include "AiHubActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <memory>

#include "activities/ai/AiImageActivity.h"
#include "activities/voice/VoiceActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void AiHubActivity::onEnter() {
  Activity::onEnter();
  // The base class does not trigger the first paint; without this the screen
  // keeps showing whatever the previous activity left in the framebuffer until
  // the first key press (ActivityManager only re-renders on request).
  requestUpdate();
}

void AiHubActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Push, not replace: Back from either front-end lands back here, so
    // switching between chat and image costs one press.
    if (selectedIndex == 0) {
      LOG_INF("AIHUB", "opening chat");
      activityManager.pushActivity(std::make_unique<VoiceActivity>(renderer, mappedInput));
    } else {
      LOG_INF("AIHUB", "opening image generation");
      activityManager.pushActivity(std::make_unique<AiImageActivity>(renderer, mappedInput));
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();  // hub replaced home, so this lands on the home screen
    return;
  }
}

void AiHubActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_VOICE));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  static constexpr StrId rowLabels[ROW_COUNT] = {StrId::STR_AI_CHAT, StrId::STR_AI_IMAGE};
  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, ROW_COUNT, selectedIndex,
      [](int index) { return I18N.get(rowLabels[index]); }, nullptr,
      [](int index) { return index == 0 ? Mic : Image; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
