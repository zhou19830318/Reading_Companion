#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  // Called when another activity is pushed on top of this one (the activity
  // stays alive on the stack). Use it to quiesce background work that must not
  // run while the user is elsewhere — e.g. tasks that touch fonts or SD caches
  // a pushed settings screen may swap out from under them.
  virtual void onPause() {}
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  // True while a repaint would preempt a live microphone capture: an e-ink
  // refresh blocks the activity loop ~0.8 s, and that loop is the only thing
  // draining the 8 ms I2S DMA ring — audio recorded during a refresh never
  // exists on device (measured: server frame counts short by exactly the
  // refresh window). Gates the automatic post-result repaint; activities with
  // explicit requestUpdate() calls still own those.
  virtual bool preventAutoUpdate() const { return false; }
  // True while the power key is held as a capture control (PTT dictation):
  // the main loop skips the power-hold deep sleep for the duration, or a
  // hold-to-talk utterance would put the device to sleep mid-speech.
  virtual bool suppressPowerSleep() const { return false; }
  virtual bool isReaderActivity() const { return false; }
  // True for idle browsing screens where PreIndexer may run its background
  // book indexing (also requires no reader anywhere on the activity stack).
  virtual bool allowPreIndex() const { return false; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  void onSelectBook(const std::string& path);
};
