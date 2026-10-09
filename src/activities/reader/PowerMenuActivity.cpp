#include "PowerMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"

namespace {
// Last picked row for this power-up (see the header note).
int lastPick = PowerMenuActivity::PICK_ASK_AI;
}  // namespace

void PowerMenuActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = lastPick;
  requestUpdate();
}

void PowerMenuActivity::onExit() { Activity::onExit(); }

void PowerMenuActivity::loop() {
  // Side Up/Down are fixed; front Left/Right mirror them so the bottom row
  // navigates the list too. The pair flips with the nav axis (INVERTED /
  // LANDSCAPE_CCW) to stay on the keys mapLabels() paints 上/下 over.
  const bool navSwapped = mappedInput.isNavDirectionSwapped();
  const auto prevKey = navSwapped ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextKey = navSwapped ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;

  // Cancel on the release edge: the reader below also reads a Back release as
  // "go home", so finishing on the press edge would hand it the orphaned
  // release and close the book behind this chooser (EpubReaderMenu's rule).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) || mappedInput.wasPressed(prevKey)) {
    if (selectedIndex > 0) {
      selectedIndex--;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) || mappedInput.wasPressed(nextKey)) {
    if (selectedIndex + 1 < PICK_COUNT) {
      selectedIndex++;
      requestUpdate();
    }
    return;
  }

  // Confirm and the power key itself both commit: the reader opened this
  // chooser with power, and the footnote chooser binds Power the same way
  // (EpubReaderFootnotesActivity::loop).
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.wasPressed(MappedInputManager::Button::Power)) {
    lastPick = selectedIndex;
    setResult(PowerMenuResult{selectedIndex});
    finish();
  }
}

void PowerMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POWER_MENU));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  static constexpr StrId items[PICK_COUNT] = {StrId::STR_ASK_AI, StrId::STR_VOICE_BOOKMARK};
  GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, PICK_COUNT, selectedIndex,
               [](int index) { return std::string(I18N.get(items[index])); });

  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
