#pragma once

#include <cstddef>
#include <cstdint>

// Reader focus timer (番茄钟): one singleton state machine shared by the reader
// menu, the status-bar countdown and the alert screen. Countdowns are
// deadline-based on millis(), so the 10 MHz idle CPU frequency never drifts
// them; the deadline comparison uses signed subtraction (wraparound safe).
//
// Phase 1: work/break cycles with three presets, a 15-minute long break every
// fourth cycle, pause/resume, and an alert on each transition. Nothing is
// persisted: a deep sleep or reboot ends the session (RAM-only by design).
//
// Finished sessions are kept in a fixed-capacity RAM queue for the relay
// upload (PomodoroClient). The queue dies with a deep sleep too — accepted
// for phase 1; the device is offline most of the time and the stats screen
// tolerates missing days (the relay returns a sparse series).
class PomodoroSession {
  static PomodoroSession instance;

 public:
  enum class State : uint8_t { Idle, Work, WorkDone, Break, BreakDone };
  // Returned exactly once by tick() when a countdown reaches zero.
  enum class Event : uint8_t { None, WorkDone, BreakDone };
  // Wire kind for the relay store (relay/relay/pomodoro_store.py).
  enum class Kind : uint8_t { Work = 0, Break = 1, LongBreak = 2 };

  static constexpr uint8_t PRESET_COUNT = 3;
  struct Preset {
    uint8_t workMinutes;
    uint8_t breakMinutes;
  };
  static constexpr Preset PRESETS[PRESET_COUNT] = {{15, 3}, {25, 5}, {45, 15}};

  static constexpr uint8_t LONG_BREAK_EVERY = 4;
  static constexpr uint8_t LONG_BREAK_MINUTES = 15;
  static constexpr uint8_t MIN_WORK_MINUTES = 1;
  static constexpr uint8_t MAX_WORK_MINUTES = 120;

  // 16 slots x 48 B = 768 B static DRAM. Oldest is dropped when full.
  static constexpr size_t QUEUE_CAP = 16;

  struct Record {
    char clientEventId[33] = {};  // 32 lowercase hex + NUL
    uint8_t kind = 0;             // Kind
    uint32_t startedAtEpoch = 0;  // 0 = system clock was unset when recorded
    uint32_t endedAtEpoch = 0;    // 0 = system clock was unset when recorded
    uint16_t minutes = 0;         // planned when completed, elapsed when not
    bool completed = true;
  };

  // Starts a fresh session (ending any current one first). The break length is
  // the short-break length; every LONG_BREAK_EVERY-th break is 15 minutes.
  void start(uint8_t workMinutes, uint8_t breakMinutes);
  // Ends the current session; a partial work/break phase is recorded with
  // completed=false when at least one full minute elapsed.
  void endSession();
  // Advances the countdown. Call once per main-loop iteration; returns Alert
  // events once on the tick that reaches a deadline.
  Event tick();
  // Leaves a Done state and begins the next phase (break after work, work
  // after break). No-op in any other state.
  void acknowledge();
  // Pauses/resumes a running work or break countdown.
  void togglePause();

  State state() const { return state_; }
  bool isActive() const { return state_ != State::Idle; }
  bool isPaused() const { return paused_; }
  // Only a running (or paused) countdown blocks auto-sleep. A Done state is
  // the alert screen, which suppresses auto-sleep itself while visible.
  bool blocksAutoSleep() const { return state_ == State::Work || state_ == State::Break; }
  uint32_t remainingMs() const;
  uint8_t remainingMinutes() const;  // ceil, 0 when not counting
  uint8_t workMinutes() const { return workMinutes_; }
  uint8_t breakMinutes() const { return breakMinutes_; }
  uint8_t cycleCount() const { return cycleCount_; }
  // Length of the break acknowledge() would start right now (handles the
  // every-fourth-cycle long break) — the alert screen previews it.
  uint8_t nextBreakMinutes() const;

  // Pending relay records, oldest first. clearPending(n) drops the n oldest
  // after the relay acknowledged them.
  size_t pendingCount() const { return queueCount_; }
  const Record* pendingAt(size_t index) const;
  void clearPending(size_t count);

  static PomodoroSession& getInstance() { return instance; }

 private:
  PomodoroSession() = default;

  void beginWork();
  void beginBreak();
  void recordSession(Kind kind, uint32_t startedAtEpoch, uint32_t endedAtEpoch, uint16_t minutes, bool completed);
  // 1-based cycle number: every LONG_BREAK_EVERY-th break is the long one.
  uint8_t breakMinutesForCycle(uint8_t cycle) const;

  State state_ = State::Idle;
  bool paused_ = false;
  uint32_t deadlineMs_ = 0;
  uint32_t pausedRemainingMs_ = 0;
  uint8_t workMinutes_ = 25;
  uint8_t breakMinutes_ = 5;
  uint8_t currentBreakMinutes_ = 5;
  // True when the running break is the every-fourth-cycle long one. Tracked
  // explicitly because a short break may also be 15 minutes (45/15 preset), so
  // the length cannot be used to tell them apart in the stats.
  bool currentBreakIsLong_ = false;
  uint8_t cycleCount_ = 0;
  uint32_t workStartedAtEpoch_ = 0;
  uint32_t workStartedAtMs_ = 0;
  uint32_t breakStartedAtEpoch_ = 0;
  uint32_t breakStartedAtMs_ = 0;

  Record queue_[QUEUE_CAP] = {};
  size_t queueHead_ = 0;
  size_t queueCount_ = 0;
};

#define POMODORO PomodoroSession::getInstance()
