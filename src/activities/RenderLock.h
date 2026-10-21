#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
// Reentrant within one task: a nested RenderLock on the task that already
// holds the mutex joins the existing ownership and does not give it back
// (FreeRTOS mutexes are non-recursive; a plain second take deadlocks).
class RenderLock {
  bool isLocked = false;
  bool isOwned = true;

 public:
  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  static bool peek();
};
