#include "PreIndexer.h"

#include <Epub.h>
#include <Epub/Section.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "activities/reader/NextChapterIndexer.h"
#include "activities/reader/ProgressFile.h"
#include "components/UITheme.h"

namespace {

// Short RAII over the queue/dirs mutex. FreeRTOS mutexes only: every caller
// runs in task context (worker, main loop, network upload callbacks).
class QueueLock {
 public:
  explicit QueueLock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
  ~QueueLock() { xSemaphoreGive(mutex_); }
  QueueLock(const QueueLock&) = delete;
  QueueLock& operator=(const QueueLock&) = delete;

 private:
  SemaphoreHandle_t mutex_;
};

constexpr uint8_t MAX_SCAN_DEPTH = 4;
constexpr size_t MAX_FAILED_BOOKS = 32;

// Layout snapshot for the one-off section build — mirrors the params
// EpubReaderActivity gathers for NextChapterIndexer (same sources: current
// settings plus oriented viewable area minus status bar), so a pre-built
// section matches what the reader's first render would produce at this
// orientation. Settings/orientation churn after the build simply invalidates
// the section the same way it invalidates any other cached section.
NextChapterIndexParams collectLayoutParams(GfxRenderer& renderer) {
  NextChapterIndexParams params;
  params.fontId = SETTINGS.getReaderFontId();
  params.lineCompression = SETTINGS.getReaderLineCompression();
  params.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  params.paragraphAlignment = SETTINGS.paragraphAlignment;
  params.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  params.embeddedStyle = SETTINGS.embeddedStyle;
  params.imageRendering = SETTINGS.imageRendering;
  params.focusReadingEnabled = SETTINGS.focusReadingEnabled;

  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int margin = SETTINGS.screenMargin;
  top += margin;
  left += margin;
  right += margin;
  bottom += std::max(margin, static_cast<int>(UITheme::getInstance().getStatusBarHeight()));

  params.viewportWidth = renderer.getScreenWidth() - left - right;
  params.viewportHeight = renderer.getScreenHeight() - top - bottom;
  return params;
}

// Spine this book will open at: persisted resume position, else 0. Raw
// progress value — the caller clamps against spineCount (a value >= count is
// the finished-book sentinel or corrupt state; nothing to pre-warm then).
int resumeSpineIndex(const Epub& epub) {
  const std::string& cachePath = epub.getCachePath();
  uint8_t data[6] = {};
  if (ProgressFile::readNewest(cachePath, data, sizeof(data), sizeof(data))) {
    return data[0] | (data[1] << 8);
  }
  // Legacy pre-dual-slot payload: raw 4/6 bytes at progress.bin.
  HalFile file;
  if (Storage.openFileForRead("PRE", cachePath + "/progress.bin", file)) {
    uint8_t legacy[6] = {};
    const int size = file.read(legacy, sizeof(legacy));
    if (size == 4 || size == 6) {
      return legacy[0] | (legacy[1] << 8);
    }
  }
  return 0;  // never opened: first chapter
}

}  // namespace

PreIndexer& PreIndexer::getInstance() {
  static PreIndexer instance;
  return instance;
}

PreIndexer::PreIndexer() {
  mutex_ = xSemaphoreCreateMutex();
  queue_.reserve(16);
  if (mutex_ == nullptr) {
    LOG_ERR("PRE", "Failed to create pre-index queue mutex");
  }
}

void PreIndexer::tick(GfxRenderer& renderer, const bool allowed) {
  reapIfDone();
  if (!allowed || paused_) {
    suspend();
    return;
  }
  if (wedged_ || handle_ != nullptr) {
    return;  // zombie worker still alive; it drains scan + queue before exiting
  }
  if (!needWork()) {
    return;
  }
  spawn(renderer);
}

