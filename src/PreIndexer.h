#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <deque>
#include <string>
#include <vector>

class GfxRenderer;

// Background EPUB pre-indexer: drains a queue of books so the caches a reader
// needs already exist by the time the user opens one. Per book, one job runs:
//   A) Epub::load(buildIfMissing=true) — book.bin + CSS cache (cold cost:
//      ~7 s OPF/TOC/CSS pass, the black-box 0-40% of the open popup)
//   B) Section::createSectionFile()    — layout of the chapter the book will
//      open at (progress.bin resume spine, else 0; the 40-100% popup phase)
//
// Threading and ownership rules:
//  - One worker ("preIdx", 8192 B stack — same shape as NextChapterIndexer's
//    proven bgIndex task), FreeRTOS priority 1.
//  - Runs ONLY while tick(..., allowed=true): idle browsing screens (Home /
//    FileBrowser) with no reader anywhere on the activity stack. The activity
//    transition calls suspend() before a reader's onEnter() so the worker is
//    joined before the foreground builds its own caches — the two never run
//    concurrently (join is bounded: cancel is polled per ~1 KB parse chunk and
//    per build-loop entry, so the worker exits within milliseconds).
//  - The worker never touches the framebuffer or progress popups (Epub
//    progress hook unset, Section popupFn = nullptr): its work is invisible.
//  - book.bin and css_rules.cache are written tmp+rename by the library, so a
//    cancel or power cut never leaves a half-written file at a readable path —
//    final-path presence IS the index-complete flag.
//  - All SD access goes through HalStorage (mutex-safe from this task).
//  - queue_/dirs_/failed_ are guarded by mutex_ (worker vs. upload callbacks);
//    handle_/cancel_/done_ are main-task-only plus atomics.
//
// Scan coverage: one SD walk at boot (armed by scanRequested_) plus targeted
// enqueue() from clearBookCache() on upload/delete. A card swapped in at
// runtime is picked up on the next boot.
class PreIndexer {
 public:
  static PreIndexer& getInstance();

  // Main-loop hook (ActivityManager::loop). allowed=false cancels+joins the
  // worker (a no-op once it is gone); allowed=true spawns it when work is due.
  void tick(GfxRenderer& renderer, bool allowed);

  // Cancel + join the worker. Bounded: waits up to ~5 s for the worker to hit
  // a cancel checkpoint; if it never does (wedged inside a blocking SD call),
  // gives up and returns so the UI loop never freezes. Called from the activity
  // transition right before a reader's onEnter() — tick() would only run after
  // onEnter returns, which is too late to stop the overlap.
  void suspend();

  // Queue one EPUB for background indexing (deduped; ignored once the book
  // has failed this boot). Called from clearBookCache() after upload/delete.
  void enqueue(const std::string& epubPath);

  // Debug/serial-console controls (CMD:PRE_OFF / PRE_ON / PRE_DUMP).
  // pause(): cancel any running worker and keep spawning disabled until
  // resume() — used to isolate foreground vs background interference.
  void pause();
  void resume();
  void dumpState() const;

  PreIndexer(const PreIndexer&) = delete;
  PreIndexer& operator=(const PreIndexer&) = delete;

 private:
  PreIndexer();
  ~PreIndexer() = default;

  struct PendingDir {
    std::string path;
    uint8_t depth;
  };

  enum class JobResult { Done, Cancelled, Failed };

  static void taskTrampoline(void* arg);
  void run();
  void spawn(GfxRenderer& renderer);
  void reapIfDone();
  bool needWork();
  void scanOneDir(const std::string& dirPath, uint8_t depth);
  JobResult processJob(const std::string& path);

  // Callers must hold mutex_.
  void enqueueLocked(const std::string& epubPath);

  // Main-task-only (spawn/reap from tick/suspend).
  TaskHandle_t handle_ = nullptr;
  GfxRenderer* renderer_ = nullptr;
  // A suspend() timed out waiting for the worker (it is wedged, e.g. inside a
  // blocking SD transaction). Spawning stays paused until the zombie finally
  // sets done_ and reapIfDone() clears the flag — never two live workers.
  bool wedged_ = false;
  // Explicit pause from the debug console; independent of wedged_.
  bool paused_ = false;

  // Shared with the worker via atomics.
  std::atomic<bool> cancel_{false};
  std::atomic<bool> done_{false};
  std::atomic<bool> scanRequested_{true};  // full SD walk on first tick

  // Guarded by mutex_: queue of book paths, DFS scan stack, per-boot failures.
  SemaphoreHandle_t mutex_ = nullptr;
  std::vector<std::string> queue_;
  std::deque<PendingDir> dirs_;
  std::vector<std::string> failed_;
};
