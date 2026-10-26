#include "VoiceActivity.h"

// clang-format off
// HalStorage.h pulls SdFat macros that collide with lwip's ip4_addr.h unless
// seen first (same ordering constraint as lib/voice/SttClient.cpp).
#include <Arduino.h>
#include <CloudConfig.h>
#include <GfxRenderer.h>
#include <HalMicrophone.h>
#include <HalStorage.h>
// clang-format on
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <NoteStore.h>
#include <SttClient.h>
#include <VoiceRelayClient.h>
#include <WiFi.h>
#include <WifiIdleManager.h>

using Notes::NoteStore;
using OpenClaw::ChatHistory;
namespace ChatHistoryFormat = OpenClaw::ChatHistoryFormat;

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/icons/voice.h"
#include "fontIds.h"

VoiceActivity::VoiceActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Voice", renderer, mappedInput) {}

VoiceActivity::~VoiceActivity() = default;

VoiceActivity* VoiceActivity::activeInstance_ = nullptr;

bool VoiceActivity::injectTranscript(const char* text) {
  if (activeInstance_ == nullptr || text == nullptr || text[0] == '\0') return false;
  VoiceActivity& self = *activeInstance_;
  switch (self.state_) {
    case State::Idle:
    case State::Answer:
    case State::Failed:
    case State::Review:
      break;
    default:
      return false;  // a capture or a round trip is already in flight
  }
  self.resetRoundTrip();
  snprintf(self.transcript_, sizeof(self.transcript_), "%s", text);
  self.haveTranscript_ = true;
  LOG_INF("VOICE", "serial inject: %u bytes", static_cast<unsigned>(strlen(self.transcript_)));
  if (self.noteContext_.valid || self.questionInput_) {
    self.openNoteEditor();
    return true;
  }
  self.sendToOpenClaw();
  return true;
}

