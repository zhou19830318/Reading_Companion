#include "OpenBookProgress.h"

#include <I18n.h>
#include <Logging.h>

#include "activities/RenderLock.h"

bool OpenBookProgress::active_ = false;
int OpenBookProgress::lastPercent_ = -1;
Rect OpenBookProgress::shell_{0, 0, 0, 0};

void OpenBookProgress::begin(GfxRenderer& renderer, const int percent) {
  RenderLock lock;
  active_ = true;
  lastPercent_ = -1;
  drawShell(renderer, percent, tr(STR_LOADING_POPUP));
  pushProgress(renderer, percent);
}

void OpenBookProgress::report(GfxRenderer& renderer, const int percent) {
  if (!active_ || percent <= lastPercent_) {
    return;
  }
  RenderLock lock;
  pushProgress(renderer, percent);
}

void OpenBookProgress::reportPhase(GfxRenderer& renderer, const int phasePercent, bool isLayout) {
  const int clamped = phasePercent < 0 ? 0 : (phasePercent > 100 ? 100 : phasePercent);
  const int mapped = isLayout ? LOAD_SHARE + (100 - LOAD_SHARE) * clamped / 100 : LOAD_SHARE * clamped / 100;
  report(renderer, mapped);
}

void OpenBookProgress::finish(GfxRenderer& renderer) {
  if (!active_) {
    return;
  }
  active_ = false;
  lastPercent_ = -1;
  RenderLock lock;
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

bool OpenBookProgress::active() { return active_; }

void OpenBookProgress::drawShell(GfxRenderer& renderer, const int percent, const char* message) {
  (void)percent;
  // The shell is a centered loading box; the bar is painted on top by
  // pushProgress. Redrawn on the phase hand-off so a resized message does not
  // leave a stale box on the panel.
  shell_ = GUI.drawPopup(renderer, message, /*withProgressBar=*/true, /*display=*/false);
}

void OpenBookProgress::pushProgress(GfxRenderer& renderer, const int percent) {
  lastPercent_ = percent;
  GUI.fillPopupProgress(renderer, shell_, percent, /*showPercentLabel=*/true, /*display=*/true);
}
