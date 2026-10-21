#pragma once

#include <GfxRenderer.h>

#include "components/UITheme.h"

// Single owner of the "opening a book" progress popup, driven from the moment
// the user picks a title until the first section is laid out.
//
// The flow has two blocking phases that both need to show one continuous bar:
//   1. Metadata load (Epub::load) runs on the *main* task inside
//      ReaderActivity::onEnter. It has no render pass of its own, so this
//      helper draws and pushes the popup directly, taking the RenderLock around
//      each draw so it never races the render task.
//   2. Section build (Section::createSectionFile) runs on the *render* task
//      inside EpubReaderActivity::render, which already holds the RenderLock.
//      It drives the same popup through a reportFn so the bar keeps climbing.
//
// The two phases are remapped onto one 0-100 axis so the bar never jumps
// backwards: metadata owns LOAD_SHARE of the range, layout owns the rest. A
// percent is only repainted when it advances (every redraw is a ~0.2-1s e-ink
// push), and the popup is torn down in finish() once the first frame is drawn.
//
// Not a heap object: it is a file-static singleton (see OpenBookProgress.cpp).
// The ESP32-C3 has one core, so no atomics are needed — the two phases are
// strictly sequential (main task, then render task), never concurrent.
class OpenBookProgress {
 public:
  // Fraction of the 0-100 axis given to metadata load; layout takes the rest.
  static constexpr int LOAD_SHARE = 40;

  // (Re)draw the popup shell at `percent` and arm the axis. Safe to call from
  // either task; takes the RenderLock internally.
  static void begin(GfxRenderer& renderer, int percent);

  // Advance the bar to `percent` (0-100 on the shared axis). Throttled: only
  // repaints when the integer percent changed. Safe from either task.
  static void report(GfxRenderer& renderer, int percent);

  // Report a phase-local 0-100 into its slice of the shared axis.
  // isLayout=false maps into [0, LOAD_SHARE], true into [LOAD_SHARE, 100].
  static void reportPhase(GfxRenderer& renderer, int phasePercent, bool isLayout);

  // Erase the popup state and repaint the whole screen white so the shell and
  // bar do not linger under the first rendered frame. Call once, from the
  // render task, right before rendering the first page.
  static void finish(GfxRenderer& renderer);

  // True while a popup is on screen (used to force a repaint on the phase
  // hand-off and to skip no-op reports).
  static bool active();

 private:
  static void drawShell(GfxRenderer& renderer, int percent, const char* message);  // caller holds RenderLock
  static void pushProgress(GfxRenderer& renderer, int percent);                    // caller holds RenderLock

  static bool active_;
  static int lastPercent_;
  static Rect shell_;
};
