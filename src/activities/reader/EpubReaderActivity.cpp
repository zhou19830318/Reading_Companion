#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>

#include "AskAiActivity.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "OpenBookProgress.h"
#include "PowerMenuActivity.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "RuledLineSettingsActivity.h"
#include "SdCardFontSystem.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/voice/VoiceActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

namespace {
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;
// Bookmark-file writes are batched: at least this long after the last toggle,
// and only when the render task is quiet (see the loop() flush).
constexpr unsigned long BOOKMARK_SAVE_IDLE_MS = 2000UL;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Load the SD card face matching the saved font size. ReaderActivity does
  // this too, but the reader is also entered directly (silent resume, root
  // Settings, KOReader sync) where nothing else would run it — and
  // CrossPointSettings::getReaderFontId() reports whatever face is currently
  // loaded, so a stale face silently pins the layout to the old size.
  sdFontSystem.ensureLoaded(renderer);

  epub->setupCacheDir();

  // FR-01: dual-slot read first (progress.bin is the freshness record; the
  // slots carry the 6-byte legacy payload). A pre-M1 cache has only the old
  // 4/6-byte progress.bin, which is too short to parse as the freshness
  // record — readNewest fails and the legacy read below handles it.
  {
    uint8_t data[6] = {};
    uint16_t seq = 0;
    if (ProgressFile::readNewest(epub->getCachePath(), data, sizeof(data), sizeof(data), &seq)) {
      progressSeq_ = ProgressSlots::nextSeq(seq);
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress slots");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      LOG_DBG("ERS", "Loaded cache (slot): %d, %d", currentSpineIndex, nextPageNumber);
    } else {
      // No dual-slot state (fresh or legacy-only cache): seed the counter past
      // anything on the card so the first save never collides with old slots.
      progressSeq_ = ProgressSlots::nextSeq(ProgressFile::readNewestSeq(epub->getCachePath()));

      // Pre-M1 legacy payload only. Once the dual-slot writer has run,
      // progress.bin is the FR-01 freshness record, so bytes 0..1 are its
      // sequence number, not a spine index — parsing it here overwrote the
      // good slot value with e.g. 21328 on every open and re-armed the
      // out-of-range recovery above forever.
      HalFile f;
      if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
        uint8_t legacy[6];
        int dataSize = f.read(legacy, 6);
        if (dataSize == 4 || dataSize == 6) {
          currentSpineIndex = legacy[0] + (legacy[1] << 8);
          nextPageNumber = legacy[2] + (legacy[3] << 8);
          if (nextPageNumber == UINT16_MAX) {
            // UINT16_MAX is an in-memory navigation sentinel for "open previous
            // chapter on its last page". It should never be treated as persisted
            // resume state after sleep or reopen.
            LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
            nextPageNumber = 0;
          }
          cachedSpineIndex = currentSpineIndex;
          LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
        }
        if (dataSize == 6) {
          cachedChapterTotalPageCount = legacy[4] + (legacy[5] << 8);
        }
      }
    }
  }
  // A persisted spine index is only legal in [0, spineCount] — spineCount is the
  // "finished the book" sentinel that shows the end-of-book screen. Anything past
  // that is a stale/corrupt progress record (e.g. written against a different
  // revision of the EPUB). render() clamps on every frame, but chapter and percent
  // jumps hand the raw value straight to getSpineItem(), so an out-of-range value
  // resurfaces as an error there and leaves the book pinned to the end-of-book
  // screen instead of recovering to the last real chapter.
  if (const int spineCount = epub->getSpineItemsCount();
      spineCount > 0 && (currentSpineIndex < 0 || currentSpineIndex > spineCount)) {
    LOG_ERR("ERS", "Persisted spine index %d out of range (0..%d); recovering to last chapter", currentSpineIndex,
            spineCount);
    currentSpineIndex = spineCount - 1;
    nextPageNumber = 0;
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = 0;
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  loadCachedBookmarks();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Stop the background indexer before anything it borrows goes away: it holds
  // a shared_ptr to epub and a reference to renderer, and a running parse must
  // not survive into section reset / epub release / cache-dir moves.
  nextChapterIndexer_.cancelAndJoin();

  freeDictCellTable();

  // Last-chance flush of a deferred bookmark write (deep sleep tears the
  // reader down through here too, so nothing user-visible is left unsaved).
  flushBookmarks();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::onPause() {
  // Another activity (menu, settings, dictionary, voice, ...) is now on top and
  // the reader will not render until it returns. The background indexer must
  // not keep parsing across that window: a pushed settings/font screen can
  // unload the very font whose advance table the layout is reading. The silent
  // trigger restarts it on the next render.
  nextChapterIndexer_.cancelAndJoin();

  // The overlay paints over the panel: whatever the reader tracked as being on
  // it is stale until the next full render re-asserts it.
  panelPageKeyValid_ = false;
  toastOnPanel_ = false;

  // The reader loop stops running while another activity is on top, and deep
  // sleep can tear down the stack from there without this activity's onExit():
  // land any deferred toggle write before yielding the screen.
  flushBookmarks();
}

void EpubReaderActivity::captureNoteExcerpt() {
  // Same page guard addBookmark() uses for its summary: the excerpt is the page
  // the mark points back to, so an out-of-range page leaves it empty and a
  // quick mark falls back to the bare tag.
  noteExcerpt_[0] = '\0';
  if (!section || notePageCount_ == 0 || notePage_ >= notePageCount_) return;
  const std::string page = BookmarkUtil::sanitizeBookmarkSummary(section->getTextFromSectionFile());
  if (page.empty()) return;
  if (page.size() >= sizeof(noteExcerpt_)) {
    // "…" is U+2026 = 3 UTF-8 bytes; cut before the reservation and back off to
    // the codepoint boundary so the excerpt never opens with half a character.
    constexpr size_t ELLIPSIS_BYTES = 3;
    size_t limit = sizeof(noteExcerpt_) - 1 - ELLIPSIS_BYTES;
    while (limit > 0 && (static_cast<unsigned char>(page[limit]) & 0xC0) == 0x80) limit--;
    snprintf(noteExcerpt_, sizeof(noteExcerpt_), "%.*s…", static_cast<int>(limit), page.c_str());
    return;
  }
  snprintf(noteExcerpt_, sizeof(noteExcerpt_), "%s", page.c_str());
}

// Shared by the voice-note menu entry and the short-press power shortcut
// (SETTINGS.shortPwrBtn == SHORT_PWRBTN::VOICE_NOTE).
void EpubReaderActivity::launchVoiceNote() {
  // M2/AI-02: capture the position now, launch the voice screen in note
  // mode. startActivityForResult keeps this reader alive on the stack, so
  // the book state the note references survives the round trip. The mic
  // rail/e-ink handoff lives in VoiceActivity (paint before MIC.begin()).
  armNoteMode();
  captureNoteExcerpt();
  auto voice = std::make_unique<VoiceActivity>(renderer, mappedInput);
  voice->setNoteContext(noteBookBase_, noteSpine_, notePage_, notePageCount_, noteExcerpt_);
  startActivityForResult(std::move(voice), [this](const ActivityResult& result) {
    noteArmed_ = false;  // one-shot consumed
    // The note browser hands a position back ("jump to page"); a plain
    // Back returns monostate and only repaints the page it overwrote.
    const auto* jump = std::get_if<ProgressChangeResult>(&result.data);
    if (jump != nullptr) {
      RenderLock lock(*this);
      clearPendingSpineCrossing();  // absolute jump supersedes a queued crossing
      currentSpineIndex = jump->spineIndex;
      nextPageNumber = jump->page;
      section.reset();
      LOG_INF("ERS", "jumped to note position: spine %d page %d", jump->spineIndex, jump->page);
    }
    requestUpdate();  // repaint the page the voice screen overwrote
  });
}

// Short power press opens the quick-action chooser: the AI question screen,
// the voice bookmark, or the dictionary character picker. The chooser returns
// PowerMenuResult{index}; Back cancels and only repaints the page. Ask-AI and
// the voice bookmark push their child from this handler (no explicit
// requestUpdate() needed — ActivityManager runs one after the handler
// returns); the dictionary picker arms the in-reader mode instead.
void EpubReaderActivity::launchPowerMenu() {
  startActivityForResult(std::make_unique<PowerMenuActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto* pick = std::get_if<PowerMenuResult>(&result.data);
                           if (pick == nullptr) return;
                           switch (pick->index) {
                             case PowerMenuActivity::PICK_ASK_AI:
                               launchAskAi();
                               break;
                             case PowerMenuActivity::PICK_VOICE_NOTE:
                               launchVoiceNote();
                               break;
                             case PowerMenuActivity::PICK_DICTIONARY:
                             default:
                               enterDictPick();
                               break;
                           }
                         });
}

