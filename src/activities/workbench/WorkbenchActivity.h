#pragma once
#include <IpGeo.h>
#include <OpenClawSession.h>
#include <WeatherFormat.h>

#include <cstdint>

#include "activities/Activity.h"

class WorkbenchActivity final : public Activity {
  // Written by render() (render task), read by loop() (main task): a benign
  // single-byte flag; a lost race only skips/duplicates one requestUpdate().
  bool clockValidAtRender = false;
  // Main-task only. millis() stamp of the last periodic clock redraw.
  uint32_t lastClockTickMs = 0;
  // Redraw cadence while this page is frontmost. The clock band is a static
  // snapshot — without a tick it freezes on the time of the last render. One
  // full refresh per minute keeps the minute hand honest; the timer is the
  // whole cost, no extra logic.
  static constexpr uint32_t CLOCK_TICK_MS = 60000;

  // ── to-do ticker (main task writes, render task reads) ──────────────
  // First visible row of the to-do window and the millis() stamp of its last
  // step. Both advanced by loop() while the cron list is longer than the
  // window; render() copies the row index through WorkbenchData. An aligned
  // size_t / uint32_t cannot tear on this single core, so no lock is needed —
  // the worst race is one frame drawn at the previous offset.
  size_t todoFirstRow = 0;
  uint32_t lastTodoScrollMs = 0;

  // ── card selection ─────────────────────────────────────────────────────
  // -1 = nothing selected (Confirm = 同步, Back = leave the page); 0..3 is the
  // index of the card in render()'s boxes[] (0 clock, 1 weather, 2 todos,
  // 3 reading). The side keys step it; so do front Left/Right while nothing is
  // selected (no room on screen to hint at side keys), and once a card is
  // picked they belong to that card. Confirm activates — see activateSelected().
  int8_t selectedCard = -1;
  // Visual reading order (top-left → bottom-right) of those four boxes,
  // written by render() from the layout it just computed and read by loop().
  // Portrait's index order already is that order; the landscape columns have
  // independent heights, so box 3 can sit above box 2. One byte per entry —
  // a render landing mid-read costs one step in the wrong direction.
  uint8_t cardOrder[4] = {0, 1, 2, 3};
  // Voice "add a to-do" grace: the gateway may create the automation a beat
  // after the reply that was asked for it, so the list is pulled once more
  // when this one-shot expires (set in the result handler, fired in loop()).
  bool cronRefreshQueued_ = false;
  uint32_t cronRefreshAtMs_ = 0;

  void moveSelection(int dir);
  void scrollTodoWindow(int dir);
  void activateSelected();

  // ── weather sync (F4b, docs/v4.0-development-plan.md §11.5-D1/D3) ────
  // Page-open sync with a 6 h cache and no polling, zero tokens: fetch this
  // device's IP geo (Chinese city names arrive already 汉字 for zh UIs), then
  // POST the gateway's /tools/invoke web_fetch wrapper around open-meteo.
  // No chat session needed at all. Both fetches run on the main task inside
  // advanceSync() — esp_http_client is a blocking call, but the sync card
  // already promises "正在同步" and e-ink repaints between steps.
  enum class Sync : uint8_t { Idle, Geo, Fetch };
  Sync sync_ = Sync::Idle;
  OpenClaw::IpGeoResult geo_{};
  bool haveGeo_ = false;
  uint32_t syncStartMs_ = 0;
  // Translated failure line (session reason or STR_WORKBENCH_SYNC_FAIL),
  // null while nothing failed. Static table pointer — no lifetime issue.
  const char* syncError_ = nullptr;
  OpenClaw::WeatherSnapshot weather_{};
  bool haveWeather_ = false;
  OpenClaw::Session& session_ = OpenClaw::Session::instance();

  // Overall bound for geo + weather fetch. Each HTTP step has its own
  // inactivity timeout (HTTP_TIMEOUT_MS); this only guarantees the card
  // cannot sit on "syncing" forever.
  static constexpr uint32_t SYNC_TOTAL_MS = 60000;
  // Freshness window for the SD cache (D1: 6 h 缓存) — what decides whether the
  // page refreshes itself on entry.
  static constexpr int64_t WEATHER_MAX_AGE_MS = 6LL * 60 * 60 * 1000;
  // Tighter window for a *manual* Sync press: the page already ran the round
  // it just needed, so a second OK inside half an hour buys nothing but a
  // repeat of ip-api + web_fetch. The to-do half of Sync is not gated — it
  // rides the WebSocket, costs no tokens, and is the part that changes often.
  static constexpr int64_t WEATHER_MANUAL_MAX_AGE_MS = 30LL * 60 * 1000;

  // ── gateway link: the to-do card's only data path ───────────────────
  // cron.list arrives over the shared WebSocket, and VoiceActivity was the
  // only thing that ever dialed it — so a cold boot landing straight here
  // showed 未连接 with a to-do list nothing could refresh. Brings the link up
  // when this page is frontmost (§11.4 mode B: 打开工作台时顺带同步). Idempotent:
  // an established or in-flight link is left alone. The socket is NOT closed
  // on exit — Voice's onExit comment records why the workbench reads it.
  void ensureGatewayLink();

  void requestSync();
  void advanceSync();
  void syncFailed(const char* reason);
  bool weatherFreshWithin(int64_t maxAgeMs) const;
  bool weatherFresh() const;
  bool weatherManualFresh() const;
  void loadWeatherCache();
  void saveWeatherCache(const OpenClaw::WeatherSnapshot& snapshot);
  static void onSessionNotifyTrampoline(void* ctx);
  void onSessionNotify();

 public:
  WorkbenchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Workbench", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // A weather round trip must not be cut short by the 10 min idle sleep.
  bool preventAutoSleep() override { return sync_ != Sync::Idle; }
};