void VoiceActivity::onEnter() {
  Activity::onEnter();
  // This screen can hit the network on any Confirm press (STT + chat) and its
  // keyboard's send path waits on the gateway, so the radio is held for as
  // long as the screen is frontmost; the idle countdown starts on exit. The
  // session's own poll() notes dials and replies, so the hold only has to
  // outlast the quiet stretches (thinking, typing, picking a tag).
  WIFI_IDLE.arm();
  WIFI_IDLE.hold();
  activeInstance_ = this;
  state_ = State::Idle;
  failReason_ = nullptr;
  transcript_[0] = '\0';
  haveTranscript_ = false;
  haveAnswer_ = false;
  answerScroll_ = 0;
  sttParser_.reset();
  // noteContext_ is NOT reset here: the launcher arms it via setNoteContext()
  // BEFORE onEnter() runs, and every VoiceActivity instance is freshly
  // constructed (replaceActivity / startActivityForResult make_unique), so
  // the default (note mode off) is already correct for a plain chat entry.
  // History buffers: PSRAM first (WiFi keeps the internal largest block at
  // ~4 KB, same reasoning as Session::allocChatBuffer), internal as a last
  // resort so a PSRAM-less build still renders history. The pool holds the
  // day tail including full replies; historyLine_ is the shared JSONL line
  // scratch append()/loadDays() need. Both freed in onExit().
  if (!questionInput_ && historyPool_ == nullptr) {
    historyPool_ = static_cast<char*>(heap_caps_malloc(HISTORY_POOL_SIZE, MALLOC_CAP_SPIRAM));
    historyPoolCap_ = HISTORY_POOL_SIZE;
    if (historyPool_ == nullptr) {
      historyPool_ = static_cast<char*>(heap_caps_malloc(HISTORY_POOL_SIZE_SMALL, MALLOC_CAP_SPIRAM));
      historyPoolCap_ = HISTORY_POOL_SIZE_SMALL;
    }
    if (historyPool_ == nullptr) {
      historyPool_ =
          static_cast<char*>(heap_caps_malloc(HISTORY_POOL_SIZE_SMALL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      historyPoolCap_ = HISTORY_POOL_SIZE_SMALL;
    }
    if (historyPool_ == nullptr) {
      historyPoolCap_ = 0;
      LOG_ERR("VOICE", "OOM: history pool %u bytes", static_cast<unsigned>(HISTORY_POOL_SIZE));
    }
  }
  if (!questionInput_ && historyLine_ == nullptr) {
    historyLine_ = static_cast<char*>(heap_caps_malloc(HISTORY_LINE_SIZE, MALLOC_CAP_SPIRAM));
    if (historyLine_ == nullptr) {
      historyLine_ = static_cast<char*>(heap_caps_malloc(HISTORY_LINE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (historyLine_ == nullptr) {
      LOG_ERR("VOICE", "OOM: history line buffer %u bytes", static_cast<unsigned>(HISTORY_LINE_SIZE));
    }
  }
  // Note-mode line scratch: same size class and allocation order as the
  // history line (an escaped NTF1 line is at most NoteStore::LINE_CAP bytes).
  // Shared across entries; freed in onExit().
  if (!questionInput_ && noteLine_ == nullptr) {
    noteLineCap_ = NoteStore::LINE_CAP;
    noteLine_ = static_cast<char*>(heap_caps_malloc(noteLineCap_, MALLOC_CAP_SPIRAM));
    if (noteLine_ == nullptr) {
      noteLine_ = static_cast<char*>(heap_caps_malloc(noteLineCap_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (noteLine_ == nullptr) {
      noteLineCap_ = 0;
      LOG_ERR("VOICE", "OOM: note line buffer %u bytes", static_cast<unsigned>(NoteStore::LINE_CAP));
    }
  }
  // Days are listed lazily when the browser opens; nothing SD here.
  //
  // Note mode brings no radio up here, and must not be gated on one: a quick
  // mark ("快捷书签") is a plain NoteStore::append and the note browser is plain
  // SD reads, so both were unreachable offline — the Wi-Fi list popped before
  // this screen could paint, and Back from it finished the whole activity.
  // STT is the only network user in note mode, and it degrades on its own
  // instead of detouring: startRecording() reports STR_STT_NO_NETWORK and the
  // edit keyboard's dictation does the same, so the mark picker and the note
  // list stay one key away. The session is never dialled in note mode either
  // way, so the Idle panel needs no status line.
  if (noteContext_.valid || questionInput_) {
    // Note mode and question mode both stay offline on entry: no radio, no
    // gateway dial. STT is the only network user and it detours on its own
    // (startRecording() / dictation), so both screens paint immediately.
    LOG_INF("VOICE", "%s: no Wi-Fi bring-up (status=%d)", noteContext_.valid ? "note mode" : "question mode",
            static_cast<int>(WiFi.status()));
    requestUpdate();
    return;
  }
  // Chat mode connects once per entry: loadConfig() validates what it can
  // before any radio work, so a missing host/token shows the reason immediately
  // instead of failing after a recording the user just made.
  //
  // Bring the network up first when it is down: without it, the STT upload
  // and the chat.send both fail at the moment the user presses Confirm, long
  // after the screen said it was ready. WifiSelectionActivity tries the last
  // known network on entry (autoConnect defaults to true) and only falls back
  // to the scan list, so the usual case is one automatic connect, not a detour.
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("VOICE", "no network, opening Wi-Fi selection");
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               // The user backed out of the network list —
                               // there is nothing to capture offline, so leave
                               // the way we came rather than show a dead screen.
                               finish();
                               return;
                             }
                             startSession();
                           });
    requestUpdate();
    return;
  }
  startSession();
  requestUpdate();
}

void VoiceActivity::onExit() {
  WIFI_IDLE.release();  // ends the onEnter() hold; the idle countdown resumes
  if (activeInstance_ == this) activeInstance_ = nullptr;
  recorder_.close();
  sttParser_.reset();
  // The relay session belongs to this activity (unlike the process-wide
  // OpenClaw session): cancel best-effort and release the socket, then the
  // reply buffer. The notifier points at `this` and dies with the client.
  if (relay_ != nullptr) {
    relay_->close();
    relay_.reset();
  }
  relayRound_ = false;
  relayReplyLen_ = 0;
  if (relayReply_ != nullptr) {
    heap_caps_free(relayReply_);
    relayReply_ = nullptr;
  }
  // L3 probe: the session is process-wide now, so leaving voice must not tear
  // the gateway link down — the workbench cards read it. The notifier points
  // at this activity's `this` and has to go before the object does.
  session_.setNotifier(nullptr, nullptr);
  if (historyPool_ != nullptr) {
    heap_caps_free(historyPool_);
    historyPool_ = nullptr;
  }
  historyPoolCap_ = 0;
  if (historyLine_ != nullptr) {
    heap_caps_free(historyLine_);
    historyLine_ = nullptr;
  }
  if (noteLine_ != nullptr) {
    heap_caps_free(noteLine_);
    noteLine_ = nullptr;
  }
  noteLineCap_ = 0;
  historyEntryCount_ = 0;
  historyDayCount_ = 0;
  Activity::onExit();
}

bool VoiceActivity::preventAutoSleep() {
  return state_ == State::Listening || state_ == State::Transcribing || state_ == State::Sending ||
         session_.state() == OpenClaw::Session::State::Connecting;
}

bool VoiceActivity::preventAutoUpdate() const {
  // Any repaint while the capture is live blocks the loop that drains the 8 ms
  // I2S ring — the refresh costs ~0.8 s of audio. Covers the automatic
  // post-result repaint after the Wi-Fi detour's handler already started a
  // recording (the keyboard has its own override for its dictation).
  return state_ == State::Listening || dictationState_ == DictationState::Recording;
}

void VoiceActivity::startSession() {
  // Warm the relay socket first: it is independent of the OpenClaw session,
  // and the dial must complete before the first capture starts.
  warmRelay();
  if (!session_.loadConfig()) {
    // reason() carries the translated line; keep the screen usable.
    return;
  }
  session_.setNotifier(this, &VoiceActivity::onSessionNotifyTrampoline);
  if (!session_.startConnect()) {
    return;  // no network / no clock: reason() is set
  }
  // Connecting runs on the next loop pass; the Idle screen says so via the
  // session's status line.
}

void VoiceActivity::onSessionNotifyTrampoline(void* ctx) { static_cast<VoiceActivity*>(ctx)->onSessionNotify(); }

void VoiceActivity::onSessionNotify() {
  // Session callbacks fire from inside the WebSocket dispatch — repaint only,
  // and only for states that show session traffic.
  if (state_ == State::Idle || state_ == State::Sending || state_ == State::Answer || state_ == State::Review) {
    requestUpdate();
  }
}

void VoiceActivity::fail(const char* reason) {
  LOG_ERR("VOICE", "failed: %s", reason != nullptr ? reason : "?");
  failReason_ = reason;
  haveAnswer_ = false;
  state_ = State::Failed;
  requestUpdate();
}

void VoiceActivity::resetRoundTrip() {
  transcript_[0] = '\0';
  haveTranscript_ = false;
  haveAnswer_ = false;
  oweSend_ = false;
  failReason_ = nullptr;
  // A new capture inherits nothing from a summarize round: pendingText_ points
  // at historyLine_, which the next recordHistoryTurn() needs as append
  // scratch, and summaryRound_/cameFromHistory_ belong to the old round only.
  // Cleared on entry to a summary round as well (sendHistorySummary), before
  // the flags are re-armed for the new round.
  pendingText_ = nullptr;
  summaryRound_ = false;
  cameFromHistory_ = false;
  // Relay per-round state. The instance and the reply buffer are activity-
  // lifetime; the round flags and the PCM cursor are not.
  relayRound_ = false;
  relayFallbackTried_ = false;
  relayEndSent_ = false;
  relayDone_ = false;
  relayError_ = false;
  relayRetryable_ = false;
  relayErrorCode_ = VoiceRelay::ErrorCode::None;
  relaySentSamples_ = 0;
  relayEndMs_ = 0;
  relayReplyLen_ = 0;
  if (relayReply_ != nullptr) relayReply_[0] = '\0';
}

namespace {
// Days-since-epoch of the user's local "today" for the flat history row
// labels; -1 while the clock is unsynced (labels then fall back to full
// dates). Same clock source and offset handling as the Workbench date line.
int64_t todayHistoryDays() {
  const int64_t nowMs = OpenClaw::nowEpochMs();
  if (nowMs < OpenClaw::MIN_SANE_EPOCH_MS) return -1;
  const int64_t localSec = ChatHistoryFormat::localEpochSeconds(nowMs, SETTINGS.clockUtcOffsetQ);
  return localSec >= 0 ? localSec / 86400 : (localSec - 86399) / 86400;
}

// The six quick marks ("快捷书签"). Why these six: they cover what a reader
// actually stops for — come back here, liked it, doubt it, keep the line, it's
// about me, fact-check it — and each one is a fixed string, so marking a page
// costs an SD append instead of an STT upload plus an agent turn. The page the
// mark was made on and its opening text are stored with it, which is what makes
// a two-character tag findable a week later.
constexpr StrId QUICK_TAGS[] = {StrId::STR_MARK_REVISIT, StrId::STR_MARK_LIKED,    StrId::STR_MARK_DOUBT,
                                StrId::STR_MARK_QUOTE,   StrId::STR_MARK_PERSONAL, StrId::STR_MARK_VERIFY};
constexpr size_t QUICK_TAG_COUNT = sizeof(QUICK_TAGS) / sizeof(QUICK_TAGS[0]);
// Chat preset questions (State::ChatPresets): fixed translations, no user
// config. Short enough to sit in transcript_ (CHAT_MESSAGE_CAP) and to read
// as one list row at UI_10 on a portrait panel.
constexpr StrId CHAT_PRESETS[] = {StrId::STR_CHAT_PRESET_1, StrId::STR_CHAT_PRESET_2, StrId::STR_CHAT_PRESET_3,
                                  StrId::STR_CHAT_PRESET_4};
constexpr size_t CHAT_PRESET_COUNT = sizeof(CHAT_PRESETS) / sizeof(CHAT_PRESETS[0]);
}  // namespace

void VoiceActivity::openHistory() {
  if (historyPool_ == nullptr || historyLine_ == nullptr) {
    fail(tr(STR_MIC_OOM));  // rare: PSRAM exhausted at entry; reuse the line
    return;
  }
  // Relist on every open: turns appended during this session have to show up,
  // and the old "list once per activity" cache silently hid them.
  historyDayCount_ = ChatHistory::listDays(historyDays_, OpenClaw::ChatHistory::MAX_DAYS);
  historyDayIndex_ = 0;  // flat list: the top row is both cursor and open day
  historyDeleteArmed_ = false;
  historyScroll_ = 0;
  state_ = State::HistoryList;
  requestUpdate();
}

bool VoiceActivity::loadHistoryDay(const size_t dayIndex) {
  if (historyPool_ == nullptr || historyLine_ == nullptr || dayIndex >= historyDayCount_) return false;
  historyDayIndex_ = dayIndex;
  historyEntryCount_ = ChatHistory::loadDays(historyDays_, dayIndex, 1, historyPool_, historyPoolCap_, historyEntries_,
                                             OpenClaw::ChatHistory::MAX_ENTRIES, historyLine_, HISTORY_LINE_SIZE);
  // Pair the entries into turns: every user entry starts one, plus a lone
  // leading assistant when the pool's tail window split a pair.
  size_t users = 0;
  for (size_t i = 0; i < historyEntryCount_; i++) {
    if (historyEntries_[i].isUser) users++;
  }
  historyTurnCount_ = users + (historyEntryCount_ > 0 && !historyEntries_[0].isUser ? 1 : 0);
  // Start at the end: the panel shows the newest turns first thing.
  historyScroll_ = 0;
  historySelected_ = historyTurnCount_ > 0 ? historyTurnCount_ - 1 : 0;
  return true;
}

size_t VoiceActivity::drawTextWindow(const int fontId, const char* text, const int x, const int y, const int width,
                                     const int capacity, size_t& firstLine) {
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

void VoiceActivity::drawPipeline(const int x, const int y) {
  static constexpr StrId stages[] = {StrId::STR_VOICE_STAGE_RECORD, StrId::STR_VOICE_STAGE_STT,
                                     StrId::STR_VOICE_STAGE_SEND, StrId::STR_VOICE_STAGE_REPLY};
  constexpr size_t stageCount = sizeof(stages) / sizeof(stages[0]);
  int active = -1;
  switch (state_) {
    case State::Listening:
      active = 0;
      break;
    case State::Transcribing:
      active = 1;
      break;
    case State::Sending:
      // The send itself is stage 2 until the gateway has it and we are only
      // waiting on the reply, which is stage 3. A relay round has no separate
      // send stage on this device: the audio already streamed live, so once
      // the capture is over the only thing left is the reply.
      active = relayRound_ ? 3 : (session_.awaitingReply() ? 3 : 2);
      break;
    case State::Answer:
      active = 3;
      break;
    default:
      break;
  }
  int cursor = x;
  for (size_t i = 0; i < stageCount; i++) {
    const bool isCurrent = static_cast<int>(i) == active;
    char chip[40];
    if (isCurrent) {
      snprintf(chip, sizeof(chip), "[%s]", I18N.get(stages[i]));
    }
    const char* label = isCurrent ? chip : I18N.get(stages[i]);
    const EpdFontFamily::Style style = isCurrent ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawText(UI_10_FONT_ID, cursor, y, label, true, style);
    cursor += renderer.getTextWidth(UI_10_FONT_ID, label, style);
    if (i + 1 < stageCount) {
      renderer.drawText(UI_10_FONT_ID, cursor, y, " > ", true, EpdFontFamily::REGULAR);
      cursor += renderer.getTextWidth(UI_10_FONT_ID, " > ", EpdFontFamily::REGULAR);
    }
  }
}

const char* VoiceActivity::gatewayStatus() const {
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
      return nullptr;
  }
}

// ── Voice Relay ─────────────────────────────────────────────────────────────

bool VoiceActivity::relayConfigured() {
  // Workbench to-do round: the OpenClaw agent has to see the transcript — it
  // is the only backend with the scheduling tool the card then lists. The
  // relay may be perfectly healthy here; it just cannot create the job.
  if (gatewayChat_) return false;
  if (SETTINGS.voiceRelayEnabled == 0) return false;
  // Manual send mode exists to review the local transcript before anything is
  // transmitted; the relay streams the audio live, so the chat round only
  // participates in auto mode. Dictation (notes / custom questions) never
  // sends to the chat — the transcript goes to the keyboard — so the gate
  // does not apply to it.
  if (!dictationMode() && SETTINGS.voiceAutoSend == 0) return false;
  CLOUD_CFG.load();  // NVS, idempotent; repopulates relayUrl/Token/DeviceId
  return CLOUD_CFG.relayUrl[0] != '\0' && CLOUD_CFG.relayToken[0] != '\0' && CLOUD_CFG.relayDeviceId[0] != '\0';
}

bool VoiceActivity::ensureRelay() {
  if (relay_ == nullptr) {
    relay_ = makeUniqueNoThrow<VoiceRelay::Client>();
    if (relay_ == nullptr) {
      LOG_ERR("VOICE", "OOM: relay client");
      fail(tr(STR_MIC_OOM));
      return false;
    }
    relay_->setEventNotifier(this, &VoiceActivity::onRelayEventTrampoline);
  }
  if (relayReply_ == nullptr) {
    relayReply_ = static_cast<char*>(heap_caps_malloc(OpenClaw::CHAT_BUF_CAP + 1, MALLOC_CAP_SPIRAM));
    if (relayReply_ == nullptr) {
      relayReply_ =
          static_cast<char*>(heap_caps_malloc(OpenClaw::CHAT_BUF_CAP + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (relayReply_ == nullptr) {
      LOG_ERR("VOICE", "OOM: relay reply buffer %u bytes", static_cast<unsigned>(OpenClaw::CHAT_BUF_CAP + 1));
      fail(tr(STR_MIC_OOM));
      return false;
    }
    relayReply_[0] = '\0';
  }
  return true;
}

void VoiceActivity::warmRelay() {
  if (!relayConfigured()) return;
  if (relay_ == nullptr) {
    relay_ = makeUniqueNoThrow<VoiceRelay::Client>();
    if (relay_ == nullptr) return;  // the round itself retries the allocation
    relay_->setEventNotifier(this, &VoiceActivity::onRelayEventTrampoline);
  }
  if (relay_->state() != VoiceRelay::Client::State::Idle) return;
  if (!relay_->prepare(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId)) {
    LOG_INF("VOICE", "warm dial skipped (%u)", static_cast<unsigned>(relay_->transportError()));
  }
}

void VoiceActivity::pumpWarmRelay() {
  // Drive a prepared dial to completion where no capture is live (startRecording
  // before the mic powers, openNoteEditor before the keyboard takes the loop).
  // loop()'s relay_->poll() drives it everywhere else. The dial's ~0.9 s blocks
  // this pass by design — it must block here, not inside the capture.
  if (relay_ == nullptr || relay_->state() != VoiceRelay::Client::State::Connecting) return;
  const uint32_t deadline = millis() + RELAY_WARM_BUDGET_MS;
  while (relay_->state() == VoiceRelay::Client::State::Connecting && !relay_->connected() && millis() < deadline) {
    relay_->poll();
    delay(1);
  }
}

void VoiceActivity::startRelayRound() {
  // Only a live or failed round must be parked for start() to accept a new
  // session. A warm socket (Connecting) and a finished round's socket (Done)
  // are reused: arming a session on an open socket dials nothing, so the
  // capture window keeps the whole handshake out of it.
  const VoiceRelay::Client::State relayState = relay_->state();
  if (relayState == VoiceRelay::Client::State::Active || relayState == VoiceRelay::Client::State::Starting ||
      relayState == VoiceRelay::Client::State::Failed) {
    relay_->close();
  }
  relayReplyLen_ = 0;
  // Null on a keyboard-only dictation round (the client was armed without the
  // chat reply buffer); the chat paths always allocate it in ensureRelay().
  if (relayReply_ != nullptr) relayReply_[0] = '\0';
  relaySentSamples_ = 0;
  relayFallbackTried_ = false;
  relayEndSent_ = false;
  relayDone_ = false;
  relayError_ = false;
  relayRetryable_ = false;
  relayErrorCode_ = VoiceRelay::ErrorCode::None;
  relayEndMs_ = 0;
  const char* mode = dictationMode() ? "dictation" : nullptr;
  if (!relay_->start(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, mode)) {
    LOG_ERR("VOICE", "relay start rejected (%u), staying on legacy", static_cast<unsigned>(relay_->transportError()));
    relayRound_ = false;
    return;
  }
  relayRound_ = true;
  LOG_INF("VOICE", "relay round: mode=%s req=%s", mode != nullptr ? mode : "relay", relay_->requestId());
}

void VoiceActivity::pumpRelayCapture() {
  if (relay_ == nullptr || relay_->state() != VoiceRelay::Client::State::Active) return;
  const size_t total = recorder_.samples();
  if (total <= relaySentSamples_) return;
  // The warm socket keeps this at one VAD chunk (well under 500 ms); a larger
  // backlog means the PCM buffer itself lagged a live round — the canary for
  // a dial that still landed inside the capture.
  const size_t backlogMs = (total - relaySentSamples_) * 1000 / HalMicrophone::SAMPLE_RATE;
  if (backlogMs > 500) LOG_INF("VOICE", "relay catch-up: %u ms pending", static_cast<unsigned>(backlogMs));
  const size_t bytes = (total - relaySentSamples_) * sizeof(int16_t);
  if (relay_->sendAudio(reinterpret_cast<const uint8_t*>(recorder_.pcm() + relaySentSamples_), bytes)) {
    relaySentSamples_ = total;
  }
}

void VoiceActivity::onRelayEventTrampoline(void* ctx, const VoiceRelay::Event& event) {
  static_cast<VoiceActivity*>(ctx)->onRelayEvent(event);
}

void VoiceActivity::onRelayEvent(const VoiceRelay::Event& event) {
  // Inside the WS dispatch (relay_->poll() runs on this task): flags and text
  // only — never a send, never a render. The recording screen owns the panel
  // while Listening (the mic power rail shares GPIO27 with EPD_RST), so
  // repaints are held back until the capture has ended.
  using ET = VoiceRelay::EventType;
  switch (event.type) {
    case ET::AsrFinal: {
      // The server accumulates sentences: each asr.final carries the full
      // transcript so far, so this replaces rather than appends.
      const size_t n = strnlen(event.text, VoiceRelay::TEXT_CAP);
      if (n > 0) {
        const size_t cap = sizeof(transcript_) - 1;
        if (event.truncated || n > cap) {
          LOG_ERR("VOICE", "asr.final truncated (event %u bytes, cap %u)", static_cast<unsigned>(n),
                  static_cast<unsigned>(cap));
        }
        const size_t copy = n < cap ? n : cap;
        snprintf(transcript_, sizeof(transcript_), "%.*s", static_cast<int>(copy), event.text);
        haveTranscript_ = true;
      }
      break;
    }
    case ET::LlmDelta: {
      if (relayReply_ == nullptr) break;
      const size_t n = strnlen(event.text, VoiceRelay::TEXT_CAP);
      const size_t room = OpenClaw::CHAT_BUF_CAP - relayReplyLen_;
      const size_t copyLen = n < room ? n : room;
      memcpy(relayReply_ + relayReplyLen_, event.text, copyLen);
      relayReplyLen_ += copyLen;
      relayReply_[relayReplyLen_] = '\0';
      if (copyLen < n) {
        LOG_ERR("VOICE", "relay reply truncated at %u bytes", static_cast<unsigned>(OpenClaw::CHAT_BUF_CAP));
      }
      break;
    }
    case ET::LlmFinal:
      // Authoritative full text; replaces the delta accumulation when it fit
      // the event buffer (deltas stand in when this frame was truncated).
      if (relayReply_ != nullptr && !event.truncated && event.text[0] != '\0') {
        const size_t n = strnlen(event.text, VoiceRelay::TEXT_CAP);
        memcpy(relayReply_, event.text, n);
        relayReplyLen_ = n;
        relayReply_[relayReplyLen_] = '\0';
      }
      break;
    case ET::SessionDone:
      relayDone_ = true;
      break;
    case ET::Error:
      relayError_ = true;
      relayRetryable_ = event.retryable;
      relayErrorCode_ = event.code;
      break;
    default:
      break;  // AsrPartial: no repaint while the capture owns the panel
  }
  if (state_ != State::Listening) requestUpdate();
}

const char* VoiceActivity::relayErrorReason() const {
  using EC = VoiceRelay::ErrorCode;
  switch (relayErrorCode_) {
    case EC::ServerOverloaded:
    case EC::AsrRateLimited:
    case EC::LlmRateLimited:
      return tr(STR_VOICE_RELAY_BUSY);
    case EC::Unauthorized:
    case EC::DeviceNotAllowed:
    case EC::AsrAuthError:
    case EC::LlmAuthError:
      return tr(STR_VOICE_RELAY_AUTH);
    default:
      return tr(STR_VOICE_RELAY_FAILED);
  }
}

void VoiceActivity::handleRelayErrorEvent() {
  const char* reason = relayErrorReason();
  LOG_ERR("VOICE", "relay error %s retryable=%d", VoiceRelay::errorCodeName(relayErrorCode_), relayRetryable_ ? 1 : 0);
  // Every relay failure falls back to the legacy MiMo + OpenClaw pipeline —
  // including the auth/protocol classes: the relay is the preferred service,
  // not a gatekeeper.
  relayFallbackOrFail(reason);
}

void VoiceActivity::relayTransportFailed() {
  const VoiceRelay::Client::TransportError te = relay_->transportError();
  LOG_ERR("VOICE", "relay transport failure %u", static_cast<unsigned>(te));
  if (te == VoiceRelay::Client::TransportError::Oom) {
    relayClose();
    relayRound_ = false;
    fail(tr(STR_MIC_OOM));
    return;
  }
  if (te == VoiceRelay::Client::TransportError::NoWifi) {
    relayClose();
    relayRound_ = false;
    fail(tr(STR_STT_NO_NETWORK));
    return;
  }
  relayFallbackOrFail(tr(STR_VOICE_RELAY_FAILED));
}

void VoiceActivity::relayFallbackOrFail(const char* reason) {
  if (relayFallbackTried_) {
    relayClose();
    relayRound_ = false;
    recorder_.close();
    fail(reason);
    return;
  }
  relayFallbackTried_ = true;
  relayClose();
  relayRound_ = false;
  relayEndSent_ = false;
  relayDone_ = false;
  relayError_ = false;
  if (state_ == State::Listening) {
    // The failure landed while the user is still speaking (dial/upgrade
    // failures arrive ~0.8 s in). Keep capturing and let the ordinary VAD
    // stop close the sentence — Listening's Complete case then routes it to
    // the legacy transcribe because relayRound_ is false. Ending the capture
    // here truncated the sentence into a partial transcript.
    LOG_INF("VOICE", "relay failed (%s) mid-capture; continuing to listen on the legacy path", reason);
    return;
  }
  LOG_INF("VOICE", "relay failed (%s), falling back to the legacy path on the captured audio", reason);
  // The capture already ended (failure during Sending) — just finalise it.
  recorder_.end();
  if (haveTranscript_) {
    if (dictationMode()) {
      // ASR already landed and there is no LLM half to fall back to: the
      // transcript is the deliverable, so it goes to the keyboard exactly as
      // the legacy path's finishTranscription() hands it over.
      LOG_INF("VOICE", "relay dictation fallback: transcript in hand");
      recorder_.close();
      openNoteEditor();
      return;
    }
    // ASR already landed; only the relay's LLM half failed, so the fallback
    // owes just the chat round trip on the shared OpenClaw session.
    LOG_INF("VOICE", "relay fallback: transcript in hand, sending via OpenClaw");
    sendToOpenClaw();
    return;
  }
  if (recorder_.samples() == 0) {
    fail(tr(STR_STT_NO_SPEECH));
    return;
  }
  startTranscribing();  // blocking STT → chat.send, exactly the legacy pipeline
}

void VoiceActivity::relayClose() {
  if (relay_ != nullptr) relay_->close();
}

void VoiceActivity::settleRelaySuccess() {
  if (dictationMode()) {
    // A dictation session has no LLM half: session.done means the accumulated
    // transcript is final. Without one, the legacy path on the same capture is
    // the retry — the same contract as the chat branch below.
    if (!haveTranscript_) {
      relayFallbackOrFail(tr(STR_STT_NO_SPEECH));
      return;
    }
    LOG_INF("VOICE", "relay dictation done (%u bytes)", static_cast<unsigned>(strlen(transcript_)));
    relayRound_ = false;
    // The socket stays up for the keyboard's next round (its 60 s idle close
    // is caught by the upkeep poll). Same release order as
    // finishTranscription(): free the PCM, then hand the transcript to the
    // keyboard (语音书签 / custom question).
    recorder_.close();
    openNoteEditor();
    return;
  }
  if (relayReplyLen_ == 0) {
    // session.done without a usable reply is not an answer; the legacy path on
    // the same capture is the retry.
    relayFallbackOrFail(tr(STR_OPENCLAW_NO_REPLY));
    return;
  }
  haveAnswer_ = true;
  answerScroll_ = 0;
  state_ = State::Answer;
  // [LAT]: server stage marks ride session.done; the local ones are this
  // device's view of the same stages. Same line shape as the legacy [LAT] logs.
  const VoiceRelay::Latency& sl = relay_->lastEvent().latency;
  const VoiceRelay::Latency& ll = relay_->localLatency();
  LOG_INF(
      "VOICE",
      "[LAT] relay server connect=%ld asrFirstPartial=%ld asrFinal=%ld llmFirstToken=%ld llmFinal=%ld visibleTotal=%ld",
      static_cast<long>(sl.connect), static_cast<long>(sl.asrFirstPartial), static_cast<long>(sl.asrFinal),
      static_cast<long>(sl.llmFirstToken), static_cast<long>(sl.llmFinal), static_cast<long>(sl.visibleTotal));
  LOG_INF(
      "VOICE",
      "[LAT] relay local  connect=%ld asrFirstPartial=%ld asrFinal=%ld llmFirstToken=%ld llmFinal=%ld visibleTotal=%ld",
      static_cast<long>(ll.connect), static_cast<long>(ll.asrFirstPartial), static_cast<long>(ll.asrFinal),
      static_cast<long>(ll.llmFirstToken), static_cast<long>(ll.llmFinal), static_cast<long>(ll.visibleTotal));
  // The session is over but the socket stays up: the next round arms a fresh
  // session on it (no dial in the capture window). relayRound_ keeps routing
  // the panel and the history append to relayReply_.
  // Backstop: a stale settle during a summarize round must not append the
  // summary label as a fake user turn (the session path guards the same way).
  if (!summaryRound_) recordHistoryTurn();
  // The answer is shown and fallback is off the table: release the PCM the way
  // finishTranscription() does for the legacy pipeline.
  recorder_.close();
  requestUpdate();
}

void VoiceActivity::loopRelaySending() {
  if (relay_ == nullptr) {
    relayRound_ = false;
    fail(tr(STR_VOICE_RELAY_FAILED));
    return;
  }
  // Error event before transport failure: the event sets both relayError_ and
  // the client's Failed state, and carries the specific reason.
  if (relayError_) {
    handleRelayErrorEvent();
    return;
  }
  if (relay_->state() == VoiceRelay::Client::State::Failed) {
    relayTransportFailed();
    return;
  }
  if (relayDone_) {
    settleRelaySuccess();
    return;
  }
  if (!relayEndSent_) {
    // Capture finished while the dial was still handshaking: flush the tail
    // first (the client gates on Active), then end the audio — the relay's
    // live ASR never missed a sample because the PCM cursor caught up here.
    pumpRelayCapture();
    if (relay_->state() == VoiceRelay::Client::State::Active) {
      const char* reason = recorder_.endedBySilence() ? "vad_silence" : "duration_cap";
      if (relay_->finishAudio(reason)) {
        relayEndSent_ = true;
        relayEndMs_ = millis();
        lastWaitShownSec_ = 0;
        requestUpdate();
      }
    }
    return;  // Connecting/Starting: the client's connect/start timers decide
  }
  // waiting read-out: repaint exactly when the displayed second changes.
  const uint32_t shownSec = relayEndMs_ != 0 ? (millis() - relayEndMs_) / 1000 : 0;
  if (shownSec != lastWaitShownSec_) {
    lastWaitShownSec_ = shownSec;
    requestUpdate();
  }
}

void VoiceActivity::startRecording() {
  if (WiFi.status() != WL_CONNECTED) {
    // Note mode never brings the radio up on entry (see onEnter), so the talk
    // key is the point that has to: connect first, then transcribe. The detour
    // is opt-in — a quick mark never reaches it — and cancelling it lands on
    // this screen's own no-network failure instead of finishing the activity,
    // so the mark picker and the note list stay one key away.
    //
    // The capture starts inside the result handler, the way the other Wi-Fi
    // children resume their work (FontDownloadActivity): ActivityManager
    // queues one requestUpdate() right after a handler returns, so deferring
    // to loop() would make requestUpdateAndWait() return on that repaint
    // instead of on the recording frame — the mic would come up before the
    // screen showed it. Here the frame is requested first; the repaint
    // ActivityManager then queues is the same frame, drawn once more.
    if (noteContext_.valid || questionInput_) {
      LOG_INF("VOICE", "%s capture offline, opening Wi-Fi selection", noteContext_.valid ? "note" : "question");
      startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                                 fail(tr(STR_STT_NO_NETWORK));
                                 return;
                               }
                               startRecording();
                             });
      return;
    }
    fail(tr(STR_STT_NO_NETWORK));
    return;
  }
  resetRoundTrip();
  // Paint first, power the mic second. The capture must not be interrupted by
  // a panel refresh, so the recording screen (stage row + mic icon + gateway
  // state) is pushed and waited for while the rail is still idle; the panel is
  // then left alone until the capture ends and startTranscribing() paints the
  // next stage.
  state_ = State::Listening;
  requestUpdateAndWait();
  // The dial must be done before the mic powers up — a pass of it inside a
  // capture starves the 8 ms I2S DMA ring (measured: every relay round's
  // server frame count trailed its capture by exactly the handshake window).
  // onEnter warmed the socket; this catches the paths that arrive cold (no
  // network until the Wi-Fi detour above, or a socket since closed).
  warmRelay();
  pumpWarmRelay();
  // A rejected start simply leaves relayRound_ false and the capture proceeds
  // on the legacy path.
  if (relayConfigured()) {
    if (!ensureRelay()) return;  // fail() already set the screen
    startRelayRound();
  }
  recorder_.setVadThreshold(VAD_RMS_THRESHOLD);
  if (!recorder_.begin(HalMicrophone::SAMPLE_RATE, MAX_RECORD_SECONDS, SILENCE_STOP_MS)) {
    if (relayRound_) {
      relayClose();
      relayRound_ = false;
    }
    fail(recorder_.error() == Voice::Recorder::Error::Alloc ? tr(STR_MIC_OOM) : tr(STR_MIC_UNAVAILABLE));
    return;
  }
  LOG_INF("VOICE", "listening (vad rms>=%d, stop after %u ms silence)", static_cast<int>(VAD_RMS_THRESHOLD),
          static_cast<unsigned>(SILENCE_STOP_MS));
}

void VoiceActivity::startTranscribing() {
  if (!sttParser_) {
    sttParser_ = makeUniqueNoThrow<SttCodec::SseTranscriptParser>();
    if (!sttParser_) {
      LOG_ERR("VOICE", "OOM: SSE parser");
      fail(tr(STR_MIC_OOM));
      return;
    }
  } else {
    sttParser_->reset();
  }
  state_ = State::Transcribing;
  requestUpdateAndWait();  // "Transcribing…" is on the panel before the blocking call
}

void VoiceActivity::runTranscription() {
  SttClient::Config cfg;
  if (!SttClient::loadConfig(sttUrlBuf_, sizeof(sttUrlBuf_), sttKeyBuf_, sizeof(sttKeyBuf_), cfg)) {
    finishTranscription(SttClient::ERR_NO_KEY);
    return;
  }
  SttClient::Stats stats;
  const SttClient::Result result =
      SttClient::transcribe(cfg, recorder_.pcm(), recorder_.samples(), HalMicrophone::SAMPLE_RATE, *sttParser_, &stats);
  finishTranscription(result);
}

void VoiceActivity::finishTranscription(SttClient::Result result) {
  if (result != SttClient::OK || sttParser_->textSize() == 0) {
    LOG_ERR("VOICE", "transcription failed: %s", SttClient::resultName(result));
    fail(result == SttClient::OK ? tr(STR_STT_NO_SPEECH) : tr(STR_STT_FAILED));
    return;
  }
  snprintf(transcript_, sizeof(transcript_), "%.*s", static_cast<int>(sttParser_->textSize()), sttParser_->text());
  haveTranscript_ = true;
  LOG_INF("VOICE", "transcript (%u bytes): %s", static_cast<unsigned>(sttParser_->textSize()), transcript_);
  // Free the capture before the socket work: the reply buffer and the TLS
  // session want the PSRAM the PCM holds.
  recorder_.close();
  // Manual mode parks the transcript in Review; the user sends with Confirm
  // (§8.6). Auto sends immediately, as before. Note mode parks the transcript
  // in the keyboard instead of saving it outright (语音书签 shortcut).
  if (noteContext_.valid || questionInput_) {
    openNoteEditor();
    return;
  }
  if (SETTINGS.voiceAutoSend == 0) {
    state_ = State::Review;
    requestUpdate();
    return;
  }
  sendToOpenClaw();
}

void VoiceActivity::recordHistoryTurn() {
  // One append per completed round trip — never per UI action, never per
  // delta (SPIFFS/SD write discipline). tsMs comes from the same epoch clock
  // the handshake signs with; the user's clock offset groups the file.
  if (historyLine_ == nullptr) {
    LOG_ERR("VOICE", "history skipped: no line buffer");
    return;
  }
  const int64_t now = OpenClaw::nowEpochMs();
  const bool okUser = ChatHistory::append(static_cast<uint64_t>(now), SETTINGS.clockUtcOffsetQ, true, transcript_,
                                          historyLine_, HISTORY_LINE_SIZE);
  // The reply source follows the round: relay rounds accumulate in relayReply_
  // (session_'s buffers hold the previous legacy round's text), legacy rounds
  // read the session as before.
  const char* replyText = relayRound_ ? relayReply_ : session_.replyText();
  const size_t replySize = relayRound_ ? relayReplyLen_ : session_.replySize();
  const bool okReply = haveAnswer_ && replySize > 0 &&
                       ChatHistory::append(static_cast<uint64_t>(now), SETTINGS.clockUtcOffsetQ, false, replyText,
                                           historyLine_, HISTORY_LINE_SIZE);
  if (okUser || okReply) {
    LOG_INF("VOICE", "history: %s", okUser && okReply ? "user+reply" : okUser ? "user only" : "reply only");
  }
}

void VoiceActivity::deleteHistoryDay() {
  historyDeleteArmed_ = false;
  if (historyDayCount_ == 0 || historyDayIndex_ >= historyDayCount_) return;
  const bool ok = ChatHistory::removeDay(historyDays_[historyDayIndex_]);
  LOG_INF("VOICE", "history day deleted: %s (%s)", historyDays_[historyDayIndex_], ok ? "ok" : "failed");
  historyDayCount_ = ChatHistory::listDays(historyDays_, OpenClaw::ChatHistory::MAX_DAYS);
  // Clamp the cursor onto whatever the relist left (or the empty list).
  if (historyDayCount_ == 0) {
    historyDayIndex_ = 0;
  } else if (historyDayIndex_ >= historyDayCount_) {
    historyDayIndex_ = historyDayCount_ - 1;
  }
  historyScroll_ = 0;
  if (!ok) {
    GUI.drawPopup(renderer, tr(STR_NOTE_DELETE_FAILED));
  }
  requestUpdate();
}

void VoiceActivity::sendHistorySummary() {
  if (historyDayCount_ == 0 || historyPool_ == nullptr || historyLine_ == nullptr) return;
  if (historyDayIndex_ >= historyDayCount_) return;
  // One file: the highlighted day. buildSummaryPrompt still caps the prompt
  // at CHAT_MESSAGE_CAP, so a long day keeps its newest turns.
  historyEntryCount_ =
      ChatHistory::loadDays(historyDays_, historyDayIndex_, 1, historyPool_, historyPoolCap_, historyEntries_,
                            OpenClaw::ChatHistory::MAX_ENTRIES, historyLine_, HISTORY_LINE_SIZE);
  char label[24];
  ChatHistoryFormat::historyDayRowLabel(label, sizeof(label), historyDays_[historyDayIndex_], todayHistoryDays(),
                                        tr(STR_TODAY), tr(STR_YESTERDAY), tr(STR_DAY_BEFORE));
  if (historyEntryCount_ == 0) {
    LOG_ERR("VOICE", "summary skipped: nothing to summarize in %s", label);
    GUI.drawPopup(renderer, tr(STR_VOICE_HISTORY_EMPTY));
    requestUpdate();
    return;
  }
  char instruction[96];
  snprintf(instruction, sizeof(instruction), "%s %s", tr(STR_VOICE_SUMMARY_REQUEST), label);
  if (!ChatHistoryFormat::buildSummaryPrompt(historyLine_, OpenClaw::CHAT_MESSAGE_CAP, historyEntries_,
                                             historyEntryCount_, historyPool_, instruction, tr(STR_VOICE_YOU),
                                             tr(STR_VOICE_ASSISTANT))) {
    LOG_ERR("VOICE", "summary skipped: prompt build failed for %s", label);
    GUI.drawPopup(renderer, tr(STR_VOICE_HISTORY_EMPTY));
    requestUpdate();
    return;
  }
  // A settled relay round leaves relayRound_/relayDone_ up; without the reset
  // below this round would route into loopRelaySending(), re-show the previous
  // relay answer under the summary label and append a fake turn (the guard in
  // settleRelaySuccess is the backstop). Flags are set again right after.
  resetRoundTrip();
  // Display keeps the short label; the long prompt goes out via pendingText_.
  snprintf(transcript_, sizeof(transcript_), "%s %s", tr(STR_VOICE_SUMMARY_OF), label);
  haveTranscript_ = true;
  pendingText_ = historyLine_;
  summaryRound_ = true;
  cameFromHistory_ = true;
  LOG_INF("VOICE", "summary prompt (%s): %u bytes", label, static_cast<unsigned>(strlen(historyLine_)));
  sendToOpenClaw();
}

void VoiceActivity::openNoteList() {
  // Fill the shared PSRAM pool with this book's notes (newest kept). The
  // load() below reuses historyLine_ as read scratch.
  if (historyPool_ == nullptr || historyLine_ == nullptr) {
    LOG_ERR("VOICE", "note list skipped: no pool/line buffer");
    return;
  }
  noteEntryCount_ = NoteStore::load(noteContext_.bookBase, historyPool_, historyPoolCap_, noteEntries_,
                                    NOTE_MAX_ENTRIES, historyLine_, HISTORY_LINE_SIZE);
  noteSelected_ = 0;
  noteViewScroll_ = 0;
  noteDeleteArmed_ = false;
  state_ = State::NoteList;  // an empty list still renders, with a hint
  requestUpdate();
}

void VoiceActivity::deleteSelectedNote() {
  if (noteEntryCount_ == 0 || noteSelected_ >= noteEntryCount_) return;
  // historyLine_ is the load scratch and at least NoteStore::LINE_CAP bytes,
  // so removeEntry streams the rewrite through the buffer we already own.
  if (!NoteStore::removeEntry(noteContext_.bookBase, noteSelected_, historyLine_, HISTORY_LINE_SIZE)) {
    LOG_ERR("VOICE", "note delete failed: entry %u of %s", static_cast<unsigned>(noteSelected_), noteContext_.bookBase);
    GUI.drawPopup(renderer, tr(STR_NOTE_DELETE_FAILED));
    noteDeleteArmed_ = false;
    requestUpdate();
    return;
  }
  noteEntryCount_ = NoteStore::load(noteContext_.bookBase, historyPool_, historyPoolCap_, noteEntries_,
                                    NOTE_MAX_ENTRIES, historyLine_, HISTORY_LINE_SIZE);
  if (noteSelected_ >= noteEntryCount_) noteSelected_ = noteEntryCount_ > 0 ? noteEntryCount_ - 1 : 0;
  noteViewScroll_ = 0;
  noteDeleteArmed_ = false;
  LOG_INF("VOICE", "note deleted, %u left in %s", static_cast<unsigned>(noteEntryCount_), noteContext_.bookBase);
  requestUpdate();
}

void VoiceActivity::openNoteEditor() {
  // Note mode used to write the .ntf the instant STT returned. The transcript
  // now opens in the keyboard first: insert/delete until it reads right, and
  // only Confirm saves (saveNoteFromTranscript → NoteSaved). Back discards —
  // no SD write, and Idle lets the capture be redone. The dictation hooks make
  // the power key the talk key right here: capture, transcribe, insert at the
  // cursor, keep editing.
  //
  // The keyboard owns the loop next, so this is the last pass that can drive
  // the relay dial: complete it here, where no capture is live (see
  // warmRelay()). A warm socket makes the first power-key round loss-free.
  warmRelay();
  pumpWarmRelay();
  // Question mode with a prefill (AI 生图 "再来一张"): the previous prompt
  // waits in the editor when the user typed nothing and spoke nothing. A
  // transcript from this round always wins — the seed is the fallback.
  if (questionInput_ && transcript_[0] == '\0' && questionSeed_ != nullptr && questionSeed_[0] != '\0') {
    snprintf(transcript_, sizeof(transcript_), "%s", questionSeed_);
  }
  VoiceDictationHooks hooks;
  hooks.ctx = this;
  hooks.start = &VoiceActivity::dictationStartTrampoline;
  hooks.poll = &VoiceActivity::dictationPollTrampoline;
  hooks.text = &VoiceActivity::dictationTextTrampoline;
  hooks.error = &VoiceActivity::dictationErrorTrampoline;
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput,
                                              questionInput_ ? I18N.get(questionTitle_) : tr(STR_VOICE_BOOKMARK_EDIT),
                                              transcript_, sizeof(transcript_) - 1, InputType::Text, hooks),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          LOG_INF("VOICE", "note edit cancelled; nothing saved");
          haveTranscript_ = false;
          transcript_[0] = '\0';
          state_ = State::Idle;
          requestUpdate();
          return;
        }
        snprintf(transcript_, sizeof(transcript_), "%s", std::get<KeyboardResult>(result.data).text.c_str());
        if (questionInput_) {
          // Question mode: the text is the deliverable — hand it back to
          // AskAiActivity instead of writing a note.
          setResult(KeyboardResult{transcript_});
          finish();
          return;
        }
        saveNoteFromTranscript();
      });
}

