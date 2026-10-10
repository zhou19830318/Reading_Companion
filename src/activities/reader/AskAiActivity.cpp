#include "AskAiActivity.h"

// clang-format off
// HalStorage.h pulls SdFat macros that collide with lwip's ip4_addr.h unless
// seen first (same ordering constraint as VoiceActivity.cpp).
#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
// clang-format on
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <OpenClawHandshake.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/voice/VoiceActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

using Notes::NoteStore;

namespace {
// Triangle-wave rule under a picked line: 3 px half-periods drawn as 1 px
// Bresenham diagonals, so a 440 px line costs ~147 drawLine calls instead of
// one per pixel. Amplitude 2 px reads as a wave without crowding the next
// line, and the full-line width comes from the page's own margins — no copy of
// the line's text is needed to know where it ends.
void drawSelectionRule(GfxRenderer& renderer, const int x, const int y, const int width) {
  constexpr int half = 3;  // period 6 px
  constexpr int amp = 2;
  if (width <= 0) return;
  for (int px = 0; px < width; px += half) {
    const int end = std::min(px + half, width);
    const bool down = ((px / half) % 2) == 0;
    renderer.drawLine(x + px, y + (down ? 0 : amp), x + end - 1, y + (down ? amp : 0), true);
  }
}
}  // namespace

void AskAiActivity::setBookContext(const std::shared_ptr<Epub>& book, const char* bookBase, uint16_t spineIndex,
                                   uint16_t pageNumber, uint16_t pageCount) {
  book_ = book;
  bookBase_[0] = '\0';
  if (bookBase != nullptr) {
    snprintf(bookBase_, sizeof(bookBase_), "%s", bookBase);
  }
  spineIndex_ = spineIndex;
  pageNumber_ = pageNumber;
  pageCount_ = pageCount;
}

void AskAiActivity::onEnter() {
  Activity::onEnter();
  // No radio and no big buffers on entry: the picker works offline, the link
  // is dialled only when a question is actually sent (ask-at-send, like the
  // note-mode fix in VoiceActivity), and prompt_/record_/lineBuf_ are allocated
  // lazily — always after the TLS dial has come back up. First paint comes
  // straight from the armed context.
  requestUpdate();
}

void AskAiActivity::onExit() {
  // The session is process-wide: drop the notifier — it points at this object —
  // and never close the socket. Same rule as VoiceActivity::onExit.
  session_.setNotifier(nullptr, nullptr);
  exitSelectionView();
  if (linePool_ != nullptr) {
    heap_caps_free(linePool_);
    linePool_ = nullptr;
  }
  linePoolCap_ = 0;
  lineCount_ = 0;
  Activity::onExit();
}

bool AskAiActivity::preventAutoSleep() { return state_ == State::Connecting || state_ == State::Asking; }

void AskAiActivity::onSessionNotifyTrampoline(void* ctx) { static_cast<AskAiActivity*>(ctx)->onSessionNotify(); }

void AskAiActivity::onSessionNotify() {
  // Session callbacks fire from inside the WebSocket dispatch: repaint only,
  // and only for the states that show session traffic.
  if (state_ == State::Connecting || state_ == State::Asking) {
    requestUpdate();
  }
}

void AskAiActivity::beginAsk(const char* question, Scope scope) {
  if (question == nullptr || question[0] == '\0') return;
  snprintf(question_, sizeof(question_), "%s", question);
  scope_ = scope;
  saved_ = false;
  answerScroll_ = 0;
  failReason_ = nullptr;
  // Drop everything the selection view held before any radio work: the page
  // and its glyph prewarm are the only internal-heap residents here, and the
  // class rule is that neither may meet the TLS handshake's dip.
  exitSelectionView();

  if (scope_ == Scope::Range && lineCount_ == 0) {
    // Retry path after a pick-time extraction failure: the normal flow
    // extracts before the selection screen, so reaching here with an empty
    // pool means the lines still have to be fetched (then picked again).
    if (!extractPageLines()) {
      fail(tr(STR_ASK_NO_TEXT));
      return;
    }
    enterSelecting();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Ask-at-send, not ask-at-entry: the preset list and the keyboard stay
    // usable offline, and cancelling the network list lands back on the picker
    // instead of finishing the activity — the shape of the note-mode fix in
    // VoiceActivity::startRecording.
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               state_ = State::Picking;
                               requestUpdate();
                               return;
                             }
                             ensureConnected();
                           });
    return;
  }
  ensureConnected();
}

