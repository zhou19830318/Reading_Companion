#pragma once

#include <cstdint>

// Keeps the Wi-Fi radio from staying up after the network features are done
// with it. A resident radio costs ~38 KB of internal heap and pins the CPU
// policy to 160 MHz (HalPowerManager keys off WiFi.getMode()), so a dictionary
// lookup or one AI answer otherwise taxes idle power until deep sleep. The
// features that leave the radio up on purpose (dictionary, ask-AI, voice,
// workbench, the file-transfer server) arm this manager when they enter; any
// session progress (noteActivity) or in-flight request (hold) postpones the
// shutdown, and poll() — pumped from the main loop — powers the radio down
// once they have all been quiet for IDLE_TIMEOUT_MS.
//
// Opt-in on purpose: OTA, OPDS browsing, Calibre and the other network
// activities already tear the radio down themselves (disconnect + restart);
// they must not race this timer mid-transfer.
//
// Threading: every method runs on the main-loop task — activities call it from
// onEnter()/loop(), WebSocket dispatches run inside poll() — so no locking.
class WifiIdleManager {
 public:
  static WifiIdleManager& getInstance();

  // An activity that may hold the radio open entered; arms the idle clock.
  // Idempotent: re-arming only restarts the clock.
  void arm();
  // Network progress (a connect landed, a dial is running, a reply streams):
  // restarts the idle clock. Cheap; call freely.
  void noteActivity();
  // A request is in flight whose radio must survive the wait (an active voice
  // capture, a running file server). Pair every hold() with release(); a
  // misbalanced hold only delays the shutdown, it leaks no memory.
  void hold();
  void release();
  // Called just before the radio is powered down, so a subsystem holding a
  // socket (the shared OpenClaw session) can park it deliberately instead of
  // watching the link die. Function pointer + context, the session-notifier
  // pattern (no heap, no template bloat).
  void setShutdownHook(void* ctx, void (*fn)(void*)) {
    hookCtx_ = ctx;
    hookFn_ = fn;
  }
  // Main-loop pump: performs the teardown when everything is idle.
  void poll();

 private:
  static constexpr uint32_t IDLE_TIMEOUT_MS = 60000;

  bool armed_ = false;
  uint8_t holds_ = 0;
  // millis() stamp of the last arm/note/hold/release; poll() uses the
  // wraparound-safe subtraction when comparing.
  uint32_t lastActivityMs_ = 0;
  void* hookCtx_ = nullptr;
  void (*hookFn_)(void*) = nullptr;

  void shutdown();
};

// Helper macro, the WIFI_STORE / SETTINGS shape.
#define WIFI_IDLE WifiIdleManager::getInstance()
