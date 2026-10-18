#include "NextChapterIndexer.h"

#include <Epub.h>
#include <Epub/Section.h>
#include <GfxRenderer.h>
#include <Logging.h>

NextChapterIndexer::~NextChapterIndexer() { cancelAndJoin(); }

void NextChapterIndexer::start(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer,
                               const NextChapterIndexParams& params) {
  if (handle_ != nullptr) {
    LOG_ERR("NCI", "start() while already building spine %d", spineIndex_);
    return;
  }

  epub_ = epub;
  renderer_ = &renderer;
  params_ = params;
  spineIndex_ = spineIndex;
  cancel_.store(false, std::memory_order_relaxed);
  done_.store(false, std::memory_order_relaxed);

  // 8192 B: the same call stack (expat buffers, ChapterHtmlSlimParser with its
  // in-object word buffer, streaming locals) runs on the reader's 8192 B render
  // task today; the worker mirrors that rather than serialising it through the
  // main loop.
  if (xTaskCreate(&NextChapterIndexer::taskTrampoline, "bgIndex", 8192, this, 1, &handle_) != pdPASS) {
    LOG_ERR("NCI", "Failed to create background index task");
    handle_ = nullptr;
    spineIndex_ = -1;
    epub_.reset();
    renderer_ = nullptr;
  }
}

bool NextChapterIndexer::running() {
  reapIfDone();
  return handle_ != nullptr;
}

void NextChapterIndexer::join() {
  while (handle_ != nullptr && !done_.load(std::memory_order_acquire)) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  reapIfDone();
}

void NextChapterIndexer::cancelAndJoin() {
  if (handle_ != nullptr) {
    cancel_.store(true, std::memory_order_relaxed);
  }
  join();
}

void NextChapterIndexer::joinIfSpineOrCancel(const int spine) {
  if (!running()) {
    return;
  }
  if (spineIndex_ == spine) {
    join();
  } else {
    cancelAndJoin();
  }
}

void NextChapterIndexer::reapIfDone() {
  if (handle_ != nullptr && done_.load(std::memory_order_acquire)) {
    // The task deleted itself just before setting done_; nothing to free here.
    failedSpine_ = lastOk_ ? -1 : spineIndex_;
    handle_ = nullptr;
    spineIndex_ = -1;
    epub_.reset();
    renderer_ = nullptr;
  }
}

void NextChapterIndexer::taskTrampoline(void* arg) { static_cast<NextChapterIndexer*>(arg)->run(); }

void NextChapterIndexer::run() {
  LOG_DBG("NCI", "Background indexing spine %d started", spineIndex_);

  Section section(epub_, spineIndex_, *renderer_);
  const bool ok = section.createSectionFile(params_.fontId, params_.lineCompression, params_.extraParagraphSpacing,
                                            params_.paragraphAlignment, params_.viewportWidth, params_.viewportHeight,
                                            params_.hyphenationEnabled, params_.embeddedStyle, params_.imageRendering,
                                            params_.focusReadingEnabled, nullptr, &cancel_);
  LOG_DBG("NCI", "Background indexing spine %d %s", spineIndex_,
          ok ? "done" : (cancel_.load(std::memory_order_relaxed) ? "cancelled" : "failed"));

  // A cancelled build is not a failure: it just means the cache stays absent.
  lastOk_ = ok || cancel_.load(std::memory_order_relaxed);
  done_.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}