// AI-01: capture the position now, then open the question picker.
// startActivityForResult keeps this reader alive on the stack, so the page the
// question refers to survives the round trip; the shared_ptr copy of the book
// lets AskAiActivity extract context on its own after the link is up.
void EpubReaderActivity::launchAskAi() {
  armNoteMode();  // one-shot capture of book base + spine/page (same fields as voice)
  auto ask = std::make_unique<AskAiActivity>(renderer, mappedInput);
  ask->setBookContext(epub, noteBookBase_, noteSpine_, notePage_, notePageCount_);
  startActivityForResult(std::move(ask), [this](const ActivityResult&) { noteArmed_ = false; });
}

// ── dictionary character picker ─────────────────────────────────────────────

void EpubReaderActivity::enterDictPick() {
  dictPickActive_ = true;
  dictShowHelp_ = true;
  dictCursor_ = 0;
  dictCellTableValid_ = false;
  automaticPageTurnActive = false;
  dictEnterMs_ = millis();
  // The Confirm release that committed the quick-action menu is about to reach
  // this loop; the handler below swallows it within a short window instead of
  // looking up the first character.
  ignoreNextConfirmRelease = true;
  requestUpdate();
}

void EpubReaderActivity::exitDictPick() {
  dictPickActive_ = false;
  dictShowHelp_ = false;
  freeDictCellTable();
}

void EpubReaderActivity::freeDictCellTable() {
  if (dictChars_ != nullptr) {
    heap_caps_free(dictChars_);
    dictChars_ = nullptr;
  }
  if (dictLines_ != nullptr) {
    heap_caps_free(dictLines_);
    dictLines_ = nullptr;
  }
  dictCharsCap_ = 0;
  dictLinesCap_ = 0;
  dictCharCount_ = 0;
  dictLineCount_ = 0;
  dictCellTableValid_ = false;
}

