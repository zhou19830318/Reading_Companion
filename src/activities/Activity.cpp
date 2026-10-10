#include "Activity.h"

#include "ActivityManager.h"

void Activity::onEnter() {
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
  // The frame that follows a switch repaints the whole screen, so keep it on
  // the FAST differential: it must not be the push that gets promoted to the
  // absolute ghosting-cleanup waveform, which flashes the panel black for
  // >1 s exactly while the user is watching the transition.
  renderer.holdOffCadenceForNextPush();
}

void Activity::onExit() {
  LOG_DBG("ACT", "Exiting activity: %s", name.c_str());
  // A pop restores the parent without calling its onEnter(), so the hold-off
  // has to be armed here as well — this is the switch path that leads straight
  // into the parent's transition frame.
  renderer.holdOffCadenceForNextPush();
}

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
