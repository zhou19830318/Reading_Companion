#include "PomodoroSession.h"

#include <Arduino.h>
#include <Logging.h>
#include <NetBootstrap.h>
#include <esp_random.h>

#include <cstdio>
#include <ctime>

PomodoroSession PomodoroSession::instance;

namespace {

uint32_t nowEpoch() { return NetBootstrap::systemTimeValid() ? static_cast<uint32_t>(time(nullptr)) : 0; }

uint32_t elapsedMinutes(const uint32_t totalMs, const uint32_t leftMs) {
  return totalMs > leftMs ? (totalMs - leftMs) / 60000UL : 0;
}

}  // namespace

void PomodoroSession::start(const uint8_t workMinutes, const uint8_t breakMinutes) {
  if (isActive()) endSession();
  workMinutes_ = workMinutes < MIN_WORK_MINUTES   ? MIN_WORK_MINUTES
                 : workMinutes > MAX_WORK_MINUTES ? MAX_WORK_MINUTES
                                                  : workMinutes;
  breakMinutes_ = breakMinutes < 1 ? 1 : breakMinutes;
  cycleCount_ = 0;
  beginWork();
  LOG_INF("POMO", "start %u/%u min", workMinutes_, breakMinutes_);
}

void PomodoroSession::beginWork() {
  state_ = State::Work;
  paused_ = false;
  pausedRemainingMs_ = 0;
  workStartedAtMs_ = millis();
  workStartedAtEpoch_ = nowEpoch();
  deadlineMs_ = workStartedAtMs_ + static_cast<uint32_t>(workMinutes_) * 60000UL;
}

void PomodoroSession::beginBreak() {
  state_ = State::Break;
  paused_ = false;
  pausedRemainingMs_ = 0;
  currentBreakMinutes_ = breakMinutesForCycle(cycleCount_);
  currentBreakIsLong_ = cycleCount_ % LONG_BREAK_EVERY == 0;
  breakStartedAtMs_ = millis();
  breakStartedAtEpoch_ = nowEpoch();
  deadlineMs_ = breakStartedAtMs_ + static_cast<uint32_t>(currentBreakMinutes_) * 60000UL;
}

uint8_t PomodoroSession::breakMinutesForCycle(const uint8_t cycle) const {
  return cycle % LONG_BREAK_EVERY == 0 ? LONG_BREAK_MINUTES : breakMinutes_;
}

uint8_t PomodoroSession::nextBreakMinutes() const { return breakMinutesForCycle(cycleCount_ + 1); }

void PomodoroSession::togglePause() {
  if (state_ != State::Work && state_ != State::Break) return;
  if (!paused_) {
    pausedRemainingMs_ = remainingMs();
    paused_ = true;
  } else {
    paused_ = false;
    deadlineMs_ = millis() + pausedRemainingMs_;
  }
  LOG_DBG("POMO", "%s", paused_ ? "paused" : "resumed");
}

uint32_t PomodoroSession::remainingMs() const {
  if (state_ != State::Work && state_ != State::Break) return 0;
  if (paused_) return pausedRemainingMs_;
  const int32_t left = static_cast<int32_t>(deadlineMs_ - millis());
  return left > 0 ? static_cast<uint32_t>(left) : 0;
}

uint8_t PomodoroSession::remainingMinutes() const {
  const uint32_t ms = remainingMs();
  return static_cast<uint8_t>((ms + 59999UL) / 60000UL);
}

PomodoroSession::Event PomodoroSession::tick() {
  if (paused_) return Event::None;
  if (state_ == State::Work) {
    if (static_cast<int32_t>(millis() - deadlineMs_) < 0) return Event::None;
    recordSession(Kind::Work, workStartedAtEpoch_, nowEpoch(), workMinutes_, true);
    state_ = State::WorkDone;
    LOG_INF("POMO", "work complete (%u min)", workMinutes_);
    return Event::WorkDone;
  }
  if (state_ == State::Break) {
    if (static_cast<int32_t>(millis() - deadlineMs_) < 0) return Event::None;
    const Kind kind = currentBreakIsLong_ ? Kind::LongBreak : Kind::Break;
    recordSession(kind, breakStartedAtEpoch_, nowEpoch(), currentBreakMinutes_, true);
    state_ = State::BreakDone;
    LOG_INF("POMO", "break complete (%u min)", currentBreakMinutes_);
    return Event::BreakDone;
  }
  return Event::None;
}

void PomodoroSession::acknowledge() {
  if (state_ == State::WorkDone) {
    cycleCount_++;
    beginBreak();
  } else if (state_ == State::BreakDone) {
    beginWork();
  }
}

void PomodoroSession::endSession() {
  if (state_ == State::Work) {
    const uint32_t minutes = elapsedMinutes(static_cast<uint32_t>(workMinutes_) * 60000UL, remainingMs());
    if (minutes > 0) {
      recordSession(Kind::Work, workStartedAtEpoch_, nowEpoch(), static_cast<uint16_t>(minutes), false);
    }
  } else if (state_ == State::Break) {
    const uint32_t minutes = elapsedMinutes(static_cast<uint32_t>(currentBreakMinutes_) * 60000UL, remainingMs());
    if (minutes > 0) {
      const Kind kind = currentBreakIsLong_ ? Kind::LongBreak : Kind::Break;
      recordSession(kind, breakStartedAtEpoch_, nowEpoch(), static_cast<uint16_t>(minutes), false);
    }
  }
  state_ = State::Idle;
  paused_ = false;
  pausedRemainingMs_ = 0;
  cycleCount_ = 0;
  LOG_INF("POMO", "session ended");
}

void PomodoroSession::recordSession(const Kind kind, uint32_t startedAtEpoch, uint32_t endedAtEpoch,
                                    const uint16_t minutes, const bool completed) {
  if (minutes == 0) return;
  // A half-known timestamp is worse than none: the relay groups stats by the
  // startedAt date prefix, and the flush substitutes flush time when both are
  // zero. Record both-or-nothing.
  if (startedAtEpoch == 0 || endedAtEpoch == 0) {
    startedAtEpoch = 0;
    endedAtEpoch = 0;
  }
  if (queueCount_ == QUEUE_CAP) {
    queueHead_ = (queueHead_ + 1) % QUEUE_CAP;
    queueCount_--;
    LOG_ERR("POMO", "session queue full, dropped oldest");
  }
  Record& record = queue_[(queueHead_ + queueCount_) % QUEUE_CAP];
  snprintf(record.clientEventId, sizeof(record.clientEventId), "%08x%08x%08x%08x", static_cast<unsigned>(esp_random()),
           static_cast<unsigned>(esp_random()), static_cast<unsigned>(esp_random()),
           static_cast<unsigned>(esp_random()));
  record.kind = static_cast<uint8_t>(kind);
  record.startedAtEpoch = startedAtEpoch;
  record.endedAtEpoch = endedAtEpoch;
  record.minutes = minutes;
  record.completed = completed;
  queueCount_++;
  LOG_DBG("POMO", "queued %s %u min (completed=%d, pending=%u)", kind == Kind::Work ? "work" : "break",
          static_cast<unsigned>(minutes), completed ? 1 : 0, static_cast<unsigned>(queueCount_));
}

const PomodoroSession::Record* PomodoroSession::pendingAt(const size_t index) const {
  if (index >= queueCount_) return nullptr;
  return &queue_[(queueHead_ + index) % QUEUE_CAP];
}

void PomodoroSession::clearPending(size_t count) {
  if (count > queueCount_) count = queueCount_;
  queueHead_ = (queueHead_ + count) % QUEUE_CAP;
  queueCount_ -= count;
}