bool VoiceActivity::dictationStartTrampoline(void* ctx) { return static_cast<VoiceActivity*>(ctx)->dictationStart(); }

int VoiceActivity::dictationPollTrampoline(void* ctx) {
  switch (static_cast<VoiceActivity*>(ctx)->dictationPoll()) {
    case DictationState::Recording:
      return 0;
    case DictationState::Ready:
      return 1;
    case DictationState::Failed:
    case DictationState::Off:
      return -1;
  }
  return -1;
}

const char* VoiceActivity::dictationTextTrampoline(void* ctx) {
  return static_cast<VoiceActivity*>(ctx)->dictationText();
}

const char* VoiceActivity::dictationErrorTrampoline(void* ctx) {
  return static_cast<VoiceActivity*>(ctx)->dictationError();
}

bool VoiceActivity::dictationStart() {
  // One capture at a time; a previous Ready/Failed result is consumed by the
  // keyboard (it inserts the text or shows the message) and starts fresh here.
  if (dictationState_ == DictationState::Recording) return false;
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("VOICE", "dictation: no network");  // the on-screen hint alone hides this from serial debugging
    dictationError_ = tr(STR_STT_NO_NETWORK);
    dictationState_ = DictationState::Failed;
    return false;
  }
  // Per-round scratch: the keyboard copied the text at construction, so a
  // silent round can never insert the previous transcript (relay or legacy).
  transcript_[0] = '\0';
  haveTranscript_ = false;
  // Arm the relay before the mic powers up — on the socket warmed by the note
  // screen (startSession) and openNoteEditor(): a cold dial here would block
  // the loop and starve the 8 ms I2S ring for the first second of speech. The
  // keyboard needs a bare client
  // only — dictation events are ASR + session.done, no reply buffer — so the
  // question-shortcut path (no panel capture before the editor) allocates one
  // without ensureRelay(). On OOM the round stays on the legacy STT instead of
  // failing the activity from under the keyboard.
  dictationRelay_ = false;
  dictationRelayEndSent_ = false;
  if (relayConfigured()) {
    if (relay_ == nullptr) {
      relay_ = makeUniqueNoThrow<VoiceRelay::Client>();
      if (relay_ == nullptr) {
        LOG_ERR("VOICE", "OOM: relay client (dictation); using legacy STT");
      } else {
        relay_->setEventNotifier(this, &VoiceActivity::onRelayEventTrampoline);
      }
    }
    if (relay_ != nullptr) {
      startRelayRound();
      dictationRelay_ = relayRound_;
      // The keyboard round must not steer the panel's answer/history routing.
      relayRound_ = false;
    }
  }
  // Same rail discipline as startRecording(), minus the voice-screen repaint:
  // the keyboard has already painted its "listening" hint (requestUpdateAndWait
  // in the keyboard's loop) before the mic is powered.
  recorder_.setVadThreshold(VAD_RMS_THRESHOLD);
  if (!recorder_.begin(HalMicrophone::SAMPLE_RATE, MAX_RECORD_SECONDS, SILENCE_STOP_MS)) {
    dictationError_ = recorder_.error() == Voice::Recorder::Error::Alloc ? tr(STR_MIC_OOM) : tr(STR_MIC_UNAVAILABLE);
    dictationState_ = DictationState::Failed;
    return false;
  }
  LOG_INF("VOICE", "dictation: capture started");
  dictationState_ = DictationState::Recording;
  return true;
}