bool AskAiActivity::extractPageLines() {
  if (!book_) {
    LOG_ERR("ASK", "no book context for selection");
    return false;
  }
  // PSRAM first: the pool lives from pick time through the TLS dial, so it
  // must not be internal DRAM (connect dips the internal heap to ~2.5 KB).
  if (linePool_ == nullptr) {
    linePool_ = static_cast<char*>(heap_caps_malloc(LINE_POOL_SIZE, MALLOC_CAP_SPIRAM));
    if (linePool_ == nullptr) {
      linePool_ = static_cast<char*>(heap_caps_malloc(LINE_POOL_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    linePoolCap_ = linePool_ != nullptr ? LINE_POOL_SIZE : 0;
    if (linePool_ == nullptr) {
      LOG_ERR("ASK", "OOM: line pool %u bytes", static_cast<unsigned>(LINE_POOL_SIZE));
      return false;
    }
  }

  const uint32_t heapBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t used = 0;
  int count = 0;
  bool full = false;
  {
    auto section = makeUniqueNoThrow<Section>(book_, spineIndex_, renderer);
    if (!section) {
      LOG_ERR("ASK", "OOM: Section");
      return false;
    }
    section->currentPage = static_cast<int>(pageNumber_);
    page_ = section->loadPageFromSectionFile();
    if (!page_) {
      LOG_ERR("ASK", "no cached page %u/%u", spineIndex_, pageNumber_);
      return false;
    }

    // Copy each PageLine's words into the pool as one line, word-separated.
    // A word that does not fit ends the line early (never a codepoint: words
    // are whole UTF-8 sequences), and a full pool or MAX_PAGE_LINES just stops
    // the walk — a truncated tail still yields a usable selection.
    for (const auto& el : page_->elements) {
      if (count >= MAX_PAGE_LINES || used >= linePoolCap_) {
        full = true;
        break;
      }
      if (el->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*el);
      const size_t start = used;
      if (line.getBlock() != nullptr) {
        for (const auto& word : line.getBlock()->getWords()) {
          const size_t sep = used > start ? 1 : 0;
          if (used + sep + word.size() > linePoolCap_) {
            full = true;
            break;
          }
          if (sep != 0) linePool_[used++] = ' ';
          memcpy(linePool_ + used, word.data(), word.size());
          used += word.size();
        }
      }
      const size_t len = used - start;
      if (len == 0) {
        // A blank layout line has no entry to select — but a line whose FIRST
        // word did not fit the pool is not blank, it is the truncation point.
        // Stopping there keeps lineRefs_ a gapless prefix of the page's
        // non-blank lines, which is the invariant the selection view's element
        // walk relies on to index it 1:1.
        if (full) break;
        continue;
      }
      lineRefs_[count].off = static_cast<uint16_t>(start);
      lineRefs_[count].len = static_cast<uint16_t>(len);
      count++;
      if (full) break;
    }
  }  // Section released here: the retained delta below is the page alone
  lineCount_ = count;
  LOG_INF("ASK", "page lines: %d (%u bytes%s), selection page costs %d bytes internal", count,
          static_cast<unsigned>(used), full ? ", pool full" : "",
          static_cast<int>(heapBefore - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  return count > 0;
}

void AskAiActivity::enterSelecting() {
  selectCursor_ = 0;
  selectAnchor_ = -1;
  scrollY_ = 0;
  state_ = State::Selecting;
  requestUpdate();
}

void AskAiActivity::exitSelectionView() {
  // Order matters only for the glyph cache: the scope's destructor clears it,
  // which is the reader's own convention at the end of a page render.
  prewarmScope_.reset();
  prewarmed_ = false;
  page_.reset();
  scrollY_ = 0;
}

void AskAiActivity::ensureConnected() {
  state_ = State::Connecting;
  lastWaitTickMs_ = millis();
  requestUpdate();

  const auto sessionState = session_.state();
  if (sessionState == OpenClaw::Session::State::Connected || sessionState == OpenClaw::Session::State::Connecting ||
      sessionState == OpenClaw::Session::State::Handshake || sessionState == OpenClaw::Session::State::Challenge) {
    // A link is already up or coming up (voice or the workbench dialled it):
    // join it instead of re-dialling — startConnect() has no connected guard
    // and would replace the socket in flight (OpenClawSession.cpp:165).
    session_.setNotifier(this, &AskAiActivity::onSessionNotifyTrampoline);
    return;
  }
  if (!session_.loadConfig()) {
    fail(session_.reason() != nullptr ? session_.reason() : tr(STR_OPENCLAW_NO_CONFIG));
    return;
  }
  session_.setNotifier(this, &AskAiActivity::onSessionNotifyTrampoline);
  if (!session_.startConnect()) {
    fail(session_.reason() != nullptr ? session_.reason() : tr(STR_OPENCLAW_DISCONNECTED));
    return;
  }
}

void AskAiActivity::composeAndSend() {
  // Context extraction runs only with the link up: a page decode's transient
  // peak must not overlap the TLS handshake's internal-heap dip (plan §3.5).
  if (!buildPrompt()) {
    fail(tr(STR_ASK_NO_TEXT));
    return;
  }
  if (session_.sendMessage(prompt_.get()) == OpenClaw::Session::SendOutcome::Failed) {
    fail(tr(STR_OPENCLAW_SEND_FAILED));
    return;
  }
  state_ = State::Asking;
  lastWaitShownSec_ = session_.awaitingMs() / 1000;
  requestUpdate();
}

bool AskAiActivity::buildPrompt() {
  if (!book_) {
    LOG_ERR("ASK", "no book context");
    return false;
  }
  // Allocated here — with the link already up — so the 1.5 KB request buffer
  // never sits on the internal heap across the TLS dial (plan §3.5).
  if (!prompt_) {
    prompt_ = makeUniqueNoThrow<char[]>(OpenClaw::CHAT_MESSAGE_CAP);
    if (!prompt_) {
      LOG_ERR("ASK", "OOM: prompt buffer");
      return false;
    }
  }
  char* const promptBuf = prompt_.get();
  // Model-facing instruction, not UI text: the translated preset question is
  // what carries the answer's language, so this stays out of the i18n tables.
  static constexpr char INSTRUCTION[] =
      "You are a reading assistant on an e-reader. Answer in the language of the question. "
      "Plain text only, no markdown, at most 120 words.\n";
  const size_t cap = OpenClaw::CHAT_MESSAGE_CAP;
  // The title goes in whole. A fixed-size copy of it (the old char title[65])
  // cut CJK metadata titles mid-codepoint, and an ill-formed byte in the
  // message makes the gateway close the socket before it can answer. A title
  // too long to fit is caught by the head >= cap test below instead of being
  // silently chopped.
  const int head =
      snprintf(promptBuf, cap, "%sBook: %s\nQuestion: %s\nText:\n", INSTRUCTION, book_->getTitle().c_str(), question_);
  if (head <= 0 || static_cast<size_t>(head) >= cap) {
    LOG_ERR("ASK", "prompt header overflow (%d)", head);
    return false;
  }
  size_t used = static_cast<size_t>(head);
  size_t budget = cap - used - 1;
  bool any = false;

  if (scope_ == Scope::Range) {
    // Join the picked lines straight from the selection pool: no SD, no
    // decode — extractPageLines() already paid that cost before the dial.
    if (linePool_ == nullptr || lineCount_ == 0 || selStart_ < 0 || selEnd_ < selStart_ || selEnd_ >= lineCount_) {
      LOG_ERR("ASK", "range selection invalid (%d..%d of %d)", selStart_, selEnd_, lineCount_);
      return false;
    }
    for (int i = selStart_; i <= selEnd_ && budget > 0; i++) {
      const LineRef ref = lineRefs_[i];
      if (ref.len == 0) continue;
      size_t take = std::min(budget, static_cast<size_t>(ref.len));
      if (take < ref.len) {
        // Never cut a UTF-8 codepoint in half: back off a continuation byte.
        while (take > 0 && (static_cast<unsigned char>(linePool_[ref.off + take]) & 0xC0) == 0x80) take--;
      }
      if (take == 0) break;
      if (any) {
        promptBuf[used++] = '\n';
        budget--;
      }
      memcpy(promptBuf + used, linePool_ + ref.off, take);
      used += take;
      budget -= take;
      promptBuf[used] = '\0';
      any = true;
    }
    LOG_DBG("ASK", "range context lines %d..%d: %u bytes left", selStart_, selEnd_, static_cast<unsigned>(budget));
    return any;
  }

  // Section reads from the book's cache file at the armed position; it is
  // built here (after the link is up) and released on return, so its page
  // decode peak is transient and never meets the handshake's.
  auto section = makeUniqueNoThrow<Section>(book_, spineIndex_, renderer);
  if (!section) {
    LOG_ERR("ASK", "OOM: Section");
    return false;
  }
  const int firstPage = static_cast<int>(pageNumber_);
  int lastPage = firstPage;
  if (scope_ == Scope::Chapter) {
    if (const auto cachedCount = section->getCachedPageCount()) {
      lastPage = std::min(firstPage + MAX_CONTEXT_PAGES - 1, static_cast<int>(*cachedCount) - 1);
    }
  }

  for (int page = firstPage; page <= lastPage && budget > 0; page++) {
    section->currentPage = page;
    const std::string text = section->getTextFromSectionFile();
    if (text.empty()) continue;  // image-only page
    size_t take = std::min(budget, text.size());
    // Never cut a UTF-8 codepoint in half: back off a continuation byte.
    while (take > 0 && (static_cast<unsigned char>(text[take]) & 0xC0) == 0x80) take--;
    if (take == 0) break;
    if (any) {
      promptBuf[used++] = '\n';
      budget--;
    }
    memcpy(promptBuf + used, text.c_str(), take);
    used += take;
    budget -= take;
    promptBuf[used] = '\0';
    any = true;
    LOG_DBG("ASK", "context page %d: %u bytes (%u left)", page, static_cast<unsigned>(take),
            static_cast<unsigned>(budget));
  }
  return any;
}

void AskAiActivity::saveRecord() {
  // Both scratch buffers are first touched here — after the answer settled,
  // long after the TLS dial — so none of this sits on the internal heap
  // across the handshake. Freed with the activity (unique_ptr members).
  if (!record_) record_ = makeUniqueNoThrow<char[]>(RECORD_CAP);
  if (!lineBuf_) lineBuf_ = makeUniqueNoThrow<char[]>(NoteStore::LINE_CAP);
  if (!record_ || !lineBuf_) {
    LOG_ERR("ASK", "record skipped: OOM (%u + %u bytes)", static_cast<unsigned>(RECORD_CAP),
            static_cast<unsigned>(NoteStore::LINE_CAP));
    saved_ = false;
    return;
  }
  const char* reply = session_.replyText();
  snprintf(record_.get(), RECORD_CAP, "%s%s%s\n%s%s%s", tr(STR_ASK_Q), tr(STR_NOTE_SEP), question_, tr(STR_ASK_A),
           tr(STR_NOTE_SEP), reply != nullptr ? reply : "");
  const int64_t now = OpenClaw::nowEpochMs();
  // One append per settled round trip — the same SD write discipline as a
  // dictated voice note. Failure only loses the record, never the answer on
  // screen, so it is logged instead of failing the state machine.
  saved_ = NoteStore::append(bookBase_, static_cast<uint64_t>(now), spineIndex_, pageNumber_, pageCount_, record_.get(),
                             lineBuf_.get(), NoteStore::LINE_CAP);
  if (saved_) {
    LOG_INF("ASK", "record saved to %s (spine %u page %u/%u)", bookBase_, spineIndex_, pageNumber_, pageCount_);
  } else {
    LOG_ERR("ASK", "note append failed (book=%s)", bookBase_);
  }
}

void AskAiActivity::fail(const char* reason) {
  LOG_ERR("ASK", "failed: %s", reason != nullptr ? reason : "?");
  failReason_ = reason != nullptr ? reason : tr(STR_OPENCLAW_DISCONNECTED);
  state_ = State::Failed;
  requestUpdate();
}

const char* AskAiActivity::phaseText() const {
  using SessionState = OpenClaw::Session::State;
  if (session_.reconnecting()) return tr(STR_OPENCLAW_DISCONNECTED);
  switch (session_.state()) {
    case SessionState::Connecting:
      return tr(STR_OPENCLAW_CONNECTING);
    case SessionState::Handshake:
      return tr(STR_OPENCLAW_HANDSHAKE);
    case SessionState::Challenge:
      return tr(STR_OPENCLAW_CHALLENGE);
    case SessionState::Connected:
      return tr(STR_OPENCLAW_CONNECTED);
    case SessionState::Failed:
      return session_.reason() != nullptr ? session_.reason() : tr(STR_OPENCLAW_DISCONNECTED);
    case SessionState::Idle:
    default:
      return tr(STR_OPENCLAW_CONNECTING);
  }
}

void AskAiActivity::loop() {
  // Pumps the shared socket in every state; on an unopened session poll() is
  // a no-op (same as VoiceActivity::loop).
  session_.poll();

  // Front Left/Right mirror the side Up/Down on every list/scroll state, so
  // the bottom row is never a dead end. The pair flips with the nav axis
  // (INVERTED / LANDSCAPE_CCW) to stay on the keys mapLabels() paints 上/下 over.
  const bool navSwapped = mappedInput.isNavDirectionSwapped();
  const auto prevKey = navSwapped ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextKey = navSwapped ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;

  // Back leaves from any state (the round trip keeps running on the
  // process-wide socket and onExit() drops the notifier that points here),
  // except in the range picker: that steps back to the question list — the
  // lines are already extracted and the user may pick another preset.
  //
  // The finish itself runs on the release edge (EpubReaderMenu's rule): the
  // reader also treats a Back release as "go home", so finishing on the press
  // edge would hand it the orphaned release and kick the user out of the book.
  // The press only arms the finish — a child that cancels on its own press
  // edge (VoiceActivity) leaves such a release behind, and with backArmed_
  // still false it is ignored.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (state_ == State::Selecting) {
      exitSelectionView();  // the page and its glyph prewarm are per-selection
      state_ = State::Picking;
      requestUpdate();
      return;
    }
    backArmed_ = true;
    return;
  }
  if (backArmed_ && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    backArmed_ = false;
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (state_ == State::Connecting) {
    // A dropped socket leaves state_ at Connected and only arms the reconnect,
    // so a send fired here writes to a closed socket and comes back as
    // "Send failed". Hold the question until the link is actually up; a
    // reconnect budget that runs out lands in Failed below and reports why.
    if (session_.state() == OpenClaw::Session::State::Connected && !session_.reconnecting()) {
      composeAndSend();
      return;
    }
    if (session_.state() == OpenClaw::Session::State::Failed) {
      fail(session_.reason());
      return;
    }
    if (millis() - lastWaitTickMs_ >= CONNECT_TICK_MS) {
      lastWaitTickMs_ = millis();
      requestUpdate();
    }
    return;
  }

  if (state_ == State::Asking) {
    if (session_.awaitingReply()) {
      // Repaint exactly when the number the screen would print changes: the
      // countdown steps 0 → 1 → 2 … and never redraws the same second.
      const uint32_t shownSec = session_.awaitingMs() / 1000;
      if (shownSec != lastWaitShownSec_) {
        lastWaitShownSec_ = shownSec;
        requestUpdate();
      }
      return;
    }
    if (session_.replySize() > 0 && session_.outcome() == OpenClaw::Session::SendOutcome::Sent) {
      answerScroll_ = 0;  // a new reply starts at its first line
      saveRecord();
      state_ = State::Answer;
      requestUpdate();
      return;
    }
    if (session_.state() == OpenClaw::Session::State::Connected) {
      // A dropped socket leaves state_ at Connected and only arms the
      // reconnect, so "the link is gone" has to be read from reconnecting()
      // and not from the state — otherwise a closed socket is reported as a
      // gateway that never answered.
      if (session_.reconnecting()) {
        fail(tr(STR_OPENCLAW_DISCONNECTED));
      } else {
        fail(session_.lastError() != nullptr ? tr(STR_OPENCLAW_AGENT_FAILED) : tr(STR_OPENCLAW_NO_REPLY));
      }
      return;
    }
    // Link dropped mid-round trip: the session's own reconnect runs inside
    // poll(); if it settles into Failed the next pass reports the reason.
    if (session_.state() == OpenClaw::Session::State::Failed) {
      fail(session_.reason());
    }
    return;
  }

  if (state_ == State::Answer) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) || mappedInput.wasPressed(prevKey)) {
      if (answerScroll_ > 0) answerScroll_--;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) || mappedInput.wasPressed(nextKey)) {
      answerScroll_++;  // clamped by drawWindow() at render time
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
               mappedInput.wasPressed(MappedInputManager::Button::Power)) {
      // The record is already written; back to the picker for another question.
      state_ = State::Picking;
      selectedIndex_ = 0;
      requestUpdate();
    }
    return;
  }

  if (state_ == State::Failed) {
    // Retry keeps the same question and scope — the network detour, if any,
    // runs again inside beginAsk().
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Power)) {
      beginAsk(question_, scope_);
    }
    return;
  }

  // ── Selecting ───────────────────────────────────────────────────
  if (state_ == State::Selecting) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) || mappedInput.wasPressed(prevKey)) {
      if (selectCursor_ > 0) selectCursor_--;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) || mappedInput.wasPressed(nextKey)) {
      if (selectCursor_ + 1 < lineCount_) selectCursor_++;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Power)) {
      if (selectAnchor_ < 0) {
        // First Confirm marks the range start; the band grows to the cursor.
        selectAnchor_ = selectCursor_;
        requestUpdate();
      } else {
        selStart_ = std::min(selectAnchor_, selectCursor_);
        selEnd_ = std::max(selectAnchor_, selectCursor_);
        beginAsk(question_, Scope::Range);
      }
    }
    return;
  }

  // ── Picking ─────────────────────────────────────────────────────
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) || mappedInput.wasPressed(prevKey)) {
    if (selectedIndex_ > 0) {
      selectedIndex_--;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) || mappedInput.wasPressed(nextKey)) {
    if (selectedIndex_ + 1 < ROW_COUNT) {
      selectedIndex_++;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.wasPressed(MappedInputManager::Button::Power)) {
    const Scope scope = kRowScope[selectedIndex_];
    if (selectedIndex_ == ROW_CUSTOM) {
      // Custom question through VoiceActivity's question-input mode: speak it
      // (one STT call) or press the left key and type it. The preset rows
      // above cost no STT call, which is the point of the presets.
      auto voice = std::make_unique<VoiceActivity>(renderer, mappedInput);
      voice->setQuestionInput();
      startActivityForResult(std::move(voice), [this](const ActivityResult& result) {
        const auto* typed = std::get_if<KeyboardResult>(&result.data);
        if (typed != nullptr && !typed->text.empty()) {
          beginAsk(typed->text.c_str(), kRowScope[ROW_CUSTOM]);
        } else {
          requestUpdate();  // cancelled: repaint the picker
        }
      });
      return;
    }
    if (scope == Scope::Range) {
      // Range presets extract this page's lines first (PSRAM pool, still no
      // radio), then open the selection screen; the dial starts only after
      // the user confirms the range.
      snprintf(question_, sizeof(question_), "%s", I18N.get(kRowLabels[selectedIndex_]));
      scope_ = Scope::Range;
      if (!extractPageLines()) {
        fail(tr(STR_ASK_NO_TEXT));
        return;
      }
      enterSelecting();
      return;
    }
    beginAsk(I18N.get(kRowLabels[selectedIndex_]), scope);
  }
}

