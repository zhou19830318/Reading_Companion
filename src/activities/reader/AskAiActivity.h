#pragma once

#include <Epub.h>
#include <Epub/Page.h>
#include <FontCacheManager.h>
#include <I18n.h>
#include <NoteStore.h>
#include <VoiceRelayChatClient.h>
#include <VoiceRelayCodec.h>

#include <memory>
#include <optional>

#include "activities/Activity.h"

// AI-01 (阅读中快捷键 → 上下文提问): pick a reading question from a preset
// list, speak or type one (VoiceActivity's question-input mode), or select a
// line range on the current page, send it to the relay's text endpoint with
// the current page / following pages as context, and keep the round trip as a
// bookmark entry (NoteStore) so it can be re-read from the reader's note list.
// The relay is the only backend: an unconfigured relay or a failed round is a
// hard failure with its reason on screen, never a fallback to another model.
//
// Why this is its own lean activity instead of VoiceActivity:
//  - no mic, no STT, no 32 KB history pool: the big buffers (prompt, record,
//    note line) are unique_ptrs allocated LAZILY — the prompt once the link is
//    up, the record and line scratch after the answer — so nothing large sits
//    on the internal heap across the TLS handshake's dip (plan §3.5);
//  - no work on entry: the radio and the session are untouched until a
//    question is picked, so the picker and the selection screen stay usable
//    offline and the Wi-Fi detour happens at send time (the same rule as the
//    note-mode fix in VoiceActivity::startRecording);
//  - context text is extracted just before the blocking relay call (Page /
//    Chapter), or is copied up front into a PSRAM-forced line pool (Range), so
//    the page decode never overlaps the TLS dip with internal-heap allocations.
class AskAiActivity final : public Activity {
 public:
  AskAiActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("AskAi", renderer, mappedInput) {}

  // Position context armed by the reader while it is alive (one-shot, same
  // shape as VoiceActivity::setNoteContext). The shared_ptr copy keeps the
  // book reachable for the context extraction in buildPrompt().
  void setBookContext(const std::shared_ptr<Epub>& book, const char* bookBase, uint16_t spineIndex, uint16_t pageNumber,
                      uint16_t pageCount);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State : uint8_t {
    Picking,    // preset / voice question list
    Selecting,  // line-range picker for the Range presets
    Asking,     // blocking relay round in flight (painted before the call)
    Answer,     // settled reply, scrollable window
    Failed,     // round trip died; Confirm retries the same question
  };

  // Context scope of a question: the current page only, a forward window from
  // it (chapter), or an explicit line range picked on this page.
  enum class Scope : uint8_t { Page, Chapter, Range };

  static constexpr int PRESET_COUNT = 8;
  static constexpr int ROW_COUNT = PRESET_COUNT + 1;  // + voice/keyboard row
  static constexpr int ROW_CUSTOM = PRESET_COUNT;
  // Chapter context reads forward from the current page, at most this many
  // pages: the relay prompt cap (1536 bytes) holds well under one page of CJK
  // text anyway, and a bounded loop keeps the SD/page decode off the watchdog.
  static constexpr int MAX_CONTEXT_PAGES = 3;
  static constexpr size_t QUESTION_CAP = 256;
  // Question + labels + reply always fit, so the record is never cut mid
  // character before NoteStore::buildLine sees it.
  static constexpr size_t RECORD_CAP = VoiceRelay::CHAT_TEXT_CAP + QUESTION_CAP + 64;
  static constexpr int WRAP_MAX_LINES = 512;
  // Range selection: the page's lines are copied once into a PSRAM-forced
  // pool (held from pick time through the TLS dial — never internal DRAM),
  // addressed by uint16 offset/length pairs so the member array stays small.
  // 12 KB ≈ one dense CJK page; extraction stops cleanly if it fills.
  static constexpr size_t LINE_POOL_SIZE = 12288;
  static constexpr int MAX_PAGE_LINES = 160;
  struct LineRef {
    uint16_t off;
    uint16_t len;
  };