VoiceActivity::DictationState VoiceActivity::dictationPoll() {
  if (dictationState_ != DictationState::Recording) return dictationState_;

  // The capture completes exactly once; poll it only while it is live, or a
  // post-Complete pass would read NotStarted and fail a round that is merely
  // waiting on session.done.
  const bool captureLive = recorder_.isActive();
  Voice::Recorder::Event captureEvent = Voice::Recorder::Event::Recording;
  if (captureLive) captureEvent = recorder_.poll();

  if (dictationRelay_) {
    if (relay_ != nullptr) {
      // Same rule as loop(): a cold dial is deferred until the capture ends —
      // a dial pass blocks ~0.9 s and starves the 8 ms I2S ring. Warm sockets
      // never dial here, and the post-capture section below completes a dial
      // that was deferred.
      if (!captureLive || !relay_->dialing()) relay_->poll();
      pumpRelayCapture();
    }
    if (relay_ == nullptr || relayError_ || relay_->state() == VoiceRelay::Client::State::Failed) {
      // Keep the capture: the Completed case below runs the legacy STT on it.
      LOG_INF("VOICE", "dictation relay failed; legacy STT on the saved capture");
      relayClose();
      dictationRelay_ = false;
    }
  }

  if (captureLive) {
    switch (captureEvent) {
      case Voice::Recorder::Event::Recording:
        return DictationState::Recording;
      case Voice::Recorder::Event::NoAudio:
        dictationError_ = tr(STR_MIC_UNAVAILABLE);
        recorder_.close();
        dictationState_ = DictationState::Failed;
        return dictationState_;
      case Voice::Recorder::Event::ReadFailed:
      case Voice::Recorder::Event::NotStarted:
        dictationError_ = tr(STR_MIC_READ_FAILED);
        recorder_.close();
        dictationState_ = DictationState::Failed;
        return dictationState_;
      case Voice::Recorder::Event::Complete:
        break;
    }
  }

  if (dictationRelay_) {
    // Capture over, the relay still owes a session: flush the tail once Active
    // (the handshake can arrive after the capture ended), send audio.end, then
    // wait for session.done. The transcription lands in transcript_ through the
    // same AsrFinal handler the panel round uses.
    if (!dictationRelayEndSent_) {
      if (relay_ != nullptr && relay_->state() == VoiceRelay::Client::State::Active) {
        const char* reason = recorder_.endedBySilence() ? "vad_silence" : "duration_cap";
        if (relay_->finishAudio(reason)) dictationRelayEndSent_ = true;
      }
      return DictationState::Recording;
    }
    if (!relayDone_) return DictationState::Recording;
    if (transcript_[0] != '\0') {
      recorder_.close();  // free the PCM before the keyboard repaints
      LOG_INF("VOICE", "dictation relay transcript (%u bytes): %s", static_cast<unsigned>(strlen(transcript_)),
              transcript_);
      dictationState_ = DictationState::Ready;
      return dictationState_;
    }
    // Empty final: nothing to insert; the legacy STT on the same capture is
    // the retry, exactly like the panel's empty-reply fallback.
    LOG_INF("VOICE", "dictation relay empty final; legacy STT on the saved capture");
    relayClose();
    dictationRelay_ = false;
  }

  // No relay (or it just fell back): the blocking STT call runs here, in the
  // keyboard's loop, with input swallowed and the panel static — same shape as
  // runTranscription().
  if (!sttParser_) {
    sttParser_ = makeUniqueNoThrow<SttCodec::SseTranscriptParser>();
    if (!sttParser_) {
      LOG_ERR("VOICE", "OOM: SSE parser (dictation)");
      recorder_.close();
      dictationError_ = tr(STR_MIC_OOM);
      dictationState_ = DictationState::Failed;
      return dictationState_;
    }
  } else {
    sttParser_->reset();
  }
  SttClient::Config cfg;
  if (!SttClient::loadConfig(sttUrlBuf_, sizeof(sttUrlBuf_), sttKeyBuf_, sizeof(sttKeyBuf_), cfg)) {
    recorder_.close();
    dictationError_ = tr(STR_STT_FAILED);
    dictationState_ = DictationState::Failed;
    return dictationState_;
  }
  SttClient::Stats stats;
  const SttClient::Result result =
      SttClient::transcribe(cfg, recorder_.pcm(), recorder_.samples(), HalMicrophone::SAMPLE_RATE, *sttParser_, &stats);
  recorder_.close();  // free the PCM before the keyboard repaints
  if (result != SttClient::OK || sttParser_->textSize() == 0) {
    LOG_ERR("VOICE", "dictation transcribe failed: %s", SttClient::resultName(result));
    dictationError_ = result == SttClient::OK ? tr(STR_STT_NO_SPEECH) : tr(STR_STT_FAILED);
    dictationState_ = DictationState::Failed;
    return dictationState_;
  }
  snprintf(transcript_, sizeof(transcript_), "%.*s", static_cast<int>(sttParser_->textSize()), sttParser_->text());
  LOG_INF("VOICE", "dictation transcript (%u bytes): %s", static_cast<unsigned>(sttParser_->textSize()), transcript_);
  dictationState_ = DictationState::Ready;
  return dictationState_;
}

