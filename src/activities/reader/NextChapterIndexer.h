#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>

class Epub;
class GfxRenderer;

// Layout inputs for a one-off section build, mirroring the arguments of
// Section::createSectionFile() minus the popup callback: the worker runs on a
// separate task and must never touch the framebuffer or progress UI.
struct NextChapterIndexParams {
  int fontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = false;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
};

// Runs a single Section::createSectionFile() for the upcoming chapter on a
// background FreeRTOS task so the reader keeps its input latency while a cold
// chapter is laid out behind it (previously a synchronous ~10-20 s freeze when
// turning onto the second-to-last page of a chapter).
//
// Ownership rules: the owner (EpubReaderActivity) must keep the shared Epub
// and the GfxRenderer alive until the task is joined, and must join or cancel
// before anything can unload its fonts (onPause when another activity is
// pushed, onExit) or re-index the same spine (single-flight: the foreground
// indexing path joins first).
class NextChapterIndexer {
 public:
  NextChapterIndexer() = default;
  ~NextChapterIndexer();

  NextChapterIndexer(const NextChapterIndexer&) = delete;
  NextChapterIndexer& operator=(const NextChapterIndexer&) = delete;

  void start(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer,
             const NextChapterIndexParams& params);

  // True while a build is in flight. Reaps a finished task as a side effect,
  // so it is safe (and cheap) to call on every render pass.
  bool running();

  // Spine index currently being built; -1 when idle.
  int spineIndex() const { return spineIndex_; }

  // Spine whose last completed build failed outright (parse error, OOM). Lets
  // the caller give up on a corrupt chapter instead of restarting the build on
  // every render. Cancelled builds do not count as failures.
  int lastFailedSpine() const { return failedSpine_; }

  // Wait for the in-flight build to finish. The caller is expected to have
  // drawn any progress UI beforehand — a cold index can take ~20 s.
  void join();

  // Request cancellation and wait for the task to stop. Bounded by one parse
  // chunk (~65 ms SD read) plus the current page's SD writes.
  void cancelAndJoin();

  // Join when the in-flight build targets `spine` (the work is wanted),
  // cancel it otherwise (superseded by a jump elsewhere).
  void joinIfSpineOrCancel(int spine);

 private:
  static void taskTrampoline(void* arg);
  void run();
  void reapIfDone();

  TaskHandle_t handle_ = nullptr;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> done_{false};
  std::shared_ptr<Epub> epub_;
  GfxRenderer* renderer_ = nullptr;
  NextChapterIndexParams params_{};
  int spineIndex_ = -1;
  // Written by the worker right before done_, read only after done_ (reap).
  bool lastOk_ = true;
  int failedSpine_ = -1;
};