void PreIndexer::suspend() {
  if (handle_ == nullptr) {
    return;
  }
  cancel_.store(true, std::memory_order_relaxed);
  if (wedged_) {
    // Already timed out once this boot; the zombie will set done_ whenever it
    // unblocks and reapIfDone() will collect it then. Never stall the UI again.
    return;
  }
  // Bounded by the next cancel checkpoint: ~1 KB parse chunk / one build-loop
  // entry / one directory step (plus the rare uncancelable zip stat calls).
  // The bound itself covers the pathological case the checkpoints cannot: the
  // worker stuck inside a blocking SdFat transaction, where no cancel check
  // can run. An unbounded spin here freezes the whole main loop (no serial
  // commands, no power key, no input) — observed on-device.
  for (int i = 0; i < 1000; ++i) {
    if (done_.load(std::memory_order_acquire)) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (done_.load(std::memory_order_acquire)) {
    handle_ = nullptr;
    return;
  }
  wedged_ = true;
  LOG_ERR("PRE", "Worker did not stop within 5s (wedged in SD/parse); UI unblocked, spawn paused");
}

void PreIndexer::enqueue(const std::string& epubPath) {
  if (!FsHelpers::hasEpubExtension(epubPath)) {
    return;
  }
  if (mutex_ == nullptr) {
    return;
  }
  QueueLock lock(mutex_);
  enqueueLocked(epubPath);
}

void PreIndexer::pause() {
  paused_ = true;
  cancel_.store(true, std::memory_order_relaxed);
  LOG_INF("PRE", "Paused (debug): worker cancel requested, spawn disabled");
}

void PreIndexer::resume() {
  paused_ = false;
  LOG_INF("PRE", "Resumed (debug): spawn enabled");
}

void PreIndexer::dumpState() const {
  size_t q = 0, d = 0, f = 0;
  if (mutex_ != nullptr) {
    QueueLock lock(mutex_);
    q = queue_.size();
    d = dirs_.size();
    f = failed_.size();
  }
  LOG_INF("PRE", "state: handle=%s done=%d cancel=%d wedged=%d paused=%d scanReq=%d queue=%zu dirs=%zu failed=%zu",
          handle_ ? "live" : "null", done_.load(std::memory_order_relaxed), cancel_.load(std::memory_order_relaxed),
          static_cast<int>(wedged_), static_cast<int>(paused_),
          static_cast<int>(scanRequested_.load(std::memory_order_relaxed)), q, d, f);
}

void PreIndexer::enqueueLocked(const std::string& epubPath) {
  if (std::find(queue_.begin(), queue_.end(), epubPath) != queue_.end()) {
    return;
  }
  if (std::find(failed_.begin(), failed_.end(), epubPath) != failed_.end()) {
    return;
  }
  queue_.push_back(epubPath);
}

void PreIndexer::reapIfDone() {
  if (handle_ != nullptr && done_.load(std::memory_order_acquire)) {
    // The worker deleted itself just before setting done_; nothing to free.
    handle_ = nullptr;
    wedged_ = false;
  }
}

bool PreIndexer::needWork() {
  if (scanRequested_.load(std::memory_order_relaxed)) {
    return true;
  }
  if (mutex_ == nullptr) {
    return false;
  }
  QueueLock lock(mutex_);
  return !queue_.empty() || !dirs_.empty();
}

void PreIndexer::spawn(GfxRenderer& renderer) {
  renderer_ = &renderer;
  cancel_.store(false, std::memory_order_relaxed);
  done_.store(false, std::memory_order_relaxed);
  // 8192 B stack: mirrors NextChapterIndexer's bgIndex task, which runs the
  // same parser and layout call stacks (expat buffers + Section layout locals).
  if (xTaskCreate(&PreIndexer::taskTrampoline, "preIdx", 8192, this, 1, &handle_) != pdPASS) {
    LOG_ERR("PRE", "Failed to create pre-index task");
    handle_ = nullptr;
  }
}

void PreIndexer::taskTrampoline(void* arg) { static_cast<PreIndexer*>(arg)->run(); }

void PreIndexer::run() {
  while (true) {
    if (cancel_.load(std::memory_order_relaxed)) {
      break;
    }

    // A scan requested after this worker spawned: restart the walk from the
    // root. Queue dedup keeps jobs unique across repeated walks.
    if (scanRequested_.exchange(false, std::memory_order_acq_rel)) {
      QueueLock lock(mutex_);
      dirs_.clear();
      dirs_.push_back({"/", 0});
      LOG_INF("PRE", "Scan started, %zu books queued", queue_.size());
    }

    // One directory per iteration so cancellation stays responsive.
    std::string dirPath;
    uint8_t dirDepth = 0;
    bool haveDir = false;
    if (mutex_ != nullptr) {
      QueueLock lock(mutex_);
      if (!dirs_.empty()) {
        PendingDir dir = std::move(dirs_.back());
        dirs_.pop_back();
        dirPath = std::move(dir.path);
        dirDepth = dir.depth;
        haveDir = true;
      }
    }
    if (haveDir) {
      scanOneDir(dirPath, dirDepth);
      vTaskDelay(1);  // yield: keep the main loop's time slice
      continue;
    }

    // Then the book queue: layer A + layer B per book.
    std::string path;
    if (mutex_ == nullptr) {
      break;
    }
    {
      QueueLock lock(mutex_);
      if (queue_.empty()) {
        break;
      }
      path = std::move(queue_.front());
      queue_.erase(queue_.begin());
    }

    const JobResult result = processJob(path);
    if (result == JobResult::Cancelled) {
      QueueLock lock(mutex_);
      queue_.insert(queue_.begin(), std::move(path));
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  done_.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

void PreIndexer::scanOneDir(const std::string& dirPath, const uint8_t depth) {
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_DBG("PRE", "Scan: cannot open dir %s", dirPath.c_str());
    return;
  }

  char name[128];
  dir.rewindDirectory();
  while (true) {
    if (cancel_.load(std::memory_order_relaxed)) {
      return;  // dir (local) closes via destructor
    }
    auto entry = dir.openNextFile();
    if (!entry) {
      break;
    }
    entry.getName(name, sizeof(name));
    if (name[0] == '.') {
      // ".", "..", ".crosspoint" and other dot-dirs are never book content
      continue;
    }

    std::string child = dirPath;
    if (child.back() != '/') {
      child += '/';
    }
    child += name;

    const bool isDir = entry.isDirectory();
    if (isDir) {
      if (depth < MAX_SCAN_DEPTH) {
        QueueLock lock(mutex_);
        dirs_.push_back({std::move(child), static_cast<uint8_t>(depth + 1)});
      }
    } else if (FsHelpers::hasEpubExtension(child)) {
      QueueLock lock(mutex_);
      enqueueLocked(child);
    }
  }
}

PreIndexer::JobResult PreIndexer::processJob(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    // Deleted between enqueue and now (delete paths also route through
    // clearBookCache -> enqueue).
    return JobResult::Done;
  }

  auto owned = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!owned) {
    LOG_ERR("PRE", "OOM: Epub for %s", path.c_str());
    return JobResult::Failed;
  }
  std::shared_ptr<Epub> epub(std::move(owned));

  // --- Layer A: metadata + CSS cache (book.bin / css_rules.cache) ---
  // Mirror the foreground's load flags; progress hook stays unset so the run
  // is silent (no popup, no framebuffer access from this task).
  epub->setCancel(&cancel_);
  const bool aOk = epub->load(true, SETTINGS.embeddedStyle == 0);
  epub->setCancel(nullptr);
  if (!aOk) {
    if (cancel_.load(std::memory_order_relaxed)) {
      return JobResult::Cancelled;
    }
    LOG_ERR("PRE", "Pre-index metadata failed: %s", path.c_str());
    QueueLock lock(mutex_);
    if (failed_.size() < MAX_FAILED_BOOKS) {
      failed_.push_back(path);
    }
    return JobResult::Failed;
  }

  if (cancel_.load(std::memory_order_relaxed)) {
    // Layer A completed (atomic tmp+rename); requeue so layer B still runs.
    return JobResult::Cancelled;
  }

  // --- Layer B: layout of the chapter this book opens at ---
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0 || renderer_ == nullptr) {
    return JobResult::Done;
  }
  const int target = resumeSpineIndex(*epub);
  if (target >= spineCount) {
    // Finished-book sentinel or corrupt progress: nothing to pre-warm; the
    // reader clamps/handles this on open.
    return JobResult::Done;
  }

  epub->setupCacheDir();
  const NextChapterIndexParams params = collectLayoutParams(*renderer_);
  Section section(epub, target, *renderer_);
  if (section.loadSectionFile(params.fontId, params.lineCompression, params.extraParagraphSpacing,
                              params.paragraphAlignment, params.viewportWidth, params.viewportHeight,
                              params.hyphenationEnabled, params.embeddedStyle, params.imageRendering,
                              params.focusReadingEnabled)) {
    return JobResult::Done;  // already laid out (reader or NextChapterIndexer)
  }
  if (cancel_.load(std::memory_order_relaxed)) {
    return JobResult::Cancelled;
  }

  LOG_DBG("PRE", "Pre-indexing chapter %d of %s", target, path.c_str());
  const bool bOk = section.createSectionFile(params.fontId, params.lineCompression, params.extraParagraphSpacing,
                                             params.paragraphAlignment, params.viewportWidth, params.viewportHeight,
                                             params.hyphenationEnabled, params.embeddedStyle, params.imageRendering,
                                             params.focusReadingEnabled, nullptr, &cancel_);
  if (!bOk) {
    if (cancel_.load(std::memory_order_relaxed)) {
      return JobResult::Cancelled;
    }
    // Layout failed (parse error/OOM): metadata is warm; leave the section to
    // the foreground path — same outcome as today's cold open.
    LOG_ERR("PRE", "Pre-index chapter build failed: %s spine %d", path.c_str(), target);
    return JobResult::Done;
  }

  LOG_INF("PRE", "Pre-indexed: %s (chapter %d/%d)", path.c_str(), target, spineCount);
  return JobResult::Done;
}