void VoiceActivity::saveNoteFromTranscript() {
  // One append per completed capture — the same SD write discipline as the
  // history layer. tsMs from the same epoch clock the chat history uses.
  if (noteLine_ == nullptr) {
    LOG_ERR("VOICE", "note skipped: no line buffer");
    fail(tr(STR_NOTE_SAVE_FAILED));
    return;
  }
  const int64_t now = OpenClaw::nowEpochMs();
  if (!NoteStore::append(noteContext_.bookBase, static_cast<uint64_t>(now), noteContext_.spineIndex,
                         noteContext_.pageNumber, noteContext_.pageCount, transcript_, noteLine_, noteLineCap_)) {
    fail(tr(STR_NOTE_SAVE_FAILED));
    return;
  }
  haveTranscript_ = true;
  state_ = State::NoteSaved;
  LOG_INF("VOICE", "note saved to %s (spine %u page %u/%u)", noteContext_.bookBase,
          static_cast<unsigned>(noteContext_.spineIndex), static_cast<unsigned>(noteContext_.pageNumber),
          static_cast<unsigned>(noteContext_.pageCount));
  requestUpdate();
}

void VoiceActivity::openNoteQuick() {
  quickSelected_ = 0;
  state_ = State::NoteQuick;
  requestUpdate();
}

void VoiceActivity::saveQuickNote() {
  // A quick mark goes through the same note append a dictated one does, but
  // nothing before it: recorder_, runTranscription() and session_ are never
  // touched, so it costs no STT upload and no agent turn. The composed line is
  // what the NoteSaved screen then shows, so what the reader sees is exactly
  // what the .ntf holds.
  const char* tag = I18N.get(QUICK_TAGS[quickSelected_]);
  if (noteExcerpt_[0] != '\0') {
    snprintf(transcript_, sizeof(transcript_), "%s%s%s", tag, tr(STR_NOTE_SEP), noteExcerpt_);
  } else {
    snprintf(transcript_, sizeof(transcript_), "%s", tag);
  }
  saveNoteFromTranscript();
}

void VoiceActivity::openChatPresets() {
  presetSelected_ = 0;
  state_ = State::ChatPresets;
  requestUpdate();
}

void VoiceActivity::startTextSend() {
  // Text-only chat round (preset question): transcript_ is already the
  // outgoing message, nothing is being captured, and the legacy session path
  // carries it — sendPending() puts it on the wire once the link is up
  // (same oweSend_ contract as a summarize round with pendingText_ null).
  haveTranscript_ = transcript_[0] != '\0';
  haveAnswer_ = false;
  answerScroll_ = 0;
  relayRound_ = false;  // no relay audio round; the reply settles via session_
  pendingText_ = nullptr;
  state_ = State::Sending;
  oweSend_ = true;
  lastWaitShownSec_ = 0;
  requestUpdate();
}

void VoiceActivity::sendPresetDirect() {
  snprintf(transcript_, sizeof(transcript_), "%s", I18N.get(CHAT_PRESETS[presetSelected_]));
  startTextSend();
}

void VoiceActivity::openPresetEditor() {
  // The highlighted preset waits in the editor; Confirm sends the edited text
  // through the same text-send path, Back returns to the picker untouched.
  // transcript_ is this activity's member buffer, so it outlives the keyboard.
  snprintf(transcript_, sizeof(transcript_), "%s", I18N.get(CHAT_PRESETS[presetSelected_]));
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PRESET_EDIT),
                                                                 transcript_, sizeof(transcript_) - 1, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             LOG_INF("VOICE", "preset edit cancelled");
                             transcript_[0] = '\0';
                             haveTranscript_ = false;
                             state_ = State::ChatPresets;
                             requestUpdate();
                             return;
                           }
                           snprintf(transcript_, sizeof(transcript_), "%s",
                                    std::get<KeyboardResult>(result.data).text.c_str());
                           if (transcript_[0] == '\0') {
                             // An emptied editor is a discard, not an empty chat.send.
                             haveTranscript_ = false;
                             state_ = State::ChatPresets;
                             requestUpdate();
                             return;
                           }
                           startTextSend();
                         });
}

void VoiceActivity::sendToOpenClaw() {
  // The session may still be handshaking if the user spoke before it settled.
  // Wait in Sending rather than dropping a capture the user just made; loop()
  // puts the frame on the wire the moment the link is up (oweSend_).
  state_ = State::Sending;
  oweSend_ = true;
  lastWaitShownSec_ = session_.awaitingMs() / 1000;
  requestUpdate();
}

void VoiceActivity::sendPending() {
  oweSend_ = false;
  // A summarize round sends the day prompt (pendingText_ → historyLine_), not
  // the short "Summary of <date>" label that transcript_ holds for display.
  const char* message = pendingText_ != nullptr ? pendingText_ : transcript_;
  if (session_.sendMessage(message) == OpenClaw::Session::SendOutcome::Failed) {
    fail(tr(STR_OPENCLAW_SEND_FAILED));
    return;
  }
  lastWaitShownSec_ = session_.awaitingMs() / 1000;
  requestUpdate();  // "Waiting for reply…" before the first streamed delta
}

