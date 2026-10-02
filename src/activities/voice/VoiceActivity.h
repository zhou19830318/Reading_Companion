#pragma once

#include <ChatHistory.h>
#include <NoteStore.h>
#include <OpenClawSession.h>
#include <Recorder.h>
#include <SttClient.h>
#include <SttCodec.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "activities/Activity.h"

// Voice conversation screen — P3-S Phase 1 (docs/voice-openclaw-port-plan.md
// §8): one user-initiated round trip per press.
//
//   Confirm → record (VAD: stops 1.5 s after speech, 8 s cap)
//           → cloud STT (blocking, ~3 s)
//           → chat.send over the shared OpenClaw::Session
//           → reply drawn on the panel; Back / timeout returns to Idle.
//
// Phase 1 closeout (§8.3/§8.6): the completed round trip is appended to the
// SD card history (/.crosspoint/chat/ per-day JSONL, one write per turn), a
// manual send mode gates the chat.send behind an explicit Confirm, and
// front-Right opens the history browser (day list → day tail): side Up/Down
// pick the day and the four front keys carry one action each — back, delete,
// summarize, open. History rendering reads
// from a fixed PSRAM pool filled by OpenClaw::ChatHistory — no String, no
// whole-day file in RAM.
//
// Deliberately minimal next to §8.3's full bubble layout: this screen's job is
// to prove the STT→chat pipeline end to end with the e-ink discipline intact
// (one refresh per state: the recording screen is painted and waited for
// BEFORE MIC.begin() powers the rail, then the panel is left alone until the
// capture ends — the mic power rail shares GPIO27 with EPD_RST,
// HalGPIO.cpp:282). Bubble list, history panel and
// auto/manual modes are Phase 1 follow-ups once the round trip is proven on
// hardware, mirroring how OpenClawActivity's probe screen preceded a real UI.
class VoiceActivity final : public Activity {
 public:
  VoiceActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Voice", renderer, mappedInput) {}
  ~VoiceActivity() override;