// Builds the per-glyph cell index for one rendered page. The pen walk mirrors
// GfxRenderer::drawText exactly — same skips (niqqud, combining marks), same
// ligature step, same differential rounding of (previous advance + kerning) —
// so cells track the glyphs at any font size or spacing. PSRAM first, internal
// heap as fallback; the table is freed when the picker closes.
void EpubReaderActivity::buildDictCellTable(const Page& page, const int fontId, const int xOffset, const int yOffset) {
  dictCellTableValid_ = false;
  dictCharCount_ = 0;
  dictLineCount_ = 0;

  const auto& fontMap = renderer.getFontMap();
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("ERS", "dict picker: font %d not registered", fontId);
    return;
  }
  const auto& font = fontIt->second;

  // Pass 1: count selectable cells (an upper bound — combining marks and
  // ligatures only shrink it) and the text lines that carry any.
  size_t charCount = 0;
  size_t lineCount = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    if (line.getBlock()->getBlockStyle().isRtl) continue;  // the walk assumes LTR
    size_t lineChars = 0;
    for (const auto& w : line.getBlock()->getWords()) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(w.c_str());
      while (utf8NextCodepoint(&p) != 0) lineChars++;
    }
    if (lineChars == 0) continue;
    charCount += lineChars;
    lineCount++;
  }
  if (charCount == 0 || lineCount == 0) return;

  if (dictCharsCap_ < charCount) {
    if (dictChars_ != nullptr) {
      heap_caps_free(dictChars_);
      dictChars_ = nullptr;
      dictCharsCap_ = 0;
    }
    dictChars_ = static_cast<DictChar*>(heap_caps_malloc(charCount * sizeof(DictChar), MALLOC_CAP_SPIRAM));
    if (dictChars_ == nullptr) {
      dictChars_ =
          static_cast<DictChar*>(heap_caps_malloc(charCount * sizeof(DictChar), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (dictChars_ == nullptr) {
      LOG_ERR("ERS", "dict picker OOM: %u chars (%u bytes)", static_cast<unsigned>(charCount),
              static_cast<unsigned>(charCount * sizeof(DictChar)));
      return;
    }
    dictCharsCap_ = charCount;
  }
  if (dictLinesCap_ < lineCount) {
    if (dictLines_ != nullptr) {
      heap_caps_free(dictLines_);
      dictLines_ = nullptr;
      dictLinesCap_ = 0;
    }
    dictLines_ =
        static_cast<DictLine*>(heap_caps_malloc(lineCount * sizeof(DictLine), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (dictLines_ == nullptr) {
      LOG_ERR("ERS", "dict picker OOM: %u lines", static_cast<unsigned>(lineCount));
      return;
    }
    dictLinesCap_ = lineCount;
  }

  // Pass 2: walk every word and emit one cell per glyph.
  size_t lineIdx = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& lineEl = static_cast<const PageLine&>(*el);
    const auto* block = lineEl.getBlock().get();
    if (block->getBlockStyle().isRtl) continue;
    const auto& words = block->getWords();
    const auto& wordXpos = block->getWordXpos();
    const auto& wordStyles = block->getWordStyles();
    if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) continue;

    const int lineTop = lineEl.yPos + yOffset;
    const size_t firstChar = dictCharCount_;
    for (size_t i = 0; i < words.size(); i++) {
      const std::string& w = words[i];
      const auto style = wordStyles[i];
      int penX = lineEl.xPos + wordXpos[i] + xOffset;
      int32_t prevAdvanceFP = 0;
      uint32_t prevCp = 0;
      const unsigned char* p = reinterpret_cast<const unsigned char*>(w.c_str());
      while (*p != '\0') {
        uint32_t cp = utf8NextCodepoint(&p);
        if (cp == 0) break;
        if (cp >= 0x0591 && cp <= 0x05C7) continue;  // Hebrew niqqud: skipped by drawText too
        if (utf8IsCombiningMark(cp)) continue;       // drawn over the base glyph, no cell
        const char* cursor = reinterpret_cast<const char*>(p);
        cp = font.applyLigatures(cp, cursor, style);
        p = reinterpret_cast<const unsigned char*>(cursor);
        if (prevCp != 0) {
          penX += fp4::toPixel(prevAdvanceFP + font.getKerning(prevCp, cp, style));
        }
        const EpdGlyph* glyph = font.getGlyph(cp, style);
        int32_t advanceFP = glyph != nullptr ? glyph->advanceX : 0;
        if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
          advanceFP = (advanceFP + 1) / 2;
        }
        if (dictCharCount_ < dictCharsCap_) {
          DictChar& cell = dictChars_[dictCharCount_++];
          cell.cp = cp;
          cell.x = static_cast<int16_t>(penX);
          cell.adv = static_cast<int16_t>(std::max(1, fp4::toPixel(advanceFP)));
          cell.line = static_cast<uint8_t>(lineIdx);
        }
        prevAdvanceFP = advanceFP;
        prevCp = cp;
      }
    }
    if (dictCharCount_ > firstChar) {
      dictLines_[lineIdx] = DictLine{static_cast<int16_t>(lineTop), static_cast<uint16_t>(firstChar),
                                     static_cast<uint16_t>(dictCharCount_ - firstChar)};
      lineIdx++;
    }
  }
  dictLineCount_ = lineIdx;
  if (dictCursor_ >= dictCharCount_) dictCursor_ = 0;
  dictCellTableValid_ = dictCharCount_ > 0 && dictLineCount_ > 0;
  LOG_INF("ERS", "dict picker: %u chars on %u lines", static_cast<unsigned>(dictCharCount_),
          static_cast<unsigned>(dictLineCount_));
}

void EpubReaderActivity::moveDictCursor(const int dChar, const int dLine) {
  if (!dictCellTableValid_) return;
  bool moved = false;
  if (dLine != 0) {
    const int curLine = static_cast<int>(dictChars_[dictCursor_].line) + dLine;
    if (curLine >= 0 && curLine < static_cast<int>(dictLineCount_)) {
      const DictLine& ln = dictLines_[curLine];
      const int curCenter = dictChars_[dictCursor_].x + dictChars_[dictCursor_].adv / 2;
      size_t best = ln.first;
      int bestDist = INT16_MAX;
      for (size_t i = ln.first; i < ln.first + ln.count && i < dictCharCount_; i++) {
        const int center = dictChars_[i].x + dictChars_[i].adv / 2;
        const int dist = std::abs(center - curCenter);
        if (dist < bestDist) {
          bestDist = dist;
          best = i;
        }
      }
      dictCursor_ = best;
      moved = true;
    }
  } else if (dChar != 0) {
    const int next = static_cast<int>(dictCursor_) + dChar;
    if (next >= 0 && next < static_cast<int>(dictCharCount_)) {
      dictCursor_ = static_cast<size_t>(next);
      moved = true;
    }
  }
  if (moved) {
    dictShowHelp_ = false;
    ignoreNextConfirmRelease = false;  // any orphaned menu edge is long gone
    requestUpdate();
  }
}

void EpubReaderActivity::launchDictionaryLookup() {
  if (!dictCellTableValid_ || dictCursor_ >= dictCharCount_) return;
  const uint32_t cp = dictChars_[dictCursor_].cp;
  LOG_INF("ERS", "dictionary lookup for U+%04X", static_cast<unsigned>(cp));
  startActivityForResult(std::make_unique<DictionaryActivity>(renderer, mappedInput, cp),
                         [this](const ActivityResult&) { requestUpdate(); });  // repaint page + cursor box
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  // Deferred bookmark save (see addBookmark()): write once the debounce has
  // elapsed AND the render task is quiet, so the SD write never waits behind
  // (nor holds up) a page render or its staging reads.
  if (bookmarksDirty && (millis() - bookmarksDirtyAt) >= BOOKMARK_SAVE_IDLE_MS && !RenderLock::peek()) {
    flushBookmarks();
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  // ── Dictionary character picker owns every input while active ──────────
  // The front pair walks characters, side up/down walks lines, Confirm opens
  // the lookup, Back leaves the mode. The block runs before every other
  // handler so both press and release edges die here: ReaderUtils::
  // detectPageTurn can fire on releases (long-press behavior) or tilt, and
  // the Confirm/turn/Back releases below would otherwise leak through.
  if (dictPickActive_) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      moveDictCursor(0, -1);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      moveDictCursor(0, 1);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      moveDictCursor(-1, 0);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      moveDictCursor(1, 0);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (ignoreNextConfirmRelease && millis() - dictEnterMs_ < 1000) {
        ignoreNextConfirmRelease = false;  // the quick-action menu's own release
      } else {
        ignoreNextConfirmRelease = false;
        launchDictionaryLookup();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Short Back leaves the picker; the page is already on the panel, the
      // repaint just drops the cursor box and restores the status bar title.
      exitDictPick();
      requestUpdate();
    }
    return;
  }

  // Enter reader menu activity on short-press Confirm. A long-press that fired a bound
  // function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      const int currentPage = section ? section->currentPage + 1 : 0;
      const int totalPages = section ? section->pageCount : 0;
      float bookProgress = 0.0f;
      if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                                 renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                                 SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty(), true),
                             [this](const ActivityResult& result) {
                               // Always apply orientation change even if the menu was cancelled
                               const auto& menu = std::get<MenuResult>(result.data);
                               applyOrientation(menu.orientation);
                               toggleAutoPageTurn(menu.pageTurnOption);
                               if (!result.isCancelled) {
                                 onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                               }
                             });
    }
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        // Hold ~0.4s drops a bookmark at the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        // Hold ~1s launches KOReader sync. If sync can't run (no credentials stored), fall
        // through so the normal Confirm-release still opens the reader menu.
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
          if (launchKOReaderSync()) {
            ignoreNextConfirmRelease = true;  // sync launched or error shown; suppress menu open
            return;
          }
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  // Short power press = quick-action chooser: AI书童 (AI-01) or the voice
  // bookmark, whichever of the two the reader was last used for
  // (SETTINGS.shortPwrBtn == VOICE_NOTE, appended after FOOTNOTES so stored
  // enum indices keep their meaning). The Down check mirrors the footnote
  // binding so the POWER+DOWN screenshot combo in main.cpp never triggers it.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::VOICE_NOTE &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    launchPowerMenu();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      clearPendingSpineCrossing();
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // Same deferred-crossing scheme as pageTurn(): queue the spine move and
    // let the render task apply it under the lock it already owns. The
    // RenderLock take that used to live here stalled the input loop whenever
    // the render task was busy with an on-demand section build.
    if (nextTriggered) {
      pendingSpineDelta.fetch_add(1, std::memory_order_relaxed);
    } else if (currentSpineIndex > 0) {
      pendingSpineDelta.fetch_sub(1, std::memory_order_relaxed);
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    clearPendingSpineCrossing();  // absolute jump supersedes a queued crossing
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
        RenderLock lock(*this);
        clearPendingSpineCrossing();  // absolute jump supersedes a queued crossing
        currentSpineIndex = sync.spineIndex;
        nextPageNumber = sync.page;
        section.reset();
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);
              clearPendingSpineCrossing();  // absolute jump supersedes a queued crossing

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::ASK_AI: {
      launchAskAi();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      // Same picker the quick-action menu opens: box a glyph, Confirm looks
      // the character up on the relay's dictionary endpoint.
      enterDictPick();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VOICE_NOTE: {
      launchVoiceNote();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::RULED_LINE_SETTINGS: {
      startActivityForResult(std::make_unique<RuledLineSettingsActivity>(renderer, mappedInput),
                             [this](const ActivityResult& result) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READER_SETTINGS: {
      // Push the normal settings screen in embedded mode: it opens on the
      // Reader category and Back pops back here instead of going home.
      const LayoutSignature before = makeLayoutSignature();
      startActivityForResult(std::make_unique<SettingsActivity>(renderer, mappedInput, /*embedded=*/true),
                             [this, before](const ActivityResult& result) { applyLayoutChanges(before); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        clearPendingSpineCrossing();  // cache goes away; a queued crossing is void
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      // The list reads the file, not this RAM cache — land any deferred toggle
      // write first so it opens on the current state.
      flushBookmarks();
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;  // acted: launched the sync activity
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

// Everything Section::loadSectionFile() keys its cache on, plus the two inputs
// that determine the viewport size (margin, orientation) and the status-bar
// height that consumes part of the bottom edge.
EpubReaderActivity::LayoutSignature EpubReaderActivity::makeLayoutSignature() const {
  LayoutSignature sig;
  sig.fontId = SETTINGS.getReaderFontId();
  sig.lineCompression = SETTINGS.getReaderLineCompression();
  sig.paragraphAlignment = SETTINGS.paragraphAlignment;
  sig.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  sig.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  sig.embeddedStyle = SETTINGS.embeddedStyle;
  sig.focusReadingEnabled = SETTINGS.focusReadingEnabled;
  sig.imageRendering = SETTINGS.imageRendering;
  sig.screenMargin = SETTINGS.screenMargin;
  sig.orientation = SETTINGS.orientation;
  sig.statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  return sig;
}

// Called when the embedded settings panel returns. Re-projects the renderer
// orientation first (so the viewport below is computed in the new frame), then
// drops the loaded section so render() re-runs loadSectionFile() — which
// itself returns false on any parameter mismatch and rebuilds the pages.
// The staged PSRAM buffers need no explicit invalidation: StageKey already
// carries fontId / lineCompression / vw / vh / orientation, so render() falls
// through to the slow path and re-stages with the new settings.
void EpubReaderActivity::applyLayoutChanges(const LayoutSignature& before) {
  RenderLock lock(*this);
  // Re-resolve the SD card face for the (possibly new) font size BEFORE
  // sampling `after`. getReaderFontId() reports the currently *loaded* face and
  // only SdCardFontSystem::ensureLoaded() reloads it, so without this a pure
  // font-size change in the embedded panel would compare equal and be dropped.
  sdFontSystem.ensureLoaded(renderer);

  const LayoutSignature after = makeLayoutSignature();
  if (before == after) return;

  if (before.orientation != after.orientation) {
    ReaderUtils::applyOrientation(renderer, after.orientation);
  }
  if (section) {
    // Preserve the reading position across the re-layout (same contract as
    // applyOrientation), otherwise render() would restart at nextPageNumber.
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

// Consumes the spine movement queued by the input-side crossing branches of
// pageTurn()/chapter-skip. Must run with the render lock held (render() entry
// is the only caller). Landing rules mirror the old under-lock code: a
// forward crossing opens the new chapter's first page; a backward page-turn
// reopens the previous chapter's last page (pendingPageJump = max, clamped by
// the render path); a backward chapter-skip opens its page 0.
void EpubReaderActivity::applyPendingSpineDelta() {
  const int delta = pendingSpineDelta.exchange(0, std::memory_order_acq_rel);
  if (delta == 0 || !epub) {
    return;
  }
  const bool backToLastPage = pendingBackLandsOnLastPage.exchange(false, std::memory_order_acq_rel);
  const long long lastSpine = static_cast<long long>(epub->getSpineItemsCount());
  long long next = static_cast<long long>(currentSpineIndex) + delta;
  if (next < 0) {
    next = 0;
  } else if (next > lastSpine) {
    next = lastSpine;  // == count renders the end-of-book screen
  }
  currentSpineIndex = static_cast<int>(next);
  if (delta < 0) {
    if (backToLastPage) {
      pendingPageJump = std::numeric_limits<uint16_t>::max();
    } else {
      nextPageNumber = 0;
    }
  } else {
    nextPageNumber = 0;
  }
  section.reset();
  LOG_DBG("ERS", "Applied pending spine delta %+d -> %d", delta, currentSpineIndex);
}

void EpubReaderActivity::clearPendingSpineCrossing() {
  pendingSpineDelta.store(0, std::memory_order_relaxed);
  pendingBackLandsOnLastPage.store(false, std::memory_order_relaxed);
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (!section) {
    // End-of-book holds no loaded section (render() returns before the load), so
    // there is no currentPage to step and touching the pointer here is undefined
    // behaviour. Page-back reopens the last real chapter instead of leaving the
    // reader stuck on the end-of-book screen.
    if (!isForwardTurn && currentSpineIndex > 0) {
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      pendingBackLandsOnLastPage.store(true, std::memory_order_relaxed);
      pendingSpineDelta.fetch_sub(1, std::memory_order_relaxed);
      requestUpdate();
    }
    lastPageTurnTime = millis();
    return;
  }
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // Crossing forward: queue the spine move for the render task instead of
      // taking RenderLock here — the input loop must stay responsive while the
      // render task performs the (up to ~9 s) on-demand section build.
      pendingSpineDelta.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      pendingBackLandsOnLastPage.store(true, std::memory_order_relaxed);
      pendingSpineDelta.fetch_sub(1, std::memory_order_relaxed);
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // Cross-chapter turns queued by pageTurn()/chapter-skip land here: this
  // task already owns the render lock, so the spine move and section reload
  // can never race a concurrent render.
  applyPendingSpineDelta();

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    // The popup replaces the tracked page on the panel.
    panelPageKeyValid_ = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // Capture-and-invalidate the panel identity: every render undoes it, the two
  // full page paths below re-assert it. The bookmark long-press requests a
  // render while an untouched page is still on the panel; that is the only
  // case the toast fast path may window-push over the existing pixels.
  const bool hadPageOnPanel = panelPageKeyValid_;
  panelPageKeyValid_ = false;
  toastOnPanel_ = false;

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const auto loadCachedSection = [this, viewportWidth, viewportHeight]() {
      return section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, SETTINGS.focusReadingEnabled);
    };

    if (!loadCachedSection()) {
      LOG_DBG("ERS", "Cache not found, building...");

      // Single-flight with the background indexer: when it is building this
      // same chapter, wait for it under the popup and use its result; anything
      // else it holds is superseded. Two builds must never write sections
      // concurrently (shared SD, shared advance tables).
      bool haveCache = false;
      if (nextChapterIndexer_.running()) {
        if (nextChapterIndexer_.spineIndex() == currentSpineIndex) {
          // Keep the bar alive while we block on the background indexer.
          OpenBookProgress::reportPhase(renderer, 0, /*isLayout=*/true);
          nextChapterIndexer_.join();
          haveCache = loadCachedSection();
          if (haveCache) {
            LOG_DBG("ERS", "Cache built by background indexer");
          }
        } else {
          nextChapterIndexer_.cancelAndJoin();
        }
      }

      if (!haveCache) {
        // The popup shell is already up (drawn in ReaderActivity::onEnter or by
        // the indexer wait above). createSectionFile drives the bar through
        // popupFn, remapped into the layout slice of the shared axis.
        const auto popupFn = [this](const int percent) {
          LOG_DBG("ERS", "Indexing progress: %d%%", percent);
          OpenBookProgress::reportPhase(renderer, percent, /*isLayout=*/true);
        };

        if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                        SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                        viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          showPendingSyncSaveError();
          return;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    // Section is ready; retire the loading popup so it does not linger under
    // the first rendered page. (finish() is a no-op if ReaderActivity never
    // started a popup — e.g. a chapter-turn rebuild.)
    OpenBookProgress::finish(renderer);

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  // NB: the framebuffer is deliberately NOT cleared here. Two paths can still
  // bail out without pushing — the section rebuild above and the SD page load
  // below — and both show GUI.drawPopup(), which pushes whatever the
  // framebuffer holds. Clearing first meant a failed load pushed a blank white
  // screen plus the popup; the previous page is still in the buffer, so popups
  // now draw over it. Each path that really renders clears first instead.

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  // Fast path: this exact page was already laid out and rendered into PSRAM
  // while the previous page sat idle on the panel. Only copy the frame and push
  // it — no layout, no glyph fetch, no renders.
  const StageKey stageKey =
      makeStageKey(currentSpineIndex, section->currentPage, viewportWidth, viewportHeight, section->pageCount);
  if (stagedValid_ && !(stagedKey_ == stageKey)) {
    LOG_DBG("ERS", "Stage key mismatch: have (%d,%d,%d,%u,%u,%u) want (%d,%d,%d,%u,%u,%u)", stagedKey_.spine,
            stagedKey_.page, stagedKey_.pageCount, stagedKey_.vw, stagedKey_.vh, stagedKey_.fontId, stageKey.spine,
            stageKey.page, stageKey.pageCount, stageKey.vw, stageKey.vh, stageKey.fontId);
  }
  // Toast fast path: the panel already shows exactly this page and the only
  // new content is the bookmark popup plus the bookmark icon in the status bar.
  // Drawing those two into the framebuffer and window-pushing just their rects
  // skips the page reload, the re-render and its grayscale pass that the full
  // path below would spend (~1.6 s) before pushing the same page pixels again.
  if (showBookmarkMessage && hadPageOnPanel && panelPageKey_ == stageKey && !dictPickActive_ && !pendingScreenshot &&
      !pendingSyncSaveError) {
    renderStatusBar();
    const Rect popupRect =
        GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED), false, false);
    // Inflate by the popup frame thickness (plus slack) so the border is
    // included, matching the tail path below.
    const int pad = UITheme::getInstance().getMetrics().popupFrameThickness + 2;
    if (renderer.displayWindow(popupRect.x - pad, popupRect.y - pad, popupRect.width + pad * 2,
                               popupRect.height + pad * 2)) {
      // Popup first so the toggle result is visible as soon as the panel
      // allows; the strip then carries the bookmark icon change. The strip is
      // taken from the page viewport bottom (content is anchored to the raw
      // viewable margin, a few pixels above it) down to the screen edge.
      const int barHeight = UITheme::getInstance().getStatusBarHeight();
      if (barHeight > 0) {
        renderer.displayWindow(0, renderer.getScreenHeight() - orientedMarginBottom - barHeight - 8,
                               renderer.getScreenWidth(), orientedMarginBottom + barHeight + 8);
      }
      toastOnPanel_ = true;
      panelPageKey_ = stageKey;
      panelPageKeyValid_ = true;
      LOG_DBG("ERS", "Bookmark toast fast path (window push)");
      return;
    }
    // The window pipeline rejected the popup rect (empty or off-panel): fall
    // through so the popup still reaches the panel via the full render.
  }
  if (stagedValid_ && stagedKey_ == stageKey && !dictPickActive_) {
    currentPageFootnotes = std::move(stagedFootnotes_);
    const auto start = millis();
    displayStagedContents();
    stagedValid_ = false;
    LOG_DBG("ERS", "Rendered staged page in %dms", millis() - start);
  } else {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderer.clearScreen();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  // Staging prepackages the next page into PSRAM; with the picker up, the
  // "next page" is a guess the reader may never make, and every cursor move
  // would redo it (~1s). Skip it while picking; it resumes on the next render
  // after the picker closes.
  if (!dictPickActive_) silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  if (section && !dictPickActive_) {
    // Prepare the page we are most likely to show next while the current page
    // sits idle on screen: lay it out and render BW plus both grayscale planes
    // into PSRAM. Cold chapters cost up to ~900ms of random SD reads and
    // ~350ms of rendering; paying it here moves all of it out of the following
    // page turn for anyone who dwells on a page longer than the work takes.
    const bool moreInChapter = section->currentPage + 1 < section->pageCount;
    const int stageSpine = moreInChapter ? currentSpineIndex : currentSpineIndex + 1;
    const int stagePage = moreInChapter ? section->currentPage + 1 : 0;
    const bool staged = stageNextPage(stageSpine, stagePage, viewportWidth, viewportHeight, orientedMarginTop,
                                      orientedMarginRight, orientedMarginLeft);
    if (!staged) {
      // The page could not be staged (image page, section cache not built yet,
      // OOM) — still warm its glyphs so the turn itself is cheaper.
      if (moreInChapter) {
        prefetchPageGlyphs(currentSpineIndex, section->currentPage + 1, viewportWidth, viewportHeight);
      } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
        prefetchPageGlyphs(currentSpineIndex + 1, 0, viewportWidth, viewportHeight);
      }
    }
  }
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  // This page is now the one on the panel (showPendingSyncSaveError above may
  // have just overridden that with its popup and cleared the flag again).
  panelPageKey_ = stageKey;
  panelPageKeyValid_ = true;

  if (showBookmarkMessage) {
    const Rect popupRect =
        GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED), false, false);
    // Only the popup differs from what is already on the panel: every path
    // reaching this tail has just pushed the page, so the controller RAM
    // matches the framebuffer outside the popup rect. Driving just the popup's
    // rect avoids a second full-panel refresh per bookmark toggle. Inflate by
    // the popup frame thickness (plus slack) so the border is included.
    const int pad = UITheme::getInstance().getMetrics().popupFrameThickness + 2;
    if (!renderer.displayWindow(popupRect.x - pad, popupRect.y - pad, popupRect.width + pad * 2,
                                popupRect.height + pad * 2)) {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    toastOnPanel_ = true;
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Keep the background indexer one to two chapters ahead of the reader.
  //
  // The previous gate only armed from the fourth-from-last page of the current
  // chapter. That fails two ways: short chapters (2-7 pages in multi-volume
  // collections) give under 4 s of margin against a ~7.5-10 s cold build, and
  // render coalescing can skip the near-end window entirely — a 40-press test
  // batch produced only 13 renders, so most chapters were entered without a
  // render pass having ever observed their final pages. Evaluating on every
  // render pass and targeting the first un-built spine in a two-chapter window
  // lets the worker finish N+1 during the current chapter's dwell and chain
  // into N+2 without a cancellation (spineIndex() only re-arms after the
  // previous build has been reaped).
  //
  // Cost per pass: at most two header-only section probes (~2-6 ms of SD I/O,
  // Section::loadSectionFile reads the magic/version/params/pageCount and
  // closes). A hit also doubles as a staleness check — a settings change makes
  // the probe fail, clear the stale cache, and retarget the worker at it.
  const int lastSpineIndex = static_cast<int>(epub->getSpineItemsCount()) - 1;
  int nextSpineIndex = -1;
  for (int i = currentSpineIndex + 1; i <= currentSpineIndex + 2 && i <= lastSpineIndex; ++i) {
    Section probe(epub, i, renderer);
    if (!probe.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                               SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                               viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                               SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
      nextSpineIndex = i;
      break;
    }
  }
  if (nextSpineIndex < 0) {
    return;
  }

  // A build for that very chapter is the only one this pass would start, so
  // let it finish; a build for a different spine is superseded (the user
  // jumped somewhere else).
  if (nextChapterIndexer_.running()) {
    if (nextChapterIndexer_.spineIndex() == nextSpineIndex) {
      return;
    }
    nextChapterIndexer_.cancelAndJoin();
  }

  // A chapter that already failed to parse once (corrupt markup, OOM) would
  // otherwise restart the build on every render pass for the whole visit.
  if (nextChapterIndexer_.lastFailedSpine() == nextSpineIndex) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  NextChapterIndexParams params;
  params.fontId = SETTINGS.getReaderFontId();
  params.lineCompression = SETTINGS.getReaderLineCompression();
  params.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  params.paragraphAlignment = SETTINGS.paragraphAlignment;
  params.viewportWidth = viewportWidth;
  params.viewportHeight = viewportHeight;
  params.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  params.embeddedStyle = SETTINGS.embeddedStyle;
  params.imageRendering = SETTINGS.imageRendering;
  params.focusReadingEnabled = SETTINGS.focusReadingEnabled;
  nextChapterIndexer_.start(epub, nextSpineIndex, renderer, params);
}

// Read a page's text out of the section cache and feed it to the SD font
// prewarm so its glyph bitmaps are resident in the PSRAM glyph cache. The
// target page is deliberately one that has not been drawn yet: the render task
// is otherwise idle once the current page is on the panel, so this runs in the
// reader's dwell time rather than inside the next page turn.
void EpubReaderActivity::prefetchPageGlyphs(const int spineIndex, const int pageIndex, const uint16_t viewportWidth,
                                            const uint16_t viewportHeight) {
  if (!epub || pageIndex < 0) return;
  const int fontId = SETTINGS.getReaderFontId();
  if (spineIndex == prefetchedSpine_ && pageIndex == prefetchedPage_ && fontId == prefetchedFont_) return;

  Section next(epub, spineIndex, renderer);
  if (!next.loadSectionFile(fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                            SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                            SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    return;  // no section cache yet - the chapter has not been laid out
  }
  if (pageIndex >= next.pageCount) return;

  next.currentPage = pageIndex;
  std::string text = next.getTextFromSectionFile();
  if (text.empty()) return;

  const auto start = millis();
  renderer.getFontCacheManager()->prewarmCache(fontId, text.c_str(), 0x01);
  LOG_DBG("ERS", "Prefetched glyphs for spine %d page %d in %lums", spineIndex, pageIndex, millis() - start);
  prefetchedSpine_ = spineIndex;
  prefetchedPage_ = pageIndex;
  prefetchedFont_ = fontId;
}

EpubReaderActivity::StageKey EpubReaderActivity::makeStageKey(const int spine, const int page, const uint16_t vw,
                                                              const uint16_t vh, const uint16_t pageCount) const {
  StageKey key;
  key.spine = spine;
  key.page = page;
  key.fontId = SETTINGS.getReaderFontId();
  key.vw = vw;
  key.vh = vh;
  key.pageCount = pageCount;
  key.lineCompression = SETTINGS.getReaderLineCompression();
  key.lineSpacing = SETTINGS.lineSpacing;
  key.alignment = SETTINGS.paragraphAlignment;
  key.extraPara = SETTINGS.extraParagraphSpacing;
  key.hyphenation = SETTINGS.hyphenationEnabled;
  key.embeddedStyle = SETTINGS.embeddedStyle;
  key.focusReading = SETTINGS.focusReadingEnabled;
  key.imageRendering = SETTINGS.imageRendering;
  key.textAA = SETTINGS.textAntiAliasing;
  key.ruledLines = SETTINGS.ruledLines;
  key.ruledStyle = SETTINGS.ruledLineStyle;
  key.ruledThick = SETTINGS.ruledLineThickness;
  key.ruledDash = SETTINGS.ruledLineDash;
  key.ruledOffset = SETTINGS.ruledLineOffset;
  key.orientation = SETTINGS.orientation;
  return key;
}

// Lays out and renders one page into PSRAM staging buffers during the reader's
// idle window. The live framebuffer — and therefore the frame the panel is
// currently showing — is never written: beginStripTarget() redirects every
// draw into the stage buffer, and clearScreen() with a strip active memsets
// only that buffer (GfxRenderer.cpp:1321-1329).
bool EpubReaderActivity::stageNextPage(const int spineIndex, const int pageIndex, const uint16_t viewportWidth,
                                       const uint16_t viewportHeight, const int orientedMarginTop,
                                       const int orientedMarginRight, const int orientedMarginLeft) {
  if (!epub || !renderer.supportsStripGrayscale()) return false;
  if (spineIndex < 0 || spineIndex >= epub->getSpineItemsCount() || pageIndex < 0) {
    LOG_DBG("ERS", "Stage skip: bad target spine=%d page=%d (spineCount=%d)", spineIndex, pageIndex,
            epub->getSpineItemsCount());
    return false;
  }

  const int fontId = SETTINGS.getReaderFontId();
  Section next(epub, spineIndex, renderer);
  if (!next.loadSectionFile(fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                            SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                            SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    LOG_DBG("ERS", "Stage skip: no section cache for spine %d", spineIndex);
    return false;  // no section cache yet - the chapter has not been laid out
  }
  if (pageIndex >= next.pageCount) {
    LOG_DBG("ERS", "Stage skip: page %d >= pageCount %d (spine %d)", pageIndex, next.pageCount, spineIndex);
    return false;
  }

  const StageKey key = makeStageKey(spineIndex, pageIndex, viewportWidth, viewportHeight, next.pageCount);
  if (stagedValid_ && stagedKey_ == key) return true;  // already staged for this exact target

  next.currentPage = pageIndex;
  auto page = next.loadPageFromSectionFile();
  if (!page) {
    LOG_DBG("ERS", "Stage skip: page load failed (%d,%d)", spineIndex, pageIndex);
    return false;
  }
  // Image pages deliberately take a double FAST refresh with selective blanking
  // (pablohc's technique), which needs a freshly rendered frame at display
  // time. Never stage those.
  if (page->hasImages()) {
    LOG_DBG("ERS", "Stage skip: image page (%d,%d)", spineIndex, pageIndex);
    return false;
  }

  const int gh = renderer.getDisplayHeight();
  const int gwBytes = renderer.getDisplayWidthBytes();
  const size_t planeSize = static_cast<size_t>(gwBytes) * gh;

  if (!stagedBw_) {
    // 3 x 48 KB is far above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4096 B), so
    // these land in PSRAM rather than the ~68 KB of free internal DRAM. Owned
    // by unique_ptr for the activity lifetime — freed on onExit(), no manual
    // free on any error path.
    stagedBw_ = makeUniqueNoThrow<uint8_t[]>(planeSize);
    stagedLsb_ = makeUniqueNoThrow<uint8_t[]>(planeSize);
    stagedMsb_ = makeUniqueNoThrow<uint8_t[]>(planeSize);
    if (!stagedBw_ || !stagedLsb_ || !stagedMsb_) {
      LOG_ERR("ERS", "OOM: page stage buffers (%u bytes each)", static_cast<unsigned>(planeSize));
      stagedBw_.reset();
      stagedLsb_.reset();
      stagedMsb_.reset();
      return false;
    }
    LOG_DBG("ERS", "Page stage buffers: %u bytes x3 in PSRAM", static_cast<unsigned>(planeSize));
  }

  const auto start = millis();

  // BW frame. Mirrors renderContents(): scan pass records which glyphs/styles
  // the page needs, endScanAndPrewarm() rasterizes them, then the real render
  // draws from cache. The prewarm scope stays alive across the grayscale passes
  // exactly as it does on the live path, because its destructor is what frees
  // the per-style mini data afterwards.
  auto scope = renderer.getFontCacheManager()->createPrewarmScope();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.beginStripTarget(stagedBw_.get(), 0, gh);
  renderer.clearScreen(0xFF);
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  scope.endScanAndPrewarm();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  drawRuledLines(*page, fontId, orientedMarginTop, orientedMarginLeft, orientedMarginRight);
  renderer.endStripTarget();

  if (SETTINGS.textAntiAliasing) {
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.beginStripTarget(stagedLsb_.get(), 0, gh);
    renderer.clearScreen(0x00);
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderer.endStripTarget();

    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.beginStripTarget(stagedMsb_.get(), 0, gh);
    renderer.clearScreen(0x00);
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderer.endStripTarget();
  }
  renderer.setRenderMode(GfxRenderer::BW);

  stagedKey_ = key;
  stagedFootnotes_ = page->footnotes;
  stagedValid_ = true;
  LOG_DBG("ERS", "Staged spine %d page %d in %lums", spineIndex, pageIndex, millis() - start);
  return true;
}

// Push path for a staged frame: identical to the tail of renderContents()
// (status bar, refresh cadence, grayscale plane push, controller re-sync) but
// the pixels come from the stage buffers, so no render runs here.
void EpubReaderActivity::displayStagedContents() {
  const auto t0 = millis();
  const int gh = renderer.getDisplayHeight();
  const int gwBytes = renderer.getDisplayWidthBytes();
  const size_t planeSize = static_cast<size_t>(gwBytes) * gh;

  memcpy(renderer.getFrameBuffer(), stagedBw_.get(), planeSize);
  const auto tCopy = millis();

  renderStatusBar();
  const auto tStatus = millis();

  // Staged pages are guaranteed image-free; the shared cadence decides whether
  // this push also runs the ghost cleanup.
  ReaderUtils::displayReaderPage(renderer);
  const auto tDisplay = millis();

  if (SETTINGS.textAntiAliasing && stagedLsb_ && stagedMsb_) {
    const auto* lsbBase = static_cast<const uint8_t*>(stagedLsb_.get());
    const auto* msbBase = static_cast<const uint8_t*>(stagedMsb_.get());
    constexpr int STRIP_ROWS = 80;
    for (int y = 0; y < gh; y += STRIP_ROWS) {
      const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
      renderer.writeGrayscalePlaneStrip(true, lsbBase + static_cast<size_t>(y) * gwBytes, y, rows);
    }
    const auto tGrayLsb = millis();

    for (int y = 0; y < gh; y += STRIP_ROWS) {
      const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
      renderer.writeGrayscalePlaneStrip(false, msbBase + static_cast<size_t>(y) * gwBytes, y, rows);
    }
    const auto tGrayMsb = millis();

    renderer.setRenderMode(GfxRenderer::BW);
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();

    // BW framebuffer was just filled from the stage buffer; re-sync controller
    // RAM for the next differential page turn directly from it.
    renderer.cleanupGrayscaleWithFrameBuffer();
    const auto tCleanup = millis();

    LOG_DBG("ERS",
            "Staged render: copy=%lums status=%lums display=%lums gray_lsb=%lums gray_msb=%lums "
            "gray_display=%lums cleanup=%lums total=%lums",
            tCopy - t0, tStatus - tCopy, tDisplay - tStatus, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
            tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tCleanup - t0);
  } else {
    const auto tEnd = millis();
    LOG_DBG("ERS", "Staged render: copy=%lums status=%lums display=%lums total=%lums", tCopy - t0, tStatus - tCopy,
            tDisplay - tStatus, tEnd - t0);
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  progressSeq_ = ProgressSlots::nextSeq(progressSeq_);
  if (!EpubReaderUtils::saveProgress(*epub, progressSeq_, spineIndex, currentPage, pageCount)) {
    return false;
  }

  // Mirror chapter + book % into the recent-books store so the workbench
  // "now reading" card can show them without loading the EPUB. This runs on
  // every page turn, so updateProgress() itself drops redundant writes.
  uint8_t percent = 0;
  if (pageCount > 0 && spineIndex >= 0) {
    const float p = epub->calculateProgress(spineIndex, static_cast<float>(currentPage) / pageCount) * 100.0f;
    if (p > 0.0f) {
      percent = static_cast<uint8_t>(p + 0.5f);
    }
  }
  const int tocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (tocIndex != -1) {
    const auto tocItem = epub->getTocItem(tocIndex);
    RECENT_BOOKS.updateProgress(epub->getPath(), percent, tocItem.title);
  } else {
    RECENT_BOOKS.updateProgress(epub->getPath(), percent, "");
  }
  return true;
}

// Optional "ruled lines" reading aid: draw a dashed rule spanning the full text
// column under each line of text (writing-paper style). Purely visual overlay
// drawn onto the BW framebuffer after text render — no layout/cache impact.
void EpubReaderActivity::drawRuledLines(const Page& page, const int fontId, const int orientedMarginTop,
                                        const int orientedMarginLeft, const int orientedMarginRight) const {
  if (!SETTINGS.ruledLines) return;
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int x1 = orientedMarginLeft;
  const int x2 = renderer.getScreenWidth() - orientedMarginRight;
  if (x2 <= x1) return;

  // Style params (all render-time; see CrossPointSettings).
  // bias 8: value 0..20 -> -8..+12px
  const int offset = static_cast<int>(SETTINGS.ruledLineOffset) - 8;
  const int thickness = std::min(3, static_cast<int>(SETTINGS.ruledLineThickness) + 1);  // 0/1/2 -> 1/2/3
  static constexpr int DASH_TABLE[3] = {8, 4, 2};
  static constexpr int GAP_TABLE[3] = {8, 4, 2};
  const int densityIdx = std::min(2, static_cast<int>(SETTINGS.ruledLineDash));
  const int dash = DASH_TABLE[densityIdx];
  const int gap = GAP_TABLE[densityIdx];
  const uint8_t style = SETTINGS.ruledLineStyle;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const int y = el->yPos + orientedMarginTop + ascender + offset;
    switch (style) {
      case 1:  // solid
        renderer.fillRect(x1, y, x2 - x1, thickness, true);
        break;
      case 2:  // dotted (1px dots, gap between)
        for (int x = x1; x < x2; x += gap + 1) {
          renderer.fillRect(x, y, 1, thickness, true);
        }
        break;
      case 3: {  // dash-dot: long dash, gap, dot, gap, repeat
        int x = x1;
        while (x < x2) {
          const int seg = std::min(dash, x2 - x);
          renderer.fillRect(x, y, seg, thickness, true);  // dash
          x += dash + gap;
          if (x >= x2) break;
          renderer.fillRect(x, y, 1, thickness, true);  // dot
          x += 1 + gap;
        }
        break;
      }
      case 4: {  // zigzag: alternating up/down diagonal segments
        // horizontal span of one V leg
        const int period = dash + gap;
        const int amp = std::min(3, std::max(1, thickness));  // peak height, tied to thickness
        int px = x1;
        int py = y;
        for (int x = x1; x < x2; x += period) {
          const int nx = std::min(x + period, x2);
          const int ny = (py == y) ? y + amp : y;  // toggle between baseline and baseline+amp
          renderer.drawLine(px, py, nx, ny, thickness, true);
          px = nx;
          py = ny;
        }
        break;
      }
      case 0:  // dashed
      default:
        for (int x = x1; x < x2; x += dash + gap) {
          const int seg = std::min(dash, x2 - x);
          renderer.fillRect(x, y, seg, thickness, true);
        }
        break;
    }
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  const auto tScan = millis();
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  drawRuledLines(*page, fontId, orientedMarginTop, orientedMarginLeft, orientedMarginRight);
  renderStatusBar();

  // Dictionary picker overlay: index the page's glyphs on the first paint,
  // then box the cursor cell — plus a one-shot control hint — into the BW
  // frame before the display calls below push it. The whole frame is already
  // BW-only (no staging while picking), so positing after status bar is safe.
  if (dictPickActive_) {
    if (!dictCellTableValid_) {
      buildDictCellTable(*page, fontId, orientedMarginLeft, orientedMarginTop);
    }
    if (dictCellTableValid_) {
      const DictChar& cell = dictChars_[dictCursor_ < dictCharCount_ ? dictCursor_ : 0];
      const int lineH = renderer.getLineHeight(fontId);
      renderer.drawRect(cell.x - 1, dictLines_[cell.line].y + 1, cell.adv + 2, lineH - 2, 2, true);
      if (dictShowHelp_) {
        const int uiFont = UI_10_FONT_ID;
        const int uiLineH = renderer.getLineHeight(uiFont);
        const int maxTextW = renderer.getScreenWidth() - 24;
        const std::vector<std::string> helpLines = renderer.wrappedText(uiFont, tr(STR_DICT_PICK_HINT), maxTextW, 3);
        int textW = 0;
        for (const std::string& line : helpLines) {
          textW = std::max(textW, renderer.getTextWidth(uiFont, line.c_str()));
        }
        const int boxW = std::min(renderer.getScreenWidth() - 8, textW + 16);
        const int boxH = uiLineH * static_cast<int>(helpLines.size()) + 10;
        const int bx = std::max(4, (renderer.getScreenWidth() - boxW) / 2);
        const int by = renderer.getScreenHeight() - UITheme::getStatusBarHeight() - boxH - 6;
        renderer.fillRect(bx, by, boxW, boxH, true);
        int ty = by + 5;
        for (const std::string& line : helpLines) {
          const int lineW = renderer.getTextWidth(uiFont, line.c_str());
          renderer.drawText(uiFont, bx + std::max(8, (boxW - lineW) / 2), ty, line.c_str(), false);
          ty += uiLineH;
        }
      }
    }
  }
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above. But the grayscale pass leaves
    // gray charge in the image region that a plain fast diff on the *next*
    // page can't clear, so text there ghosts gray (#2190). Park the shared
    // cleanup counter so the next ordinary page runs the HALF ghost-cleanup
    // path, which drives every pixel to its target regardless of residue.
    renderer.requestGhostCleanup();
  } else {
    ReaderUtils::displayReaderPage(renderer);
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (needsAnyGrayscale && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tCleanup = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums scan=%lums fontpw=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tScan - t0, tPrewarm - tScan, tBwRender - tPrewarm, tDisplay - tBwRender,
              tGrayLsb - tDisplay, tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (needsAnyGrayscale) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        const auto tEnd = millis();
        LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No text AA and no images: BW frame already displayed above, no grayscale
      // to render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (dictPickActive_) {
    title = tr(STR_DICTIONARY);

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    clearPendingSpineCrossing();  // absolute jump supersedes a queued crossing
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    clearPendingSpineCrossing();  // absolute jump supersedes a queued crossing
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  const std::string bmPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  if (Storage.exists(bmPath.c_str())) {
    String json = Storage.readFile(bmPath.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str());
    }
  }
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->pageCount;
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  // Nothing reads the file until the reader reloads it itself (bookmarks list
  // open / return) or the activity exits, so the SD write does not need to sit
  // on the toggle's critical path: mark dirty and let flushBookmarks() write
  // once the render task is quiet. Batching also collapses rapid toggles into
  // one write. deep sleep runs onExit(), which flushes, so the only exposure is
  // a crash within the debounce window.
  bookmarksDirty = true;
  bookmarksDirtyAt = millis();
  requestUpdate();
}

// Writes the in-RAM bookmark cache if a toggle made it dirty. Called when the
// render task is idle (loop), before the bookmarks list opens (it reads the
// file, not the cache), and from onExit() (deep sleep included).
void EpubReaderActivity::flushBookmarks() {
  if (!bookmarksDirty || !epub) {
    return;
  }
  bookmarksDirty = false;  // before the write: a failed save is retried by the
                           // next toggle, not every idle frame the loop sees
  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  if (!JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str())) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
  }
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const ProgressRange pageRange =
      getPageProgressRange(epub, currentSpineIndex, section->currentPage, section->pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, section->pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