void VoiceActivity::loop() {
  // Note mode never opens the session; poll() on the unopened session is a
  // no-op, so this is safe in both modes. The same is true of the relay client
  // before its first start() — poll() guards on nullptr/Idle internally.
  //
  // While the capture is live this pass must only feed the microphone: a WS
  // dial (the OpenClaw session or a cold relay socket) blocks it for ~0.9 s
  // and starves the recorder's 8 ms I2S ring. Neither is needed mid-capture —
  // the relay flushes the buffered PCM once it connects, and the OpenClaw
  // session is not touched until the transcript is ready.
  const bool captureLive = state_ == State::Listening;
  if (!captureLive) session_.poll();
  if (relay_ != nullptr && !(captureLive && relay_->dialing())) relay_->poll();

  // Hint placement and action binding follow one rule. mapLabels() puts the
  // "previous" label on the front-Left key and "next" on the front-Right key,
  // and swaps them when the nav axis is mirrored (frontButtonFollowOrientation
  // on a rotated screen). The actions must follow the same swap, or the key a
  // hint sits over does something else — which is exactly what the old
  // history-on-Left / hint-over-Right split did.
  const bool navSwapped = mappedInput.isNavDirectionSwapped();
  const auto prevKey = navSwapped ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextKey = navSwapped ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;

  // A session that hit the reconnect budget lands in Failed — surface it
  // unless the round trip already failed for its own reason. Note mode never
  // opened a session: skip the state machine entirely (it would only report
  // stale/idle states), while Back, capture and save handling below still run.
  // A relay round does not depend on the OpenClaw link: its failures are the
  // relay's own, and the shared session may be down for the whole round.
  if (!noteContext_.valid && !questionInput_ && !relayRound_ && state_ != State::Failed) {
    if (session_.state() == OpenClaw::Session::State::Failed) {
      fail(session_.reason() != nullptr ? session_.reason() : tr(STR_OPENCLAW_DISCONNECTED));
      // fall through: Back still works below
    } else if (session_.reconnecting() && (state_ == State::Idle || state_ == State::Answer)) {
      state_ = State::Idle;
      haveAnswer_ = false;
    }
  }

  // Back: every state above unwinds one level on the press edge; only the
  // final "leave the activity" step is deferred to the release edge — the
  // reader underneath also reads a Back release as "go home"
  // (EpubReaderActivity.cpp, GO_HOME_MS), so finishing on the press edge
  // would hand it the orphaned release and close the book (EpubReaderMenu's
  // rule, same pattern as AskAiActivity::backArmed_).
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // An armed delete backs out without leaving the list first.
    if (state_ == State::NoteList && noteDeleteArmed_) {
      noteDeleteArmed_ = false;
      requestUpdate();
      return;
    }
    // The mark picker backs out to the idle note screen, not out of the
    // activity: Back there still leaves the way it always did.
    if (state_ == State::NoteQuick) {
      state_ = State::Idle;
      requestUpdate();
      return;
    }
    // The chat preset picker unwinds to the idle chat screen the same way.
    if (state_ == State::ChatPresets) {
      state_ = State::Idle;
      requestUpdate();
      return;
    }
    // Note browser levels unwind one step before leaving the activity.
    if (state_ == State::NoteView) {
      state_ = State::NoteList;
      requestUpdate();
      return;
    }
    if (state_ == State::NoteList) {
      state_ = State::NoteSaved;
      requestUpdate();
      return;
    }
    // History unwinds one step before leaving the activity.
    if (state_ == State::HistoryDay) {
      state_ = State::HistoryList;
      // The cursor never moved while the turns were open (historyDayIndex_ is
      // both), so the highlight is already on the day just closed.
      historyScroll_ = 0;
      requestUpdate();
      return;
    }
    if (state_ == State::HistoryList) {
      // An armed delete backs out without touching the file (same pattern as
      // the note list above).
      if (historyDeleteArmed_) {
        historyDeleteArmed_ = false;
        requestUpdate();
        return;
      }
      state_ = State::Idle;
      requestUpdate();
      return;
    }
    // A settled (or failed) summarize round goes back to the list it was
    // started from instead of leaving the activity — the summary is browsed
    // from there, like any other turn. Cursor and listing are untouched by
    // the round trip, so the view is exactly as it was.
    if ((state_ == State::Answer || state_ == State::Failed) && cameFromHistory_) {
      cameFromHistory_ = false;
      state_ = State::HistoryList;
      historyScroll_ = 0;
      requestUpdate();
      return;
    }
    backArmed_ = true;
    return;
  }
  if (backArmed_ && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    backArmed_ = false;
    finish();
    return;
  }

  // ── History browser input ────────────────────────────────────
  if (state_ == State::HistoryList) {
    // One action per front key, day selection on the side keys: Back leaves
    // (an armed delete backs out instead), front-Left deletes the highlighted
    // day (two presses, armed like the note list), front-Right asks the
    // gateway to summarize it, Confirm opens the day's turns. Hints in
    // render() are drawn on the same keys (mapLabels).
    const bool hasDay = historyDayCount_ > 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (historyDayIndex_ > 0) {
        historyDayIndex_--;
        historyDeleteArmed_ = false;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (hasDay && historyDayIndex_ + 1 < historyDayCount_) {
        historyDayIndex_++;
        historyDeleteArmed_ = false;
        requestUpdate();
      }
    } else if (hasDay && mappedInput.wasPressed(prevKey)) {
      // Delete is two presses of the same key: the first only arms it (the
      // hints and the confirmation line change), the second removes the
      // highlighted day file.
      if (historyDeleteArmed_) {
        deleteHistoryDay();
      } else {
        historyDeleteArmed_ = true;
        requestUpdate();
      }
    } else if (hasDay && mappedInput.wasPressed(nextKey)) {
      sendHistorySummary();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (hasDay) {
        historyDeleteArmed_ = false;
        if (loadHistoryDay(historyDayIndex_)) {
          state_ = State::HistoryDay;
        }
        requestUpdate();
      }
    }
    return;
  }
  if (state_ == State::HistoryDay) {
    // Side Up/Down page the window over the selected turn's text — an
    // assistant reply is several screens long and line-stepping buried the
    // rest behind dozens of presses. Front Left/Right step to the
    // previous/next turn and start from the top again. The hints in render()
    // split the same way.
    const size_t page = historyPageLines_ > 0 ? historyPageLines_ : 1;
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      historyScroll_ = historyScroll_ > page ? historyScroll_ - page : 0;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      historyScroll_ += page;  // clamped by drawTextWindow() when the panel renders
      requestUpdate();
    } else if (mappedInput.wasPressed(prevKey) && historySelected_ > 0) {
      historySelected_--;
      historyScroll_ = 0;
      requestUpdate();
    } else if (mappedInput.wasPressed(nextKey) && historySelected_ + 1 < historyTurnCount_) {
      historySelected_++;
      historyScroll_ = 0;
      requestUpdate();
    }
    return;
  }

  // Side Up/Down scroll a settled reply: it is normally longer than one
  // screen, and without this the panel just dropped the rest behind an
  // ellipsis. The offsets are clamped when render() draws the window.
  if (state_ == State::Answer) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (answerScroll_ > 0) {
        answerScroll_--;
      } else {
        // At the top the reply cannot scroll further up, and there is no
        // other way back to Idle from a settled answer — so Up opens the
        // preset picker there (hinted as 预设提问 at scroll 0, 上 above it).
        openChatPresets();
      }
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      answerScroll_++;
      requestUpdate();
      return;
    }
  }

  // History is opened by the key the "History" hint is painted over: the hint
  // is passed as the next label, so it belongs to nextKey.  Opens from any
  // quiet state (Idle / Answer / Failed). Chat-only: note mode advertises no
  // such key.
  if (!noteContext_.valid && !questionInput_ && mappedInput.wasPressed(nextKey) &&
      (state_ == State::Idle || state_ == State::Answer || state_ == State::Failed)) {
    openHistory();
    return;
  }

  // Chat presets: side Up from the quiet chat screen opens the preset picker.
  // The three front keys are already bound there (talk / send-mode / history),
  // and side Up is free in Idle and Failed (Answer spends it scrolling the
  // reply — a new question from there starts with the talk key). Idle/Failed
  // are the same quiet states the history key opens from.
  if (!noteContext_.valid && !questionInput_ && mappedInput.wasPressed(MappedInputManager::Button::Up) &&
      (state_ == State::Idle || state_ == State::Failed)) {
    openChatPresets();
    return;
  }

  // The send mode toggles wherever its hint chip is shown. Chat-only as well.
  if (!noteContext_.valid && !questionInput_ && mappedInput.wasPressed(prevKey) &&
      (state_ == State::Idle || state_ == State::Answer || state_ == State::Failed || state_ == State::Review)) {
    const bool turningAuto = SETTINGS.voiceAutoSend == 0;
    SETTINGS.voiceAutoSend = turningAuto ? 1 : 0;
    // Value changed by definition; one settings write per user action is the
    // accepted pattern for an explicit toggle (same as the BLE switch).
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  // Question mode: the front-Left slot (painted with STR_Q_CUSTOM below) opens
  // the keyboard — type the question instead of speaking it. Idle/Failed only,
  // mirroring the note mode quick-entry keys.
  if (questionInput_ && mappedInput.wasPressed(prevKey) && (state_ == State::Idle || state_ == State::Failed)) {
    openNoteEditor();
    return;
  }

  if (state_ == State::Listening) {
    const Voice::Recorder::Event captureEvent = recorder_.poll();
    if (relayRound_) {
      pumpRelayCapture();  // streams what poll() just appended, once Active
      // The error-event flag first: a server error also parks the client at
      // Failed, and its reason mapping beats the transport fallback's generic
      // message (transport failures leave relayError_ false).
      if (relayError_) {
        handleRelayErrorEvent();
        return;
      }
      if (relay_ != nullptr && relay_->state() == VoiceRelay::Client::State::Failed) {
        relayTransportFailed();
        return;
      }
    }
    switch (captureEvent) {
      case Voice::Recorder::Event::Recording:
        return;  // no repaint, no input: the capture owns the loop
      case Voice::Recorder::Event::Complete:
        if (relayRound_) {
          // Audio stays in the capture buffer: the relay Sending state flushes
          // any tail, sends audio.end, and can still hand the same capture to
          // the legacy fallback. recorder_.close() waits for the settle.
          state_ = State::Sending;
          relayEndMs_ = millis();
          lastWaitShownSec_ = 0;
          requestUpdate();
        } else {
          startTranscribing();
        }
        return;
      case Voice::Recorder::Event::NoAudio:
        if (relayRound_) {
          relayClose();
          relayRound_ = false;
        }
        fail(tr(STR_MIC_UNAVAILABLE));
        return;
      case Voice::Recorder::Event::ReadFailed:
      case Voice::Recorder::Event::NotStarted:
        if (relayRound_) {
          relayClose();
          relayRound_ = false;
        }
        fail(tr(STR_MIC_READ_FAILED));
        return;
    }
    return;
  }

  if (state_ == State::Transcribing) {
    runTranscription();
    return;
  }

  if (state_ == State::Review) {
    // The transcript is parked; Confirm commits it to the wire.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      sendToOpenClaw();
    }
    return;
  }

  if (state_ == State::Sending) {
    if (relayRound_) {
      loopRelaySending();
      return;
    }
    // Transcript ready, chat.send not yet on the wire (the user spoke while
    // the session was still handshaking). oweSend_ says so — not
    // session_.outcome(), which still reports the previous round and would
    // leave this one waiting on a send that never goes out.
    if (oweSend_) {
      // Same guard as AskAiActivity's Connecting branch: Connected is stale
      // while the reconnect runs, and writing to the closed socket fails the
      // send instead of queueing it for when the link returns.
      if (session_.state() == OpenClaw::Session::State::Connected && !session_.reconnecting()) sendPending();
      return;
    }
    if (session_.awaitingReply()) {
      // Keep the waiting line alive: refresh exactly when the number the
      // screen would print changes, so the counter steps 0 → 1 → 2 … and
      // never repaints the same second twice.
      const uint32_t shownSec = session_.awaitingMs() / 1000;
      if (shownSec != lastWaitShownSec_) {
        lastWaitShownSec_ = shownSec;
        requestUpdate();
      }
      return;
    }
    // Reply settled (Sent/AgentFailed/NoReply/Aborted/Failed) → show it.
    if (session_.replySize() > 0 && session_.outcome() == OpenClaw::Session::SendOutcome::Sent) {
      haveAnswer_ = true;
      answerScroll_ = 0;  // a new reply starts at its first line
      state_ = State::Answer;
      if (summaryRound_) {
        // The summary is about the history, not part of it: appending would
        // write a fake "Summary of <date>" turn into the day file.
        summaryRound_ = false;
        pendingText_ = nullptr;
      } else {
        recordHistoryTurn();
      }
      requestUpdate();
    } else if (session_.state() == OpenClaw::Session::State::Connected) {
      // A dropped socket leaves state_ at Connected and only arms the
      // reconnect, so "the link is gone" has to be read from reconnecting()
      // and not from the state — otherwise a closed socket is reported as a
      // gateway that never answered.
      if (session_.reconnecting()) {
        fail(tr(STR_OPENCLAW_DISCONNECTED));
      } else {
        // Settled without a usable reply; the reason is in the session.
        fail(session_.lastError() != nullptr ? tr(STR_OPENCLAW_AGENT_FAILED) : tr(STR_OPENCLAW_NO_REPLY));
      }
    }
    // Otherwise the link dropped mid-round trip: the reconnect / session-
    // Failed handling at the top of loop() decides, and the gateway line on
    // this screen says which of the two is running.
    return;
  }

  // Note mode has its own settled screen (STR_NOTE_HINT / Back); the chat
  // Confirm handlers below never run while it is showing.
  if (state_ == State::NoteSaved) {
    if (mappedInput.wasPressed(prevKey)) {
      // Left opens this book's bookmark list (the chat history browser's slot;
      // that one stays chat-only).
      openNoteList();
    }
    return;
  }

  // The 书签列表 hint is painted on the idle/failed note screen as well, so
  // the key has to work from there — browsing is not save-exclusive.
  if (noteContext_.valid && mappedInput.wasPressed(prevKey) && (state_ == State::Idle || state_ == State::Failed)) {
    openNoteList();
    return;
  }
  // The other half of the note-mode key map: the front-Right key, which the
  // chat screen spends on history and this one has no use for, opens the mark
  // picker. Both keys are painted with these labels on the same screen.
  if (noteContext_.valid && mappedInput.wasPressed(nextKey) && (state_ == State::Idle || state_ == State::Failed)) {
    openNoteQuick();
    return;
  }

  // ── Quick mark picker ───────────────────────────────────────────
  if (state_ == State::NoteQuick) {
    // Side Up/Down and front Left/Right all move the highlight, Confirm writes
    // the selected mark, Back returns to the idle screen (handled above).
    // Confirm commits here as it does everywhere else; the talk key is
    // deliberately not bound on this screen, so a mark can never start a
    // capture by accident.
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) || mappedInput.wasPressed(prevKey)) {
      if (quickSelected_ > 0) quickSelected_--;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) || mappedInput.wasPressed(nextKey)) {
      if (quickSelected_ + 1 < QUICK_TAG_COUNT) quickSelected_++;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      saveQuickNote();
    }
    return;
  }

  // ── Chat preset picker ──────────────────────────────────────────
  if (state_ == State::ChatPresets) {
    // Side Up/Down move the highlight, Confirm sends the highlighted question
    // now, front-Left opens it in the keyboard for editing first. Back returns
    // to Idle (handled above); the talk key is deliberately unbound so a
    // picker pass can never start a capture.
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (presetSelected_ > 0) presetSelected_--;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (presetSelected_ + 1 < CHAT_PRESET_COUNT) presetSelected_++;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      sendPresetDirect();
    } else if (mappedInput.wasPressed(prevKey)) {
      openPresetEditor();
    }
    return;
  }

  // ── Note browser input (list level) ─────────────────────────────
  if (state_ == State::NoteList) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) && noteSelected_ > 0) {
      noteSelected_--;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) && noteSelected_ + 1 < noteEntryCount_) {
      noteSelected_++;
      requestUpdate();
    } else if (mappedInput.wasPressed(nextKey) && noteEntryCount_ > 0) {
      // Delete is two presses of the same key: the first only arms it (the
      // hints and the confirmation line change), the second rewrites the file.
      if (noteDeleteArmed_) {
        deleteSelectedNote();
      } else {
        noteDeleteArmed_ = true;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && noteEntryCount_ > 0) {
      noteViewScroll_ = 0;
      state_ = State::NoteView;
      requestUpdate();
    }
    return;
  }
  if (state_ == State::NoteView) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (noteViewScroll_ > 0) noteViewScroll_--;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      noteViewScroll_++;  // clamped by drawTextWindow() at render time
      requestUpdate();
    } else if (mappedInput.wasPressed(nextKey) && noteEntryCount_ > 0 && noteSelected_ < noteEntryCount_) {
      // Hand the note's stored position back to the reader, which is still on
      // the activity stack under us (setNoteContext was armed by it).
      const auto& entry = noteEntries_[noteSelected_];
      LOG_INF("VOICE", "jump to note: spine %u page %u", static_cast<unsigned>(entry.spineIndex),
              static_cast<unsigned>(entry.pageNumber));
      setResult(ProgressChangeResult{entry.spineIndex, entry.pageNumber});
      finish();
      return;
    }
    return;
  }

  if (!mappedInput.wasPressed(MappedInputManager::Button::Confirm)) return;

  switch (state_) {
    case State::Idle:
    case State::Answer:
    case State::Failed:
      // A fresh round trip from any settled state.
      startRecording();
      break;
    case State::Listening:
    case State::Transcribing:
    case State::Sending:
    case State::Review:
    case State::HistoryList:
    case State::HistoryDay:
    case State::NoteSaved:
    case State::NoteQuick:
    case State::ChatPresets:
      break;  // unreachable: handled above
  }
}