  // Arms note mode before onEnter(): the transcript lands in the book's note
  // file instead of an OpenClaw round trip (see NoteContext). spineIndex/
  // pageNumber/pageCount mirror the progress triple (progress saves use the
  // same 0-based page); pageNumber==0 keeps the note file parseable.
  void setNoteContext(const char* bookBase, uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount) {
    noteContext_.valid = bookBase != nullptr && bookBase[0] != '\0';
    if (!noteContext_.valid) return;
    snprintf(noteContext_.bookBase, sizeof(noteContext_.bookBase), "%s", bookBase);
    noteContext_.spineIndex = spineIndex;
    noteContext_.pageNumber = pageNumber;
    noteContext_.pageCount = pageCount;
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

  // Serial-console debug hook (CMD:VOICE_TX, main.cpp): stands in for the STT
  // step so one chat round trip — and whatever the gateway answers it — can be
  // reproduced without speaking. False when this is not the current Voice
  // activity or a capture / round trip is already in flight.
  static bool injectTranscript(const char* text);

  // Dictation bridge for the 语音书签 edit keyboard (KeyboardEntryActivity):
  // the power key there is this screen's talk key — one capture through the
  // same recorder/STT stack, transcript handed back for insertion at the
  // cursor. The voice screen must not repaint while a capture is live (the
  // keyboard owns the panel); poll() blocks during the STT call exactly like
  // runTranscription() does in the normal pipeline.
  enum class DictationState : uint8_t { Off, Recording, Ready, Failed };
  bool dictationStart();
  DictationState dictationPoll();
  const char* dictationText() const { return transcript_; }
  const char* dictationError() const { return dictationError_; }

 private:
  enum class State : uint8_t {
    Idle,          // waiting for Confirm; session may still be connecting
    Listening,     // recording; zero repaints in this state
    Transcribing,  // STT upload in flight; blocking pass
    Sending,       // chat.send accepted; reply accumulating
    Answer,        // reply settled and shown
    Failed,        // fatal for this round trip; Confirm retries
    Review,        // manual mode: transcript ready, Confirm sends it
    HistoryList,   // history: flat day list, newest first (smart date labels)
    HistoryDay,    // history: one day's latest turns (Up/Down scroll)
    NoteSaved,     // note mode: transcript written to /.crosspoint/notes/
    NoteList,      // note mode: this book's notes (prev key enters)
    NoteView,      // note mode: one note, scroll window
  };

  // Recording ceiling. The VAD usually stops long before this (1.5 s of
  // silence after speech); the cap bounds the PSRAM buffer and the STT upload
  // for a capture that never goes quiet (ambient noise above the threshold).
  static constexpr uint32_t MAX_RECORD_SECONDS = 8;
  // VAD stop: 1.5 s below the speech threshold ends the capture (§8.4
  // Listening → Transcribing). Paired with Recorder::VOICED_MIN_MS so a
  // cough alone does not trigger a send.
  static constexpr uint32_t SILENCE_STOP_MS = 1500;
  // Chunk-RMS speech threshold, from the measured levels of this microphone
  // (speech ≈1600-4000, quiet room well below 1000).
  static constexpr int32_t VAD_RMS_THRESHOLD = 1200;
  // STT endpoint override and API key, read from the same SD files
  // MicrophoneTest uses. Members rather than locals: the loop task's stack is
  // small and the config pointers must outlive the blocking upload.
  static constexpr size_t STT_URL_SIZE = 192;
  static constexpr size_t STT_KEY_SIZE = 96;
  // History day tail kept in PSRAM: one 32 KB pool holds a day's last turns
  // including full assistant replies (up to CHAT_BUF_CAP each) instead of the
  // old 6 KB, which dropped every reply that did not fit. Allocated once per
  // activity entry with the capture/session buffers, freed in onExit().
  static constexpr size_t HISTORY_POOL_SIZE = 32768;
  // Degraded size when PSRAM is exhausted: the internal heap's largest block
  // once WiFi is up is ~4 KB, so this may also fail — history then reports
  // OOM instead of crashing, which is what the nullptr checks are for.
  static constexpr size_t HISTORY_POOL_SIZE_SMALL = 6144;
  // Shared JSONL line scratch for ChatHistory::append/loadDay (LINE_CAP bytes).
  // Caller-owned PSRAM because neither the 4 KB task stack nor the internal
  // heap can hold one line of an escaped reply.
  static constexpr size_t HISTORY_LINE_SIZE = OpenClaw::ChatHistory::LINE_CAP;
  // Ceiling on the wrapped-line vector built for one scroll window. A reply
  // tops out at CHAT_BUF_CAP (4 KB), which is ~150 lines of CJK at UI_10; the
  // bound only exists so a pathological string cannot grow the vector without
  // limit.
  static constexpr int WRAP_MAX_LINES = 512;

  // One repaint per WAIT_TICK_MS while the reply is outstanding. A screen that
  // never moves while the gateway thinks reads as frozen (the alarm report),
  // and e-ink cannot animate: the tick only refreshes the elapsed-seconds
  // read-out, so a full 120 s wait costs at most 8 refreshes.
  static constexpr uint32_t WAIT_TICK_MS = 15000;

  void startRecording();
  void startTranscribing();
  void runTranscription();
  void finishTranscription(SttClient::Result result);
  // Note mode tail of the pipeline: append the transcript to the book's .ntf
  // (NoteStore) instead of dialling OpenClaw. Same one-write-per-note SD
  // discipline as the history layer.
  void saveNoteFromTranscript();
  // Note mode (语音书签 shortcut): open the transcript in
  // KeyboardEntryActivity before anything is written. Confirm →
  // saveNoteFromTranscript(), Back → discard (state back to Idle, no SD
  // write). Also the serial inject path (CMD:VOICE_TX) in note mode.
  void openNoteEditor();
  void startSession();
  void sendToOpenClaw();
  // Issues the owed chat.send (oweSend_) once the session reaches Connected.
  void sendPending();
  void onSessionNotify();
  static void onSessionNotifyTrampoline(void* ctx);
  void fail(const char* reason);
  void resetRoundTrip();
  // Stage row "Record > Transcribe > Send > Reply": the stage the round trip
  // is in is drawn bold, the rest regular. Answer for the active stage when
  // there is one, nullptr for the states where no stage is running.
  void drawPipeline(int x, int y);
  // Translated gateway line for the current Session::State (null when the
  // session has not dialled yet) — the "is the OpenClaw link up" answer.
  const char* gatewayStatus() const;
  // History. The browser is ONE flat list of day files, newest first — the
  // old Year → Month → Week → Day drill-down was deeper than any mainstream
  // chat history (they all show a flat reverse-chronological list with date
  // labels; storage was always flat per-day JSONL, the levels were only a
  // view). openHistory() (re)lists the days and lands on the top row;
  // historyDayIndex_ is both the list cursor and the open day. Row labels
  // come from ChatHistoryFormat::historyDayRowLabel (今天/昨天/前天 near
  // today, "MM-DD" inside the year, full date across years). loadHistoryDay()
  // fills the PSRAM pool; the renderers only read.
  void openHistory();
  bool loadHistoryDay(size_t dayIndex);
  void recordHistoryTurn();
  // List actions (see render of State::HistoryList): deleteHistoryDay()
  // removes the highlighted day file and refreshes the listing;
  // sendHistorySummary() loads that day and issues a chat.send asking the
  // gateway to summarize it (the round trip then runs through the ordinary
  // Sending/Answer states).
  void deleteHistoryDay();
  void sendHistorySummary();
  // Wraps `text` and draws the `capacity`-line window starting at firstLine
  // (clamped in place). Returns the total wrapped line count so the caller can
  // render an "n/m" position indicator. Used by the Answer and HistoryDay
  // states, where a reply is longer than one screen and ends in a scroll
  // window rather than an ellipsis.
  size_t drawTextWindow(int fontId, const char* text, int x, int y, int width, int capacity, size_t& firstLine);

  // The live instance, for the serial inject hook above. There is at most one
  // VoiceActivity (replaceActivity drops the old one), and it clears this in
  // onExit() so a hook can never reach a destroyed activity.
  static VoiceActivity* activeInstance_;

  State state_ = State::Idle;
  const char* failReason_ = nullptr;  // translated static, never freed
  // The transcript as it stands: the STT result, then the message that was
  // sent. Kept so Answer can show the pair and a retry can re-send it.
  char transcript_[OpenClaw::CHAT_MESSAGE_CAP + 1] = {};
  bool haveTranscript_ = false;
  // True once the round trip produced an answer worth showing; False after a
  // failure so the panel does not mix an old answer with a new error.
  bool haveAnswer_ = false;

  // True from "the transcript is ready" until "chat.send is on the wire".
  // sendToOpenClaw() can land while the session is still handshaking, so the
  // round trip waits in Sending; this flag, not session_.outcome(), is what
  // says there is still a send to make. outcome() still describes the
  // PREVIOUS round there — keying the send off it made a second round issued
  // during a reconnect inherit "already sent" and never transmit.
  bool oweSend_ = false;
  // Last elapsed-seconds repaint (see WAIT_TICK_MS); reset on every send.
  uint32_t lastWaitTickMs_ = 0;

  Voice::Recorder recorder_;
  // STT state. The parser owns the transcript buffer (~6 KB PSRAM); the url
  // and key buffers back SttClient's config pointers for as long as an
  // upload can run.
  std::unique_ptr<SttCodec::SseTranscriptParser> sttParser_;
  char sttUrlBuf_[STT_URL_SIZE] = {};
  char sttKeyBuf_[STT_KEY_SIZE] = {};

  // Dictation sub-state for the edit keyboard (see dictationStart()). Kept
  // separate from state_: the voice screen's own pipeline is parked while the
  // keyboard is on top and must not be disturbed by a side capture.
  DictationState dictationState_ = DictationState::Off;
  const char* dictationError_ = nullptr;  // translated static, never freed
  static bool dictationStartTrampoline(void* ctx);
  static int dictationPollTrampoline(void* ctx);
  static const char* dictationTextTrampoline(void* ctx);
  static const char* dictationErrorTrampoline(void* ctx);

  // ── note mode (M2: AI-02 voice notes) ───────────────────────────────
  // Set by the reader before launching this activity (startActivityForResult,
  // so the reader stays alive on the stack and repaints on return). When
  // valid, the whole OpenClaw half of the pipeline is skipped: no session, no
  // handshake, no gateway frames — the transcript lands in the book's note
  // file instead. STT itself still needs the network, so the Wi-Fi bring-up
  // flow is reused.
  struct NoteContext {
    char bookBase[Notes::NoteFormat::NAME_PATH_SIZE] = {};  // "<book>.epub" -> "<book>"
    uint16_t spineIndex = 0;
    uint16_t pageNumber = 0;
    uint16_t pageCount = 0;
    bool valid = false;
  };
  NoteContext noteContext_;
  // NoteStore::append line scratch, same rationale as historyLine_ (an
  // escaped NTF1 line is at most LINE_CAP bytes; caller-owned PSRAM).
  char* noteLine_ = nullptr;
  size_t noteLineCap_ = 0;
  // Note browser (NoteList/NoteView). The text pool is historyPool_ — the
  // chat history browser is unreachable while note mode is armed, so the two
  // can never compete for it. The line scratch is historyLine_ as well
  // (LINE_CAP 6144 covers NoteStore::LINE_CAP).
  static constexpr size_t NOTE_MAX_ENTRIES = 64;
  Notes::NoteStore::Entry noteEntries_[NOTE_MAX_ENTRIES] = {};
  size_t noteEntryCount_ = 0;
  size_t noteSelected_ = 0;
  size_t noteViewScroll_ = 0;  // line window inside one note
  // Two-step delete: the list's next key only arms this (the hints and a
  // "Delete this note?" line say so), the second press rewrites the file.
  // Back disarms without touching the file.
  bool noteDeleteArmed_ = false;
  void openNoteList();
  void deleteSelectedNote();

  // Bound to the process-wide instance: the workbench status cards and this
  // activity share one gateway connection, so entering voice does not pay for
  // a second TLS session.
  OpenClaw::Session& session_ = OpenClaw::Session::instance();

  // ── history (P3-S Phase 1 closeout) ───────────────────────────────────
  // PSRAM pool + index the day tail is rendered from. Allocated in onEnter()
  // alongside the session buffers, freed in onExit(); nullptr when PSRAM is
  // exhausted (the history browser then says so instead of crashing).
  char* historyPool_ = nullptr;
  size_t historyPoolCap_ = 0;
  char* historyLine_ = nullptr;
  OpenClaw::ChatHistory::Entry historyEntries_[OpenClaw::ChatHistory::MAX_ENTRIES] = {};
  size_t historyEntryCount_ = 0;
  // Turn count of the loaded day: a turn is a user entry plus the assistant
  // entry that follows it (plus a lone assistant head when loadDays() dropped
  // an odd leading entry). The HistoryDay browser moves per turn, not per
  // entry, so one screen shows the question and its answer together.
  size_t historyTurnCount_ = 0;
  // Day names, newest first — the whole flat listing window (the browser
  // shows it as one list, no grouping levels). historyDayCount_ is the list
  // size; historyDayIndex_ is both the list cursor and the day whose turns
  // are open (they are never out of sync: opening a row keeps the cursor on
  // it, Back from the turns view just swaps the state).
  char historyDays_[OpenClaw::ChatHistory::MAX_DAYS][OpenClaw::ChatHistory::DAY_NAME_SIZE] = {};
  size_t historyDayCount_ = 0;
  size_t historyDayIndex_ = 0;
  // Highlight inside the open day's turn list; line scroll offsets for the
  // day view and the answer panel (the reply can be many screens long, so the
  // visible window slides instead of ending in an ellipsis).
  size_t historySelected_ = 0;
  size_t historyScroll_ = 0;
  size_t answerScroll_ = 0;
  // List key map (one action per front key, side Up/Down picks the day):
  // Back leaves, front-Left deletes the highlighted day, front-Right asks the
  // gateway to summarize it, Confirm opens the day's turns. Delete is the
  // note list's two-press pattern — arm, then confirm; Back or any move
  // disarms. Summarize needs no arm (its prompt is a chat message, not a
  // file operation).
  bool historyDeleteArmed_ = false;
  // A summarize round trip: pendingText_ overrides transcript_ as the message
  // sent (it holds the day prompt in historyLine_, far longer than
  // transcript_), and the settled reply is NOT appended to the day file — the
  // summary is a meta-turn about the history, not part of it. cleared when the
  // round settles or a new capture starts (resetRoundTrip).
  const char* pendingText_ = nullptr;
  bool summaryRound_ = false;
  // Back from the settled summary returns to the day list it was started from
  // instead of leaving the activity.
  bool cameFromHistory_ = false;
};