  // Preset table: row label and its context scope. flash-resident constexpr.
  static constexpr StrId kRowLabels[ROW_COUNT] = {
      StrId::STR_Q_SUMMARIZE_PAGE,    StrId::STR_Q_KEY_POINTS,          StrId::STR_Q_EXPLAIN,
      StrId::STR_Q_CONNECT_LIFE,      StrId::STR_Q_CHAPTER_SUMMARY,     StrId::STR_Q_CHAPTER_BACKGROUND,
      StrId::STR_Q_EXPLAIN_SELECTION, StrId::STR_Q_TRANSLATE_SELECTION, StrId::STR_Q_CUSTOM};
  static constexpr Scope kRowScope[ROW_COUNT] = {Scope::Page,  Scope::Page,    Scope::Page,
                                                 Scope::Page,  Scope::Chapter, Scope::Chapter,
                                                 Scope::Range, Scope::Range,   Scope::Chapter};

  void beginAsk(const char* question, Scope scope);
  bool extractPageLines();
  void enterSelecting();
  void exitSelectionView();
  void renderSelectionView(int viewTop, int viewHeight);
  // The relay setting is complete (URL + token + device id). Checked per ask,
  // so flipping the setting applies to the next question.
  bool relayConfigured();
  // Blocking POST /api/v1/chat on the configured relay origin, one question
  // per call. Paints the waiting frame first: no repaint can run mid-call.
  void beginRelayAsk();
  const char* answerText() const;
  bool buildPrompt();
  void saveRecord();
  void fail(const char* reason);
  size_t drawWindow(int fontId, const char* text, int x, int y, int width, int capacity, size_t& firstLine);
  // Armed by the reader (see setBookContext).
  std::shared_ptr<Epub> book_;
  char bookBase_[Notes::NoteFormat::NAME_PATH_SIZE] = {};
  uint16_t spineIndex_ = 0;
  uint16_t pageNumber_ = 0;
  uint16_t pageCount_ = 0;

  State state_ = State::Picking;
  int selectedIndex_ = 0;
  Scope scope_ = Scope::Page;
  char question_[QUESTION_CAP] = {};
  size_t answerScroll_ = 0;
  bool saved_ = false;
  const char* failReason_ = nullptr;
  // Back press seen while current: the finish waits for its release edge so
  // the reader never consumes an orphaned Back release (see loop()).
  bool backArmed_ = false;
  // Big buffers, allocated on first use (buildPrompt / saveRecord) — both
  // happen only after the link is up, so none of them is resident on the
  // internal heap across the TLS dial. unique_ptr members: no onExit
  // bookkeeping, freed with the activity.
  std::unique_ptr<char[]> prompt_;
  std::unique_ptr<char[]> record_;
  std::unique_ptr<char[]> lineBuf_;
  // Relay answer scratch (CHAT_TEXT_CAP + 1), allocated on the first round
  // only. Freed with the activity like the buffers above.
  std::unique_ptr<char[]> relayReply_;
  // Range selection state (State::Selecting).
  char* linePool_ = nullptr;
  size_t linePoolCap_ = 0;
  int lineCount_ = 0;
  int selectCursor_ = 0;
  int selectAnchor_ = -1;  // -1: start not picked yet
  int selStart_ = 0;       // inclusive, valid once a range was confirmed
  int selEnd_ = 0;
  LineRef lineRefs_[MAX_PAGE_LINES] = {};
  // The page the selection view draws. Owned here from extraction until
  // exitSelectionView() — which runs on every path out of State::Selecting —
  // so it is never resident across the TLS dial (the class comment's rule) nor
  // while the preset list is up.
  std::unique_ptr<Page> page_;
  // Sliding window over the page's viewport-relative Y coordinates: the view
  // scrolls only to keep selectCursor_'s line whole inside it.
  int scrollY_ = 0;
  // Font prewarm for the selection view, held for as long as the view is up.
  // The reader's PrewarmScope clears the glyph cache in BOTH its constructor
  // and its destructor, so a scope per repaint would clear and re-prewarm the
  // whole page on every key press. Held instead: one scan + prewarm, then every
  // repaint is a page-buffer hit and nothing touches the SD font path.
  std::optional<FontCacheManager::PrewarmScope> prewarmScope_;
  bool prewarmed_ = false;
};