void VoiceActivity::render(RenderLock&&) {
  LOG_DBG("VOICE", "render state=%d noteMode=%d", static_cast<int>(state_), noteContext_.valid ? 1 : 0);
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int left = metrics.contentSidePadding;

  GUI.drawHeader(
      renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
      noteContext_.valid ? tr(STR_VOICE_BOOKMARK) : (questionInput_ ? I18N.get(questionScreenTitle_) : tr(STR_VOICE)));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textWidth = pageWidth - 2 * left;
  char buf[96];

  switch (state_) {
    case State::Idle: {
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_VOICE_HINT), true, EpdFontFamily::BOLD);
      y += metrics.verticalSpacing * 2;
      // Session progress on the same screen: the user sees the connect (or
      // reconnect) happen.
      if (noteContext_.valid) {
        snprintf(buf, sizeof(buf), "%s", tr(STR_VOICE_BOOKMARK));
      } else if (questionInput_) {
        // The action label, not the header again: the header already shows
        // questionScreenTitle_, and repeating it here read as a glitch.
        snprintf(buf, sizeof(buf), "%s", I18N.get(questionTitle_));
      } else if (session_.state() == OpenClaw::Session::State::Connected && !session_.reconnecting()) {
        snprintf(buf, sizeof(buf), "%s · %s", tr(STR_OPENCLAW_CONNECTED), session_.status());
      } else {
        snprintf(buf, sizeof(buf), "%s", session_.status());
      }
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, buf, textWidth, 2)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      if (noteContext_.valid || questionInput_) {
        break;  // no session line: nothing is dialling in note/question mode
      }
      if (session_.reconnecting()) {
        renderer.drawText(UI_10_FONT_ID, left, y + metrics.verticalSpacing, tr(STR_OPENCLAW_DISCONNECTED));
      } else if (session_.reason() != nullptr) {
        renderer.drawText(UI_10_FONT_ID, left, y + metrics.verticalSpacing, session_.reason());
      }
      break;
    }

    case State::Listening: {
      // Painted by startRecording() before MIC.begin(): the capture owns the
      // panel for the rest of this state, so the stage row, the mic icon and
      // the gateway state all have to be on screen now.
      drawPipeline(left, y);
      y += lineHeight + metrics.verticalSpacing;
      renderer.drawIcon(VoiceIcon, left, y, 32, 32);
      renderer.drawText(UI_12_FONT_ID, left + 40, y + 8, tr(STR_VOICE_LISTENING), true, EpdFontFamily::BOLD);
      y += 32 + metrics.verticalSpacing * 2;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, tr(STR_VOICE_RECORD_HINT), textWidth, 2)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      if (const char* gw = (noteContext_.valid || questionInput_)
                               ? nullptr
                               : (relayRound_ ? tr(STR_VOICE_RELAY) : gatewayStatus())) {
        const int gwY = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight - metrics.verticalSpacing;
        renderer.drawText(UI_10_FONT_ID, left, gwY, gw, true, EpdFontFamily::BOLD);
      }
      break;
    }

    case State::Transcribing: {
      drawPipeline(left, y);
      y += lineHeight + metrics.verticalSpacing;
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_STT_TRANSCRIBING), true, EpdFontFamily::BOLD);
      y += metrics.verticalSpacing;
      if (const char* gw = (noteContext_.valid || questionInput_) ? nullptr : gatewayStatus()) {
        renderer.drawText(UI_10_FONT_ID, left, y + lineHeight, gw);
      }
      break;
    }

    case State::Sending: {
      drawPipeline(left, y);
      y += lineHeight + metrics.verticalSpacing;
      // Elapsed / deadline read-out — the one thing that changes between the
      // per-second repaints, so a slow agent turn reads as progress instead
      // of a frozen screen. Nothing to count while the frame is still waiting
      // for the link, so that case shows the plain line.
      char waiting[72];
      if (relayRound_) {
        const uint32_t waitedMs = relayEndMs_ != 0 ? millis() - relayEndMs_ : 0;
        snprintf(waiting, sizeof(waiting), "%s %u/%us", tr(STR_OPENCLAW_WAITING),
                 static_cast<unsigned>(waitedMs / 1000),
                 static_cast<unsigned>(VoiceRelay::Client::DONE_TIMEOUT_MS / 1000));
      } else if (session_.awaitingReply()) {
        snprintf(waiting, sizeof(waiting), "%s %u/%us", tr(STR_OPENCLAW_WAITING),
                 static_cast<unsigned>(session_.awaitingMs() / 1000),
                 static_cast<unsigned>(OpenClaw::Session::CHAT_TIMEOUT_MS / 1000));
      } else {
        snprintf(waiting, sizeof(waiting), "%s", tr(STR_OPENCLAW_WAITING));
      }
      renderer.drawText(UI_10_FONT_ID, left, y, waiting, true, EpdFontFamily::BOLD);
      y += metrics.verticalSpacing * 2;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, transcript_, textWidth, 2)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      if (const char* gw = relayRound_ ? tr(STR_VOICE_RELAY) : gatewayStatus()) {
        const int gwY = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight - metrics.verticalSpacing;
        renderer.drawText(UI_10_FONT_ID, left, gwY, gw);
      }
      break;
    }

    case State::Answer: {
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_VOICE_YOU), true, EpdFontFamily::BOLD);
      y += lineHeight;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, transcript_, textWidth, 3)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      y += metrics.verticalSpacing;
      const int labelY = y;
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_VOICE_ASSISTANT), true, EpdFontFamily::BOLD);
      y += lineHeight;
      const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
      int maxLines = lineHeight > 0 ? (bottom - y) / lineHeight : 3;
      if (maxLines < 1) maxLines = 1;
      // Body text stays clear of the right margin, where the side Up/Down
      // scroll hints are drawn. What does not fit is scrolled, not dropped:
      // the "first-last/total" read-out says where the window is. The reply
      // source follows the round (relay accumulation vs. shared session).
      const int bodyWidth = textWidth - metrics.sideButtonHintsWidth;
      const size_t total = drawTextWindow(UI_10_FONT_ID, relayRound_ ? relayReply_ : session_.replyText(), left, y,
                                          bodyWidth, maxLines, answerScroll_);
      if (total > static_cast<size_t>(maxLines)) {
        const size_t lastShown = std::min(total, answerScroll_ + static_cast<size_t>(maxLines));
        char pos[24];
        snprintf(pos, sizeof(pos), "%u-%u/%u", static_cast<unsigned>(answerScroll_ + 1),
                 static_cast<unsigned>(lastShown), static_cast<unsigned>(total));
        renderer.drawText(UI_10_FONT_ID, pageWidth - left - renderer.getTextWidth(UI_10_FONT_ID, pos), labelY, pos);
      }
      break;
    }

    case State::Review: {
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_VOICE_YOU), true, EpdFontFamily::BOLD);
      y += lineHeight;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, transcript_, textWidth, 4)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      // Send mode: the toggle key's own hint (previousLabel below) is the
      // always-visible chip — nothing extra is drawn in the panel.
      break;
    }

    case State::HistoryList: {
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_VOICE_HISTORY), true, EpdFontFamily::BOLD);
      y += lineHeight + metrics.verticalSpacing;
      if (historyDayCount_ == 0) {
        renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_VOICE_HISTORY_EMPTY));
        break;
      }
      // The list owns the whole panel: the two actions sit on the front keys
      // (hint bar below) and side Up/Down picks the day, so the window pages
      // with the highlight — the 128-day listing window can exceed the rows
      // that fit on screen, and a highlight scrolled out of view would leave
      // every key acting on an invisible row.
      const int armedHeight = historyDeleteArmed_ ? lineHeight + metrics.verticalSpacing : 0;
      const int listBottom =
          renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - armedHeight;
      int rows = lineHeight > 0 ? (listBottom - y) / lineHeight : 0;
      if (rows > 8) rows = 8;  // fits either orientation at UI_10 line height
      if (rows < 1) rows = 1;
      const size_t sel = historyDayIndex_;
      const size_t pageStart = (sel / static_cast<size_t>(rows)) * static_cast<size_t>(rows);
      const int64_t today = todayHistoryDays();
      char label[24];
      for (size_t i = pageStart; i < historyDayCount_ && i < pageStart + static_cast<size_t>(rows); i++) {
        ChatHistoryFormat::historyDayRowLabel(label, sizeof(label), historyDays_[i], today, tr(STR_TODAY),
                                              tr(STR_YESTERDAY), tr(STR_DAY_BEFORE));
        const bool selected = i == sel;
        if (selected) {
          renderer.fillRect(left - 4, y - 2, textWidth, lineHeight);
        }
        renderer.drawText(UI_10_FONT_ID, left, y, label, !selected, EpdFontFamily::REGULAR);
        y += lineHeight;
      }
      if (historyDeleteArmed_) {
        renderer.drawText(UI_10_FONT_ID, left, listBottom, tr(STR_CONFIRM_DELETE_HISTORY));
      }
      break;
    }

    case State::HistoryDay: {
      char label[24];
      ChatHistory::dayLabel(historyDays_[historyDayIndex_], label, sizeof(label));
      renderer.drawText(UI_10_FONT_ID, left, y, label, true, EpdFontFamily::BOLD);
      const int dayRowY = y;
      y += lineHeight + metrics.verticalSpacing;
      if (historyEntryCount_ == 0 || historyTurnCount_ == 0) {
        renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_VOICE_NO_HISTORY_DAY));
        break;
      }
      // One selected turn = the user's line and the assistant's reply behind
      // it, composed into ONE scroll window so a Q&A pair shares the page and
      // the "n/N" position counts turns, not entries. Turn starts are user
      // entries (a "user only" append starts its own turn); index 0 also
      // starts one when the pool's tail window begins on an assistant reply.
      size_t start = 0;
      for (size_t t = 0; t < historySelected_; t++) {
        start++;
        while (start < historyEntryCount_ && !historyEntries_[start].isUser) start++;
        if (start >= historyEntryCount_) {
          start = historyEntryCount_ - 1;  // defensive: turnCount_ bounds t
          break;
        }
      }
      const auto& first = historyEntries_[start];
      const bool hasReply = start + 1 < historyEntryCount_ && !historyEntries_[start + 1].isUser;
      const auto& second = historyEntries_[hasReply ? start + 1 : start];
      char pos[24];
      snprintf(pos, sizeof(pos), "%u/%u", static_cast<unsigned>(historySelected_ + 1),
               static_cast<unsigned>(historyTurnCount_));
      renderer.drawText(UI_10_FONT_ID, pageWidth - left - renderer.getTextWidth(UI_10_FONT_ID, pos), dayRowY, pos);
      const int labelY = y;
      // Tags lead each half inline, each prefixed with that entry's HH:MM so
      // the turn reads as a full "YYYY-MM-DD/HH:MM" against the day label
      // above (dayLabel() -> "2026-10-08"). transcript (≤ CHAT_MESSAGE_CAP) +
      // reply (≤ CHAT_BUF_CAP) + tags always fit HISTORY_LINE_SIZE with room
      // for two six-byte stamps; the raw fallback keeps a mid-codepoint cut
      // away from drawText() if a corrupted entry ever breaks that bound.
      const char* tag1 = first.isUser ? tr(STR_VOICE_YOU) : tr(STR_VOICE_ASSISTANT);
      const char* tag2 = second.isUser ? tr(STR_VOICE_YOU) : tr(STR_VOICE_ASSISTANT);
      char time1[6] = "";
      char time2[6] = "";
      const bool haveTime1 = ChatHistoryFormat::formatTimeForOffset(
          time1, sizeof(time1), static_cast<int64_t>(first.tsMs), SETTINGS.clockUtcOffsetQ);
      const bool haveTime2 = ChatHistoryFormat::formatTimeForOffset(
          time2, sizeof(time2), static_cast<int64_t>(second.tsMs), SETTINGS.clockUtcOffsetQ);
      const int n =
          hasReply ? snprintf(historyLine_, HISTORY_LINE_SIZE, "%s%s %s\n%s%s %s", haveTime1 ? time1 : "", tag1,
                              historyPool_ + first.textOff, haveTime2 ? time2 : "", tag2, historyPool_ + second.textOff)
                   : snprintf(historyLine_, HISTORY_LINE_SIZE, "%s%s %s", haveTime1 ? time1 : "", tag1,
                              historyPool_ + first.textOff);
      bool composed = n > 0 && static_cast<size_t>(n) < HISTORY_LINE_SIZE;
      if (!composed) {
        LOG_ERR("VOICE", "turn compose overflow (%d bytes), raw fallback", n);
      }
      const char* body = composed ? historyLine_ : historyPool_ + second.textOff;
      const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
      int maxLines = lineHeight > 0 ? (bottom - y) / lineHeight : 5;
      if (maxLines < 1) maxLines = 1;
      historyPageLines_ = static_cast<size_t>(maxLines);  // page step for loop()'s Up/Down
      const int bodyWidth = textWidth - metrics.sideButtonHintsWidth;
      const size_t total = drawTextWindow(UI_10_FONT_ID, body, left, y, bodyWidth, maxLines, historyScroll_);
      if (total > static_cast<size_t>(maxLines)) {
        const size_t lastShown = std::min(total, historyScroll_ + static_cast<size_t>(maxLines));
        char lines[24];
        snprintf(lines, sizeof(lines), "%u-%u/%u", static_cast<unsigned>(historyScroll_ + 1),
                 static_cast<unsigned>(lastShown), static_cast<unsigned>(total));
        renderer.drawText(UI_10_FONT_ID, pageWidth - left - renderer.getTextWidth(UI_10_FONT_ID, lines), labelY, lines);
      }
      break;
    }

    case State::NoteSaved: {
      // Settled note screen: one status, the transcript, Back/Left hints. The
      // next onEnter() re-arms the mode.
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_NOTE_SAVED), true, EpdFontFamily::BOLD);
      y += lineHeight + metrics.verticalSpacing;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, transcript_, textWidth, 4)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      break;
    }

    case State::NoteList: {
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_VOICE_BOOKMARK), true, EpdFontFamily::BOLD);
      y += lineHeight + metrics.verticalSpacing;
      if (noteEntryCount_ == 0) {
        renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_NOTE_LIST_EMPTY));
        break;
      }
      // Window follows the selection page-wise, but the page is whatever the
      // panel can actually show — the old hard 8 rows per page left half a
      // portrait screen blank and forced a page turn for lists that already
      // fit. The armed-delete line gets its row reserved up front (the same
      // way HistoryList does), so drawing it can never land on the last note.
      const int armedHeight = noteDeleteArmed_ ? lineHeight + metrics.verticalSpacing : 0;
      const int listBottom =
          renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - armedHeight;
      int rows = lineHeight > 0 ? (listBottom - y) / lineHeight : 8;
      if (rows < 1) rows = 1;
      const size_t window = static_cast<size_t>(rows);
      const size_t first = (noteSelected_ / window) * window;
      for (size_t i = first; i < noteEntryCount_ && i < first + window; i++) {
        // One line per note: wall-clock stamp + text head (entry text starts
        // at textOff in the pool, NUL-terminated by load()). The old ordinal
        // prefix is gone — the stamp is what makes a row traceable and the
        // row is addressed by highlight anyway. An unsynced clock keeps the
        // plain prefix instead of printing 1970-01-20.
        const char* text = historyPool_ + noteEntries_[i].textOff;
        char stamp[ChatHistoryFormat::STAMP_SIZE];
        std::string row;
        if (ChatHistoryFormat::formatStampForOffset(stamp, sizeof(stamp), static_cast<int64_t>(noteEntries_[i].tsMs),
                                                    SETTINGS.clockUtcOffsetQ)) {
          row.assign(stamp);
          row += ' ';
          row += text;
        } else {
          row = std::to_string(static_cast<unsigned>(i + 1)) + ". " + text;
        }
        // Cut to the panel width, not to a byte count: a fixed byte slice can
        // end inside a multi-byte character and draw a missing-glyph box.
        if (renderer.getTextWidth(UI_10_FONT_ID, row.c_str()) > textWidth) {
          row.resize(renderer.fitPrefixLen(UI_10_FONT_ID, row, textWidth));
        }
        const bool selected = i == noteSelected_;
        if (selected) {
          renderer.fillRect(left - 4, y - 2, textWidth, lineHeight);
        }
        renderer.drawText(UI_10_FONT_ID, left, y, row.c_str(), !selected, EpdFontFamily::REGULAR);
        y += lineHeight;
      }
      if (noteDeleteArmed_) {
        // At listBottom, i.e. inside the row reserved above — not after the
        // last note, which now sits exactly at the bottom of the panel.
        renderer.drawText(UI_10_FONT_ID, left, listBottom, tr(STR_CONFIRM_DELETE_NOTE), true, EpdFontFamily::BOLD);
      }
      break;
    }

    case State::NoteQuick: {
      // Quick marks ("快捷书签"): opened from the idle note screen by the
      // front-Right key the hint names. Six fixed tags plus a preview of the
      // line OK stores, and nothing here touches the mic, the STT client or the
      // session — saveQuickNote() goes straight to the note append, so a mark
      // costs no audio upload and no agent turn.
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_NOTE_QUICK), true, EpdFontFamily::BOLD);
      y += lineHeight;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, tr(STR_NOTE_QUICK_HINT), textWidth, 1)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      y += metrics.verticalSpacing;
      for (size_t i = 0; i < QUICK_TAG_COUNT; i++) {
        const bool selected = i == quickSelected_;
        char row[32];
        snprintf(row, sizeof(row), "%u. %s", static_cast<unsigned>(i + 1), I18N.get(QUICK_TAGS[i]));
        if (selected) {
          renderer.fillRect(left - 4, y - 2, textWidth, lineHeight);
        }
        renderer.drawText(UI_10_FONT_ID, left, y, row, !selected, EpdFontFamily::REGULAR);
        y += lineHeight;
      }
      // The composed line, not the bare excerpt: moving the highlight changes
      // its head, so this answers "存进去是哪条" instead of showing a tag with
      // an unexplained quote under it. Only the rows the list left above the
      // hint bar are used, and the side scroll labels claim the right margin.
      const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
      const int capLines = lineHeight > 0 ? (bottom - y) / lineHeight - 1 : 0;
      if (noteExcerpt_[0] != '\0' && capLines > 0) {
        char preview[256];
        snprintf(preview, sizeof(preview), "%s%s%s", I18N.get(QUICK_TAGS[quickSelected_]), tr(STR_NOTE_SEP),
                 noteExcerpt_);
        y += metrics.verticalSpacing;
        const int bodyWidth = textWidth - metrics.sideButtonHintsWidth;
        for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, preview, bodyWidth, capLines)) {
          renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
          y += lineHeight;
        }
      }
      break;
    }

    case State::ChatPresets: {
      // Preset questions: same row layout as the quick-mark picker (fill
      // highlight, numbered rows). Confirm sends, front-Left edits — the
      // hint bar below names both keys.
      renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_CHAT_PRESETS), true, EpdFontFamily::BOLD);
      y += lineHeight + metrics.verticalSpacing;
      for (size_t i = 0; i < CHAT_PRESET_COUNT; i++) {
        const bool selected = i == presetSelected_;
        char row[96];
        snprintf(row, sizeof(row), "%u. %s", static_cast<unsigned>(i + 1), I18N.get(CHAT_PRESETS[i]));
        if (selected) {
          renderer.fillRect(left - 4, y - 2, textWidth, lineHeight);
        }
        renderer.drawText(UI_10_FONT_ID, left, y, row, !selected, EpdFontFamily::REGULAR);
        y += lineHeight;
      }
      break;
    }

    case State::NoteView: {
      const auto& entry = noteEntries_[noteSelected_];
      // Wall-clock stamp first: the list row shows the same one, and both
      // come from the NTF1 tsMs stored with the note. Costs one body line.
      char stamp[ChatHistoryFormat::STAMP_SIZE];
      if (ChatHistoryFormat::formatStampForOffset(stamp, sizeof(stamp), static_cast<int64_t>(entry.tsMs),
                                                  SETTINGS.clockUtcOffsetQ)) {
        renderer.drawText(UI_10_FONT_ID, left, y, stamp, true, EpdFontFamily::BOLD);
        y += lineHeight;
      }
      // Position line: where in the book this note was spoken.
      char pos[48];
      snprintf(pos, sizeof(pos), "ch %u · p %u/%u", static_cast<unsigned>(entry.spineIndex),
               static_cast<unsigned>(entry.pageNumber + 1), static_cast<unsigned>(entry.pageCount));
      renderer.drawText(UI_10_FONT_ID, left, y, pos, true, EpdFontFamily::BOLD);
      y += lineHeight;
      const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
      int maxLines = lineHeight > 0 ? (bottom - y) / lineHeight : 5;
      if (maxLines < 1) maxLines = 1;
      const int bodyWidth = textWidth - metrics.sideButtonHintsWidth;
      drawTextWindow(UI_10_FONT_ID, historyPool_ + entry.textOff, left, y, bodyWidth, maxLines, noteViewScroll_);
      break;
    }

    case State::Failed: {
      // Gateway pairing bread ("auth failed" + `openclaw devices approve …`)
      // belongs to the legacy MiMo + OpenClaw service only. With the relay on,
      // this screen is reached before any conversation and the command is
      // noise for a relay user, so the pair of lines is shown only when the
      // relay preference is off (SettingsList: STR_VOICE_RELAY).
      const bool pairingPending = session_.approveCommand() != nullptr;
      const bool showPairing = pairingPending && SETTINGS.voiceRelayEnabled == 0;
      if (!pairingPending || showPairing) {
        renderer.drawText(UI_10_FONT_ID, left, y, failReason_ != nullptr ? failReason_ : tr(STR_STT_FAILED), true,
                          EpdFontFamily::BOLD);
        y += metrics.verticalSpacing * 2;
      }
      if (haveTranscript_) {
        for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, transcript_, textWidth, 3)) {
          renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
          y += lineHeight;
        }
      } else if (showPairing) {
        // Pairing pending: show the operator command where it will be typed.
        for (const auto& line :
             renderer.wrappedText(UI_10_FONT_ID, session_.approveCommand(), textWidth, 3, EpdFontFamily::BOLD)) {
          renderer.drawText(UI_10_FONT_ID, left, y, line.c_str(), true, EpdFontFamily::BOLD);
          y += lineHeight;
        }
      }
      break;
    }
  }

  // Side Up/Down hints wherever the side keys navigate: they move the
  // hierarchy list and scroll a long turn or reply. Empty labels draw
  // nothing, so this is skipped for the states that do not use them.
  if (state_ == State::Answer) {
    // At the top of a reply the Up key opens the preset picker instead of
    // scrolling (bound in loop()), so the hint names that action there.
    GUI.drawSideButtonHints(renderer, answerScroll_ > 0 ? tr(STR_DIR_UP) : tr(STR_CHAT_PRESETS), tr(STR_DIR_DOWN));
  } else if (state_ == State::HistoryList || state_ == State::HistoryDay || state_ == State::NoteList ||
             state_ == State::NoteView || state_ == State::NoteQuick || state_ == State::ChatPresets) {
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  } else if (state_ == State::Idle && !noteContext_.valid && !questionInput_) {
    // Chat idle: side Up opens the preset picker (Down is unbound) — the one
    // key that is free on this screen. Note/question mode spends the side keys
    // on nothing and has no picker, so they draw no hint there.
    GUI.drawSideButtonHints(renderer, tr(STR_CHAT_PRESETS), "");
  } else if (state_ == State::Failed && !noteContext_.valid && !questionInput_) {
    GUI.drawSideButtonHints(renderer, tr(STR_CHAT_PRESETS), "");
  }

  // Front-key hints. mapLabels() puts previousLabel on the front-Left key and
  // nextLabel on the front-Right key (mirrored when the nav axis is swapped),
  // which is the pairing loop() binds the actions to — the hint and the press
  // have to agree, key for key.
  char modeHint[48];
  if (noteContext_.valid) {
    snprintf(modeHint, sizeof(modeHint), "%s", tr(STR_NOTE_HINT));
  } else if (questionInput_) {
    // Question mode has no mode chip: the front-Left slot is the keyboard
    // key, painted below from STR_Q_CUSTOM.
    modeHint[0] = '\0';
  } else {
    // Bare value, not "Mode: %s": the prefix pushed the chip past the button
    // edge on a narrow portrait panel, and the mode it names is obvious from
    // the two labels alone.
    snprintf(modeHint, sizeof(modeHint), "%s",
             SETTINGS.voiceAutoSend != 0 ? tr(STR_VOICE_MODE_AUTO) : tr(STR_VOICE_MODE_MANUAL));
  }
  const bool quiet = state_ == State::Idle || state_ == State::Answer || state_ == State::Failed;
  const char* confirmLabel = "";
  const char* previousLabel = "";
  const char* nextLabel = "";
  const char* backHint = tr(STR_BACK);
  switch (state_) {
    case State::HistoryDay:
      // Confirm has no action on an open turn, so it gets no hint.
      previousLabel = tr(STR_VOICE_PREV_TURN);
      nextLabel = tr(STR_VOICE_NEXT_TURN);
      break;
    case State::HistoryList: {
      // One action per front key: Back leaves, front-Left deletes the
      // highlighted day, front-Right summarizes it, Confirm opens the day's
      // turns — the hint bar names each key, so the action box it replaced
      // is gone. Armed delete flips Back to Cancel and the delete key to
      // Confirm, the same two-press wording the note list uses.
      if (historyDayCount_ > 0) {
        confirmLabel = tr(STR_OPEN);
        previousLabel = historyDeleteArmed_ ? tr(STR_CONFIRM) : tr(STR_NOTE_DELETE);
        nextLabel = tr(STR_VOICE_SUMMARIZE);
      }
      if (historyDeleteArmed_) backHint = tr(STR_CANCEL);
      break;
    }
    case State::Review:
      confirmLabel = tr(STR_VOICE_SEND);
      previousLabel = modeHint;
      break;
    case State::NoteQuick:
      // Confirm writes the highlighted mark and Back returns to the idle note
      // screen; side Up/Down and front Left/Right all move the highlight, so
      // all four get the Up/Down labels (bottom bar mirrors the side ones).
      confirmLabel = tr(STR_NOTE_QUICK_SAVE);
      previousLabel = tr(STR_DIR_UP);
      nextLabel = tr(STR_DIR_DOWN);
      break;
    case State::ChatPresets:
      // Confirm sends the highlighted preset; front-Left opens it in the
      // keyboard; front-Right is unbound (history stays on the idle screen).
      confirmLabel = tr(STR_PRESET_SEND);
      previousLabel = tr(STR_PRESET_EDIT);
      nextLabel = "";
      break;
    default:
      if (quiet) {
        confirmLabel = tr(STR_VOICE_TALK);
        if (questionInput_) {
          // Left = keyboard, Right unused: question mode has no browser slot.
          previousLabel = I18N.get(questionTitle_);
          nextLabel = "";
        } else {
          previousLabel = noteContext_.valid ? tr(STR_NOTE_LIST) : modeHint;
          // The front-Right slot: history for the chat screen, the mark picker
          // for note mode — both are the "keys that browse instead of speak" slot.
          nextLabel = noteContext_.valid ? tr(STR_NOTE_QUICK) : tr(STR_VOICE_HISTORY);
        }
      }
      break;
  }
  // NoteSaved/NoteList/NoteView draw their own Back hints; the shared
  // mode-hint path would paint slots these states never bind.
  if (state_ == State::NoteSaved) {
    const auto noteLabels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_NOTE_LIST), "");
    GUI.drawButtonHints(renderer, noteLabels.btn1, noteLabels.btn2, noteLabels.btn3, noteLabels.btn4);
  } else if (state_ == State::NoteList || state_ == State::NoteView) {
    // Same pairing rule as above: what loop() binds to nextKey has to be the
    // label painted on that key — delete (twice) from the list, jump from an
    // open note — and Back reads Cancel while a delete is armed.
    const char* backLabel = tr(STR_BACK);
    const char* confirmLabel2 = state_ == State::NoteList ? tr(STR_OPEN) : "";
    const char* nextLabel2 = "";
    if (noteEntryCount_ > 0) {
      if (state_ == State::NoteList) {
        if (noteDeleteArmed_) {
          backLabel = tr(STR_CANCEL);
          nextLabel2 = tr(STR_CONFIRM);
        } else {
          nextLabel2 = tr(STR_NOTE_DELETE);
        }
      } else {
        nextLabel2 = tr(STR_NOTE_JUMP);
      }
    }
    const auto noteLabels = mappedInput.mapLabels(backLabel, confirmLabel2, "", nextLabel2);
    GUI.drawButtonHints(renderer, noteLabels.btn1, noteLabels.btn2, noteLabels.btn3, noteLabels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(backHint, confirmLabel, previousLabel, nextLabel);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
