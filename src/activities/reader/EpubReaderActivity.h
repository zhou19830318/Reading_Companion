#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <atomic>
#include <optional>

#include "BookmarkEntry.h"
#include "EpubReaderMenuActivity.h"
#include "NextChapterIndexer.h"
#include "NoteStore.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  // Builds the upcoming chapter's section file on a background task while the
  // reader keeps turning pages; joined/cancelled before any other indexing
  // (single-flight) and on onPause/onExit.
  NextChapterIndexer nextChapterIndexer_;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  // Cross-chapter turns queue the spine move here instead of taking
  // RenderLock from the input path: that lock blocks the main loop for the
  // whole on-demand section build (measured 9-15.7 s freezes). The render
  // task exchanges the delta at render() entry, where it already owns the
  // lock. Written by the main task only; read/cleared by the render task.
  std::atomic<int> pendingSpineDelta{0};
  // Direction landing for a pending backward delta: true = previous chapter's
  // last page (normal page-turn), false = page 0 (chapter skip). Paired with
  // the delta on apply; both are main-task writes.
  std::atomic<bool> pendingBackLandsOnLastPage{false};
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Deferred bookmark-file write (see addBookmark()/flushBookmarks()): the RAM
  // cache is authoritative until the file is read again (bookmarks list open),
  // so the SD write is batched to a quiet moment instead of blocking the
  // toggle. Cleared by flushBookmarks().
  bool bookmarksDirty = false;
  unsigned long bookmarksDirtyAt = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void drawRuledLines(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft,
                      int orientedMarginRight) const;
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void prefetchPageGlyphs(int spineIndex, int pageIndex, uint16_t viewportWidth, uint16_t viewportHeight);
  int prefetchedSpine_ = -1;
  int prefetchedPage_ = -1;
  int prefetchedFont_ = -1;

  // Whole-page staging: the next page is laid out and rendered into PSRAM while
  // the current page sits idle on the panel, so the following page turn only
  // copies the frame and pushes it instead of re-running layout + 3 renders.
  // Identity of a staged frame. Everything that can change the pixels the page
  // would produce has to be in here, otherwise a stale buffer is displayed.
  // (spine/pageCount catch layout-affecting settings that rebuild the section
  // cache; vw/vh catch orientation and margin changes; textAA and the ruled-line
  // params change rendering without changing pageCount.)
  struct StageKey {
    int spine = -1;
    int page = -1;
    int fontId = -1;
    uint16_t vw = 0;
    uint16_t vh = 0;
    uint16_t pageCount = 0;
    float lineCompression = 0.0f;
    uint8_t lineSpacing = 0;
    uint8_t alignment = 0;
    uint8_t extraPara = 0;
    uint8_t hyphenation = 0;
    uint8_t embeddedStyle = 0;
    uint8_t focusReading = 0;
    uint8_t imageRendering = 0;
    uint8_t textAA = 0;
    uint8_t ruledLines = 0;
    uint8_t ruledStyle = 0;
    uint8_t ruledThick = 0;
    uint8_t ruledDash = 0;
    uint8_t ruledOffset = 0;
    uint8_t orientation = 0;
    bool operator==(const StageKey& other) const = default;
  };

  StageKey makeStageKey(int spine, int page, uint16_t vw, uint16_t vh, uint16_t pageCount) const;
  // Renders (spine,page) into the stage buffers. Returns true when a valid
  // frame for that exact key is available afterwards (already staged counts).
  bool stageNextPage(int spine, int page, uint16_t vw, uint16_t vh, int orientedMarginTop, int orientedMarginRight,
                     int orientedMarginLeft);
  // Push path for a staged frame: mirrors renderContents() but sources the
  // pixels from the stage buffers instead of re-rendering them.
  void displayStagedContents();

  StageKey stagedKey_;
  bool stagedValid_ = false;
  std::unique_ptr<uint8_t[]> stagedBw_;
  std::unique_ptr<uint8_t[]> stagedLsb_;
  std::unique_ptr<uint8_t[]> stagedMsb_;
  std::vector<FootnoteEntry> stagedFootnotes_;

  // Identity of the full page render currently on the panel, and whether a
  // bookmark toast has since been painted over it. Only the full page paths in
  // render() re-assert this; every other render (and pause) invalidates it. A
  // long-press bookmark toggle on an idle page then only has to draw and
  // window-push the popup plus the status strip instead of reloading the page
  // from SD, re-rendering it and redoing its grayscale pass (~2.4 s).
  StageKey panelPageKey_;
  bool panelPageKeyValid_ = false;
  bool toastOnPanel_ = false;
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  void applyOrientation(uint8_t orientation);

  // Snapshot of every input that feeds Section::loadSectionFile(). Taken before
  // the embedded settings panel is pushed and compared when it returns: any
  // difference means the loaded section's pagination is stale and the reader
  // must drop it (and re-project the viewport) before the next render.
  struct LayoutSignature {
    int fontId = 0;
    float lineCompression = 0;
    uint8_t paragraphAlignment = 0;
    uint8_t screenMargin = 0;
    uint8_t orientation = 0;
    uint8_t statusBarHeight = 0;
    uint8_t imageRendering = 0;
    bool extraParagraphSpacing = false;
    bool hyphenationEnabled = false;
    bool embeddedStyle = false;
    bool focusReadingEnabled = false;
    bool operator==(const LayoutSignature& other) const = default;
  };
  LayoutSignature makeLayoutSignature() const;
  void applyLayoutChanges(const LayoutSignature& before);

  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  // Applies pendingSpineDelta (spine move + section reload) at render() entry,
  // the only call site, which already owns the render lock.
  void applyPendingSpineDelta();
  // Drops a queued crossing: absolute spine writes (TOC jump, footnote href,
  // sync restore, cache delete) supersede anything pageTurn() had queued.
  void clearPendingSpineCrossing();
  void loadCachedBookmarks();
  void addBookmark();
  void flushBookmarks();
  void updateBookmarkFlag();
  uint16_t progressSeq_ = 0;  // dual-slot write counter (FR-01), seeded in onEnter()

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void onPause() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;

  // Arms the voice-note mode for the next VoiceActivity launch (M2/AI-02):
  // captures the note file base name and the current spine/page position now,
  // while the reader is alive; VoiceActivity reads it in its onEnter() and
  // clears it (note arming is one-shot, never persisted).
  void armNoteMode() {
    Notes::NoteFormat::baseNameForBook(epub ? epub->getPath().c_str() : "", noteBookBase_, sizeof(noteBookBase_));
    noteSpine_ = currentSpineIndex;
    notePage_ = section ? section->currentPage : 0;
    notePageCount_ = section ? section->pageCount : 0;
    noteArmed_ = true;
  }

 private:
  // Launches VoiceActivity in note mode from the menu entry or the short
  // power-press shortcut (SETTINGS.shortPwrBtn == SHORT_PWRBTN::VOICE_NOTE).
  void launchVoiceNote();
  // Short power press: quick-action chooser between the AI question screen
  // and the voice bookmark (PowerMenuActivity).
  void launchPowerMenu();
  // AI-01 (阅读中快捷键 → 上下文提问): arm the reading position and open the
  // question picker.
  void launchAskAi();
  // Quick marks ("快捷书签") attach the page they were made on, and the section
  // is only reachable from here, so the page text is snapshotted next to the
  // position it points back to.
  void captureNoteExcerpt();

  // ── voice-note mode arming (M2/AI-02) ──────────────────────────────
  bool noteArmed_ = false;
  char noteBookBase_[Notes::NoteFormat::NAME_PATH_SIZE] = {};
  uint16_t noteSpine_ = 0;
  uint16_t notePage_ = 0;
  uint16_t notePageCount_ = 0;
  char noteExcerpt_[Notes::NoteFormat::EXCERPT_CAP] = {};

  // ── dictionary character picker (short-power quick action) ─────────
  // A page-local glyph index built from the rendered Page the first time the
  // picker paints: side up/down walk lines, the front pair walks characters,
  // Confirm sends the codepoint to DictionaryActivity. Cell coordinates mirror
  // GfxRenderer::drawText's differential-rounding pen exactly (see
  // buildDictCellTable), so the box tracks glyphs at any font size. The table
  // lives in PSRAM (with an internal-heap fallback) and is freed when the
  // picker closes.
  struct DictChar {
    uint32_t cp = 0;
    int16_t x = 0;    // cell left edge, framebuffer coords (margins applied)
    int16_t adv = 0;  // cell advance width, >= 1 px
    uint8_t line = 0;
    // Index into DictLine's block words: lets a lookup resolve the cursor to
    // the full whitespace token without re-running ligature logic (fallback 0
    // is harmless — the cell still maps to the line's first word).
    uint16_t wordIdx = 0;
  };
  struct DictLine {
    int16_t y = 0;  // line top, framebuffer coords
    uint16_t first = 0;
    uint16_t count = 0;
    uint16_t elementIdx = 0;  // index into Page::elements for this line
  };
  bool dictPickActive_ = false;
  bool dictShowHelp_ = false;        // one hint overlay until the first move
  bool dictCellTableValid_ = false;  // table matches the currently rendered page
  DictChar* dictChars_ = nullptr;
  DictLine* dictLines_ = nullptr;
  size_t dictCharsCap_ = 0;
  size_t dictLinesCap_ = 0;
  size_t dictCharCount_ = 0;
  size_t dictLineCount_ = 0;
  size_t dictCursor_ = 0;
  unsigned long dictEnterMs_ = 0;  // guards against the menu's orphaned Confirm release
  // Entry from the quick-action menu sets ignoreNextConfirmRelease for the
  // orphaned Confirm edge; moveDictCursor clears it in case none arrives.
  void enterDictPick();
  void exitDictPick();
  void freeDictCellTable();
  void buildDictCellTable(const Page& page, int fontId, int xOffset, int yOffset);
  // Resolves the dict cursor to an English word for the word card: the
  // whitespace token holding the cursor glyph (identity recorded at table
  // build, so ligatures cannot shift it), trimmed to its longest ASCII
  // letter run. False for CJK tokens and single letters.
  bool extractWordAtCursor(const Page& page, std::string& out) const;
  void moveDictCursor(int dChar, int dLine);
  void launchDictionaryLookup();
};