size_t AskAiActivity::drawWindow(const int fontId, const char* text, const int x, const int y, const int width,
                                 const int capacity, size_t& firstLine) {
  // Re-wrapped per repaint, exactly like VoiceActivity::drawTextWindow: the
  // answer lives in the session's shared buffer, and orientation decides the
  // width, so caching lines would need an orientation-keyed invalidation for
  // no measurable gain at e-ink frame rates.
  const std::vector<std::string> lines = renderer.wrappedText(fontId, text, width, WRAP_MAX_LINES);
  const size_t total = lines.size();
  if (total == 0 || capacity <= 0) {
    firstLine = 0;
    return total;
  }
  const size_t maxFirst = total > static_cast<size_t>(capacity) ? total - static_cast<size_t>(capacity) : 0;
  if (firstLine > maxFirst) firstLine = maxFirst;
  const size_t end = std::min(total, firstLine + static_cast<size_t>(capacity));
  int lineY = y;
  for (size_t i = firstLine; i < end; i++, lineY += renderer.getLineHeight(fontId)) {
    renderer.drawText(fontId, x, lineY, lines[i].c_str());
  }
  return total;
}

// Draws the page into the selection window at [viewTop, viewTop+viewHeight).
// Called twice for the first paint (scan, then real) and once per repaint
// afterwards — see the prewarm block in render().
void AskAiActivity::renderSelectionView(const int viewTop, const int viewHeight) {
  const int fontId = SETTINGS.getReaderFontId();
  const int lineH = renderer.getLineHeight(fontId);
  // Same horizontal margins the reader laid this page out with: the cached
  // elements are viewport-relative, so reproducing them is exactly what puts
  // every line back where it was on the reading page.
  int bezelTop = 0, bezelRight = 0, bezelBottom = 0, bezelLeft = 0;
  renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
  const int marginL = bezelLeft + SETTINGS.screenMargin;
  const int marginR = bezelRight + SETTINGS.screenMargin;
  const int rightX = renderer.getScreenWidth() - marginR;
  const int caretX = std::max(0, marginL - 7);
  const int viewBottom = viewTop + viewHeight;

  // Keep the cursor's line whole inside the window. The window slides over the
  // page's viewport-relative Y coordinates; scrollY_ is never negative and
  // never leaves the cursor less than a full line from either edge.
  int cursorY = -1;
  {
    int idx = -1;
    for (const auto& el : page_->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const auto& probe = static_cast<const PageLine&>(*el);
      if (probe.getBlock() == nullptr || probe.getBlock()->getWords().empty()) continue;
      if (++idx == selectCursor_) {
        cursorY = probe.yPos;
        break;
      }
    }
  }
  if (cursorY >= 0) {
    if (cursorY < scrollY_) scrollY_ = cursorY;
    if (cursorY + lineH > scrollY_ + viewHeight) scrollY_ = cursorY + lineH - viewHeight;
    if (scrollY_ < 0) scrollY_ = 0;
  }

  const bool bandOn = selectAnchor_ >= 0;
  const int lo = bandOn ? std::min(selectAnchor_, selectCursor_) : 0;
  const int hi = bandOn ? std::max(selectAnchor_, selectCursor_) : -1;
  const int yOffset = viewTop - scrollY_;
  const int caretH = std::max(2, lineH - 4);
  int idx = -1;
  for (const auto& el : page_->elements) {
    const int sy = el->yPos + yOffset;
    switch (el->getTag()) {
      case TAG_PageLine: {
        const auto& line = static_cast<const PageLine&>(*el);
        // The same test extractPageLines() uses to skip a line, so this index
        // walks lineRefs_ 1:1 (the extraction loop stops rather than skipping
        // a line once the pool runs out).
        if (line.getBlock() == nullptr || line.getBlock()->getWords().empty()) break;
        ++idx;
        // There is no pixel clip path, so a line that does not fit the window
        // entirely is dropped instead of being allowed to overpaint the header
        // or the button hints.
        if (sy < viewTop || sy + lineH > viewBottom) break;
        const bool picked = bandOn && idx >= lo && idx <= hi;
        const bool isCursor = idx == selectCursor_;
        el->render(renderer, fontId, marginL, yOffset);
        if (picked) {
          drawSelectionRule(renderer, marginL + line.xPos, sy + lineH - 4, rightX - (marginL + line.xPos));
        }
        if (isCursor) {
          renderer.fillRect(caretX, sy + 2, 4, caretH, true);
        }
        break;
      }
      case TAG_PageHorizontalRule:
        if (sy >= viewTop && sy < viewBottom) el->render(renderer, fontId, marginL, yOffset);
        break;
      case TAG_PageImage:
      default:
        // Images are skipped deliberately: PageImage has no clip path (it would
        // paint outside the window), and every pass would re-decode it from SD.
        // The gap it leaves keeps the lines below it at their original
        // positions, so the layout still matches the reading page.
        break;
    }
  }
}

void AskAiActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int left = metrics.contentSidePadding;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ASK_AI));

  const int y0 = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textWidth = pageWidth - 2 * left;
  int y = y0;

  switch (state_) {
    case State::Picking: {
      const int contentHeight = pageHeight - y0 - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
      GUI.drawList(
          renderer, Rect{0, y0, pageWidth, contentHeight}, ROW_COUNT, selectedIndex_,
          [](int index) { return std::string(I18N.get(kRowLabels[index])); },
          [](int index) {
            // Range rows also read "this page": the selection comes from it.
            return std::string(I18N.get(kRowScope[index] == Scope::Chapter ? StrId::STR_ASK_SCOPE_CHAPTER
                                                                           : StrId::STR_ASK_SCOPE_PAGE));
          });
      break;
    }

    case State::Selecting: {
      // The question that got us here, then this page drawn in its reading
      // layout: the reader's own margins, line positions and per-word
      // placement — not a re-typeset copy. The band being picked shows as a
      // wavy rule under each line, and the moving end carries a caret in the
      // left gutter.
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, question_, textWidth, 2)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      y += metrics.verticalSpacing;
      const int viewTop = y;
      const int viewHeight = pageHeight - y - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
      const int minLineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
      if (page_ == nullptr || viewHeight < minLineHeight) {
        LOG_ERR("ASK", "selection view unusable (page=%d, view h=%d, line h=%d)", page_ != nullptr, viewHeight,
                minLineHeight);
        break;
      }
      if (!prewarmed_) {
        // The reader's two-pass pattern, run ONCE per selection instead of per
        // repaint: the scope clears the glyph cache on construction and again
        // on destruction, so a fresh scope every key press would re-prewarm the
        // whole page each time. Held, the first pass records the glyphs, the
        // prewarm rasterizes them, and every later pass is a cache hit.
        prewarmScope_.emplace(renderer.getFontCacheManager()->createPrewarmScope());
        renderSelectionView(viewTop, viewHeight);  // scan pass: records, paints nothing
        prewarmScope_->endScanAndPrewarm();
        prewarmed_ = true;
      }
      renderSelectionView(viewTop, viewHeight);
      break;
    }

    case State::Connecting: {
      const char* phase = phaseText();
      renderer.drawText(UI_10_FONT_ID, left, y, phase, true, EpdFontFamily::BOLD);
      y += lineHeight + metrics.verticalSpacing;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, question_, textWidth, 3)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      break;
    }

    case State::Asking: {
      // Elapsed / deadline read-out — the one thing that changes between the
      // per-second repaints, so a slow agent turn reads as progress.
      char waiting[72];
      if (session_.awaitingReply()) {
        snprintf(waiting, sizeof(waiting), "%s %u/%us", tr(STR_OPENCLAW_WAITING),
                 static_cast<unsigned>(session_.awaitingMs() / 1000),
                 static_cast<unsigned>(OpenClaw::Session::CHAT_TIMEOUT_MS / 1000));
      } else {
        snprintf(waiting, sizeof(waiting), "%s", tr(STR_OPENCLAW_WAITING));
      }
      renderer.drawText(UI_10_FONT_ID, left, y, waiting, true, EpdFontFamily::BOLD);
      y += lineHeight + metrics.verticalSpacing;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, question_, textWidth, 3)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      break;
    }

    case State::Answer: {
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_ASK_Q), true, EpdFontFamily::BOLD);
      if (saved_) {
        // Saved badge rides the question row; the scroll read-out below owns
        // the right end of the answer label row, so the two never overlap.
        const int badgeW = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_ASK_SAVED));
        renderer.drawText(UI_10_FONT_ID, pageWidth - left - badgeW, y, tr(STR_ASK_SAVED));
      }
      y += lineHeight;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, question_, textWidth, 2)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      y += metrics.verticalSpacing;
      const int labelY = y;
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_ASK_A), true, EpdFontFamily::BOLD);
      y += lineHeight;
      const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
      int maxLines = lineHeight > 0 ? (bottom - y) / lineHeight : 1;
      if (maxLines < 1) maxLines = 1;
      // Body text stays clear of the right margin, where the side Up/Down
      // scroll hints are drawn (same split as VoiceActivity's answer window).
      const int bodyWidth = textWidth - metrics.sideButtonHintsWidth;
      const size_t total = drawWindow(UI_10_FONT_ID, session_.replyText(), left, y, bodyWidth, maxLines, answerScroll_);
      if (total > static_cast<size_t>(maxLines)) {
        const size_t lastShown = std::min(total, answerScroll_ + static_cast<size_t>(maxLines));
        char pos[24];
        snprintf(pos, sizeof(pos), "%u-%u/%u", static_cast<unsigned>(answerScroll_ + 1),
                 static_cast<unsigned>(lastShown), static_cast<unsigned>(total));
        renderer.drawText(UI_10_FONT_ID, pageWidth - left - renderer.getTextWidth(UI_10_FONT_ID, pos), labelY, pos);
      }
      break;
    }

    case State::Failed: {
      renderer.drawText(UI_10_FONT_ID, left, y, failReason_, true, EpdFontFamily::BOLD);
      y += lineHeight + metrics.verticalSpacing;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, question_, textWidth, 3)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      break;
    }
  }

  // Side Up/Down move the picker highlight and scroll the answer; the other
  // states never bind them, so their slots stay empty. The front Left/Right
  // keys mirror those two, so they carry the same labels on the same states.
  const bool navStates = state_ == State::Picking || state_ == State::Selecting || state_ == State::Answer;
  if (navStates && state_ != State::Selecting) {
    // Selecting keeps its nav labels (below) but drops the side chips: the
    // selection view spans the reading width, so the chips would sit on top of
    // the page text.
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
  const char* confirmLabel = "";
  switch (state_) {
    case State::Picking:
      confirmLabel = tr(STR_SELECT);
      break;
    case State::Selecting:
      // First press marks the start, the second both closes the range and
      // sends it — the labels say which press is due.
      confirmLabel = selectAnchor_ < 0 ? tr(STR_ASK_RANGE_START) : tr(STR_ASK_RANGE_CONFIRM);
      break;
    case State::Answer:
      confirmLabel = tr(STR_ASK_AGAIN);
      break;
    case State::Failed:
      confirmLabel = tr(STR_RETRY);
      break;
    default:
      break;
  }
  const char* prevHint = navStates ? tr(STR_DIR_UP) : "";
  const char* nextHint = navStates ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, prevHint, nextHint);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
