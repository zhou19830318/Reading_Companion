#include "WorkbenchActivity.h"

// clang-format off
// HalStorage.h pulls SdFat macros that collide with lwip's ip4_addr.h unless
// seen first (same ordering constraint as VoiceActivity.cpp / ChatHistory.cpp).
#include <Arduino.h>
#include <Bitmap.h>
#include <ChatHistoryFormat.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
// clang-format on
#include <CloudConfig.h>
#include <I18n.h>
#include <IpGeo.h>
#include <Logging.h>
#include <Memory.h>
#include <NetBootstrap.h>
#include <OpenClawHandshake.h>
#include <OpenClawSession.h>
#include <WeatherCities.h>
#include <WeatherFormat.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/voice/VoiceActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace ChatHistoryFormat = OpenClaw::ChatHistoryFormat;

namespace {

// Card chrome, tuned against docs/images/workbench.png. The mockup's portrait
// stacks fit 480x800 with proportional growth; landscape (width > height)
// splits into two columns so nothing falls off a 480px-tall panel.
constexpr int kCardRadius = 8;
constexpr int kCardPad = 10;
constexpr int kHeadGap = 6;
constexpr int kClockGap = 4;
constexpr double kScaleMin = 0.7;
constexpr double kScaleMax = 1.4;
constexpr int kStatusDot = 6;
constexpr int kOcCardMaxW = 210;
constexpr int kOcCardMinW = 140;
constexpr int kCoverColDiv = 3;  // cover takes at most 1/3 of the card width
// Rows the to-do window shows at once. Longer lists creep up one row at a
// time while this page is frontmost (see loop()), so a cron list that once
// forced a page turn now reads itself across the space the card already owns.
constexpr int kTodoWindowRows = 3;
// One row every 1.5 s — the window only holds three of them, and a 0.5 s step
// scrolled the list past unread before the eye had settled on it. The panel's
// own fast refresh (~600 ms) is the floor, so this is comfortably above it.
constexpr uint32_t TODO_SCROLL_STEP_MS = 1500;
// Dwell on the last window before wrapping: one full window's worth of steps,
// so the final rows get as long to be read as every other row does.
constexpr uint32_t TODO_SCROLL_HOLD_MS = kTodoWindowRows * TODO_SCROLL_STEP_MS;

// Grace after the voice round trip returns from "add a to-do": the agent asks
// for the automation to be created and the reply lands before the job exists,
// so the first cron.list pull right after Back can still be one beat early.
constexpr uint32_t VOICE_TODO_REFRESH_MS = 3000;

// SD cache for the weather snapshot — root dir first so first run can create
// it (chat/notes normally pre-create .crosspoint, never rely on it).
constexpr const char* WEATHER_CACHE_DIR = "/.crosspoint";
constexpr const char* WEATHER_CACHE_PATH = "/.crosspoint/weather.json";

// ── weather sync endpoints (F4b round 2: zero-token, device-located) ──
//
// Step 1 — WHERE: ip-api.com, this device's own public IP, so the card names
// the reader's location, not the server's (the old chat.send path could only
// see the gateway's IP). The lang code comes from IpGeo (zh UIs get 省/市 as
// 汉字); other UIs fall back to the English names the service serves by
// default. Plain HTTP (no key, no TLS) — location is not a secret worth TLS
// setup time.
//
// Step 2 — WHAT: the gateway's POST /tools/invoke web_fetch wrapper around
// open-meteo, using the coordinates step 1 returned. Same Bearer token and
// host the WebSocket dial already uses (CloudConfig). ~2 KB of traffic, no
// model tokens, no history pollution. The double-wrapped reply is unwound by
// OpenClaw::parseWeatherInvokeResponse (host-tested in test/weather_format).
constexpr const char* GEO_URL_FMT = "http://ip-api.com/json/?fields=status,country,regionName,city,lat,lon&lang=%s";
constexpr double GEO_LAT_MIN = -90.0, GEO_LAT_MAX = 90.0;
constexpr double GEO_LON_MIN = -180.0, GEO_LON_MAX = 180.0;

// tools/invoke URL + timeout: same host:scheme:port the WebSocket dial uses
// (only the path differs — the HTTP control plane sits next to the WS one).
// 12 s per socket op: the server measured ~1.3 s, so 12 s is generous while
// keeping a dead gateway from hanging the "syncing" card.
std::string invokeUrl() {
  const CloudConfig& cfg = CLOUD_CFG;
  return std::string(cfg.tls ? "https://" : "http://") + cfg.host + ":" + std::to_string(cfg.port) + "/tools/invoke";
}
constexpr uint32_t INVOKE_TIMEOUT_MS = 12000;

// WMO weather-processing codes, the numeric condition open-meteo's daily
// weather_code carries. Stored as a DECIMAL STRING in the snapshot (the
// cache/parser stay text-shaped); composeDayStatus maps it through tr().

// WMO code -> StrId, so languages with a full translation get a localized
// condition line. Two clusters collapse code families that share one word in
// every UI language: drizzle (51-57) is 小雨 in Chinese, lluvia débil in
// Spanish, just "Light drizzle" in English; snow (71-77, 85-86) likewise.
StrId wmoToStrId(const int code) {
  if (code == 0) return StrId::STR_WEATHER_CLEAR;
  if (code == 1 || code == 2) return StrId::STR_WEATHER_MAINLY_CLEAR;
  if (code == 3) return StrId::STR_WEATHER_OVERCAST;
  if (code == 45 || code == 48) return StrId::STR_WEATHER_FOG;
  if (code >= 51 && code <= 57) return StrId::STR_WEATHER_DRIZZLE;
  if (code >= 61 && code <= 65) return StrId::STR_WEATHER_RAIN;
  if (code == 66 || code == 67) return StrId::STR_WEATHER_FREEZING_RAIN;
  if (code >= 71 && code <= 77) return StrId::STR_WEATHER_SNOW;
  if (code == 80 || code == 81 || code == 82) return StrId::STR_WEATHER_SHOWERS;
  if (code == 85 || code == 86) return StrId::STR_WEATHER_SNOW_SHOWERS;
  if (code == 95) return StrId::STR_WEATHER_THUNDERSTORM;
  if (code == 96 || code == 99) return StrId::STR_WEATHER_THUNDER_HAIL;
  return StrId::STR_WEATHER_UNKNOWN;
}

// 16-point compass -> 8 named sectors (the eight-word Chinese wind scheme;
// EN/ES get the same eight directions). Inter-cardinal codes fold to the
// nearer cardinal — 北 East/北 north stay distinguishable enough on a card.
StrId windDirToStrId(const int deg) {
  const int d = ((deg % 360) + 360) % 360;
  if (d >= 337 || d < 23) return StrId::STR_WIND_N;
  if (d < 68) return StrId::STR_WIND_NE;
  if (d < 113) return StrId::STR_WIND_E;
  if (d < 158) return StrId::STR_WIND_SE;
  if (d < 203) return StrId::STR_WIND_S;
  if (d < 248) return StrId::STR_WIND_SW;
  if (d < 293) return StrId::STR_WIND_W;
  return StrId::STR_WIND_NW;
}

struct CardBox {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct WorkbenchData {
  bool clockValid = false;
  char timeBuf[8] = "--:--";
  char dateLine[44] = "----------";
  char dateOnly[32] = "----------";
  char weekday[16] = "";
  bool linkUp = false;
  char uptime[16] = {};
  const RecentBook* book = nullptr;
  std::string coverThumb;
  bool hasCover = false;
  // Weather card inputs — copied from the sync-owned state by render().
  // Publish order (whole snapshot first, haveWeather_ flag after) keeps a
  // torn read down to "one stale frame", the same benign race the session
  // statics below already accept (render task vs main task).
  bool haveWeather = false;
  bool syncActive = false;
  const char* syncError = nullptr;
  OpenClaw::WeatherSnapshot weather{};
  // First row of the to-do window — see the ticker in loop(). Render task
  // reads the copy loop() published; a step landing mid-render costs one
  // frame drawn at the previous offset, the same benign race as syncActive.
  size_t todoFirst = 0;
};

double clampScale(const double s) { return s < kScaleMin ? kScaleMin : (s > kScaleMax ? kScaleMax : s); }

// tr() takes a literal StrId, so the weekday is a switch, not an array index.
const char* weekdayStr(const int wday) {
  switch (wday) {
    case 0:
      return tr(STR_WD_SUN);
    case 1:
      return tr(STR_WD_MON);
    case 2:
      return tr(STR_WD_TUE);
    case 3:
      return tr(STR_WD_WED);
    case 4:
      return tr(STR_WD_THU);
    case 5:
      return tr(STR_WD_FRI);
    case 6:
      return tr(STR_WD_SAT);
    default:
      return "";
  }
}

// Selection frame: a heavier inside border drawn over the card's own 1 px
// chrome, so it reads as "this one" without touching the padded content.
// The clock band is frameless, which is exactly why the frame is drawn from
// the layout box rather than left to each card.
void drawSelectionFrame(const GfxRenderer& r, const CardBox& b) {
  if (b.h <= 0) return;
  r.drawRoundedRect(b.x, b.y, b.w, b.h, 3, kCardRadius, true);
}

int clockBandH(const GfxRenderer& r) {
  const int lh18 = r.getLineHeight(NOTOSERIF_18_FONT_ID);
  const int lh14 = r.getLineHeight(NOTOSERIF_14_FONT_ID);
  const int lh12 = r.getLineHeight(UI_12_FONT_ID);
  // lineFits() only grants (kCardPad - 2)px of bottom slack, so a band sized to
  // exactly the text it holds would silently drop its last line.
  const int slack = kCardPad - 2;
  // Left column: time, then the date, with the weekday on its own line when
  // "YYYY年MM月DD日 星期四" is too wide for the space left of the OpenClaw card.
  const int leftNeed = lh18 + kClockGap + lh14 + kClockGap + lh14 + slack;
  // OpenClaw card: pad + title(L+2) + status(L+4) + divider(5) + label(L) + value(L).
  // Before this the band was sized from the left column only, which is what made
  // the uptime value the line that got dropped.
  const int ocNeed = kCardPad + (lh12 + 2) + (lh12 + 4) + 5 + lh12 + lh12 + slack;
  return std::max(leftNeed, ocNeed);
}

int weatherCardH(const GfxRenderer& r) {
  const int lh = r.getLineHeight(UI_12_FONT_ID);
  // Three UI_12 lines (label / temperatures / status) — the temperature row
  // shares the card's face since F4b round 3 dropped the 16pt numerals.
  return kCardPad * 2 + lh * 3 + 4;
}

int todosCardH(const GfxRenderer& r) {
  const int lh = r.getLineHeight(UI_12_FONT_ID);
  // One line for the empty state; otherwise one per visible row, so a short
  // list does not pay for blank rows — and a long one is capped at the window
  // it scrolls through instead of growing until the layout has to scale down.
  int rows = 1;
  if (OpenClaw::Session::cronValid()) {
    rows = static_cast<int>(OpenClaw::Session::cronJobCount());
    if (rows < 1) rows = 1;
    if (rows > kTodoWindowRows) rows = kTodoWindowRows;
  }
  return kCardPad * 2 + lh + kHeadGap + rows * lh;
}

int readingCardH(const GfxRenderer& r, const bool hasCover) {
  const int header = r.getLineHeight(UI_12_FONT_ID) + kHeadGap;
  const int body = hasCover ? 116 : r.getLineHeight(UI_12_FONT_ID) * 3 + 4;
  return kCardPad * 2 + header + body;
}

bool lineFits(const GfxRenderer& r, const CardBox& b, const int y, const int font) {
  return y + r.getLineHeight(font) <= b.y + b.h - kCardPad + 2;
}

// Free-standing clock (no frame, per mockup) plus the framed OpenClaw card.
void drawClockBand(const GfxRenderer& r, const CardBox& b, const WorkbenchData& d) {
  if (b.h <= 0) return;
  const int font12 = UI_12_FONT_ID;

  int ocW = b.w * 9 / 20;
  if (ocW > kOcCardMaxW) ocW = kOcCardMaxW;
  if (ocW < kOcCardMinW) ocW = b.w / 2;
  const int ocX = b.x + b.w - ocW;

  int ty = b.y;
  if (lineFits(r, b, ty, NOTOSERIF_18_FONT_ID)) {
    r.drawText(NOTOSERIF_18_FONT_ID, b.x, ty, d.timeBuf, true, EpdFontFamily::BOLD);
    ty += r.getLineHeight(NOTOSERIF_18_FONT_ID) + kClockGap;
  }
  if (d.clockValid && lineFits(r, b, ty, NOTOSERIF_14_FONT_ID)) {
    const int maxW = ocX - b.x - kCardPad;
    const int lh14 = r.getLineHeight(NOTOSERIF_14_FONT_ID);
    if (r.getTextWidth(NOTOSERIF_14_FONT_ID, d.dateLine) <= maxW) {
      r.drawText(NOTOSERIF_14_FONT_ID, b.x, ty, d.dateLine, true);
    } else {
      // The weekday does not fit beside the date; give it its own line instead
      // of ellipsising the whole line — a truncated date reads as a rendering
      // fault, and the band is already tall enough for the OpenClaw card. When
      // even the bare date is too wide (landscape's narrow left column) fall
      // back to truncating just that line so it cannot collide with the card.
      const std::string date = r.getTextWidth(NOTOSERIF_14_FONT_ID, d.dateOnly) <= maxW
                                   ? std::string(d.dateOnly)
                                   : r.truncatedText(NOTOSERIF_14_FONT_ID, d.dateOnly, maxW);
      r.drawText(NOTOSERIF_14_FONT_ID, b.x, ty, date.c_str(), true);
      ty += lh14 + kClockGap;
      if (lineFits(r, b, ty, NOTOSERIF_14_FONT_ID) && d.weekday[0] != '\0' &&
          r.getTextWidth(NOTOSERIF_14_FONT_ID, d.weekday) <= maxW) {
        r.drawText(NOTOSERIF_14_FONT_ID, b.x, ty, d.weekday, true);
      }
    }
  }

  r.drawRoundedRect(ocX, b.y, ocW, b.h, 1, kCardRadius, true);
  const int ix = ocX + kCardPad;
  int iy = b.y + kCardPad;
  if (lineFits(r, b, iy, font12)) {
    r.drawText(font12, ix, iy, "OpenClaw", true, EpdFontFamily::BOLD);
    iy += r.getLineHeight(font12) + 2;
  }
  if (lineFits(r, b, iy, font12)) {
    r.fillRect(ix, iy + r.getLineHeight(font12) / 2 - kStatusDot / 2, kStatusDot, kStatusDot);
    r.drawText(font12, ix + kStatusDot + 4, iy, d.linkUp ? tr(STR_WORKBENCH_RUNNING) : tr(STR_WORKBENCH_IDLE));
    iy += r.getLineHeight(font12) + 4;
  }
  if (iy + 4 <= b.y + b.h - kCardPad) {
    r.drawLine(ix, iy, ocX + ocW - kCardPad, iy);
    iy += 5;
  }
  if (lineFits(r, b, iy, font12)) {
    r.drawText(font12, ix, iy, tr(STR_WORKBENCH_UPTIME));
    iy += r.getLineHeight(font12);
  }
  if (lineFits(r, b, iy, font12)) {
    r.drawText(font12, ix, iy, d.uptime, true, EpdFontFamily::BOLD);
  }
}

// One status line for a day: condition and wind share it (either may be
// absent, an empty result renders as a blank line).
//
// Text-shape note: with the zero-token pipeline a day's cond is a decimal
// WMO code string and wind is "deg,speed", both translated HERE at render
// time through tr() — the cache stays language-independent, so switching the
// UI language re-translates without a resync. A legacy cache (v1, from the
// chat.send era) holds real words; the leading-digit test tells the two
// shapes apart, and the legacy text passes through untranslated.
void composeDayStatus(char* out, const size_t cap, const OpenClaw::WeatherDay& day) {
  char cond[48];
  if (day.cond[0] >= '0' && day.cond[0] <= '9') {
    snprintf(cond, sizeof(cond), "%s", I18N.get(wmoToStrId(atoi(day.cond))));
  } else {
    snprintf(cond, sizeof(cond), "%s", day.cond);
  }
  // "deg,speed" from the new pipeline vs legacy "NNE 15km/h" free text.
  char wind[48];
  const char* comma = strchr(day.wind, ',');
  if (comma != nullptr && day.wind[0] >= '0' && day.wind[0] <= '9') {
    const int deg = atoi(day.wind);
    const int spd = atoi(comma + 1);
    if (spd > 0) {
      snprintf(wind, sizeof(wind), tr(STR_WIND_FMT), I18N.get(windDirToStrId(deg)), spd);
    } else {
      wind[0] = '\0';  // calm — the condition alone fills the line
    }
  } else {
    snprintf(wind, sizeof(wind), "%s", day.wind);
  }
  if (cond[0] != '\0' && wind[0] != '\0') {
    snprintf(out, cap, "%s %s", cond, wind);
  } else if (cond[0] != '\0') {
    snprintf(out, cap, "%s", cond);
  } else {
    snprintf(out, cap, "%s", wind);
  }
}

void drawWeatherHalf(const GfxRenderer& r, const CardBox& b, const int x, const int w, int y, const char* label,
                     const OpenClaw::WeatherDay* day, const char* status) {
  const int font12 = UI_12_FONT_ID;
  if (!lineFits(r, b, y, font12)) return;
  // The label may carry " · <city>" (today half), so it gets the same
  // width guard the status line has — a run-off label reads as a fault.
  const std::string labelLine = r.truncatedText(font12, label, w, EpdFontFamily::BOLD);
  r.drawText(font12, x, y, labelLine.c_str(), true, EpdFontFamily::BOLD);
  y += r.getLineHeight(font12);
  if (!lineFits(r, b, y, font12)) return;
  char temps[24];
  if (day != nullptr && day->hasTemps) {
    snprintf(temps, sizeof(temps), "%d° / %d°", day->hi, day->lo);
  } else {
    snprintf(temps, sizeof(temps), "%s", "--° / --°");
  }
  // Same face as the label and status around it: the NOTOSERIF_16 the mockup
  // used for the temperatures made the two halves read as a poster headline
  // over 12pt captions. One face across the card costs nothing but the
  // weatherCardH() line that sized itself off the old height.
  UITheme::drawCenteredText(r, Rect{x, y, w, r.getLineHeight(font12)}, font12, y, temps, true);
  y += r.getLineHeight(font12);
  if (status == nullptr || status[0] == '\0') return;
  if (!lineFits(r, b, y, font12)) return;
  const std::string statusLine = r.truncatedText(font12, status, w);
  UITheme::drawCenteredText(r, Rect{x, y, w, r.getLineHeight(font12)}, font12, y, statusLine.c_str(), true);
}

void drawWeatherCard(const GfxRenderer& r, const CardBox& b, const WorkbenchData& d) {
  if (b.h <= 0) return;
  r.drawRoundedRect(b.x, b.y, b.w, b.h, 1, kCardRadius, true);
  const int innerTop = b.y + kCardPad;
  const int innerBottom = b.y + b.h - kCardPad;
  const int divX = b.x + b.w / 2;
  r.drawLine(divX, innerTop, divX, innerBottom);
  const int lx = b.x + kCardPad;
  const int rx = divX + 6;

  // Status precedence, mirrored on both halves so the card never shows one
  // half "syncing" and the other "not synced":
  //   sync in flight -> syncing line; failed -> the failure line; cached ->
  //   condition (+wind), tomorrow blank when the reply had none; else empty.
  char todayStatus[96];
  char tomorrowStatus[96];
  const char* todayLine = tr(STR_WORKBENCH_NO_WEATHER);
  const char* tomorrowLine = tr(STR_WORKBENCH_NO_WEATHER);
  if (d.syncActive) {
    todayLine = tomorrowLine = tr(STR_WORKBENCH_SYNCING);
  } else if (d.syncError != nullptr) {
    todayLine = tomorrowLine = d.syncError;
  } else if (d.haveWeather) {
    composeDayStatus(todayStatus, sizeof(todayStatus), d.weather.today);
    todayLine = todayStatus;
    if (d.weather.hasTomorrow) {
      composeDayStatus(tomorrowStatus, sizeof(tomorrowStatus), d.weather.tomorrow);
      tomorrowLine = tomorrowStatus;
    } else {
      tomorrowLine = "";
    }
  }

  // The today label carries the city once a sync has one — it answers
  // "which location is this forecast for?" at no extra row.
  char todayLabel[80];
  if (d.weather.city[0] != '\0') {
    snprintf(todayLabel, sizeof(todayLabel), tr(STR_WORKBENCH_TODAY_CITY_FMT), tr(STR_WORKBENCH_TODAY), d.weather.city);
  } else {
    snprintf(todayLabel, sizeof(todayLabel), "%s", tr(STR_WORKBENCH_TODAY));
  }

  const OpenClaw::WeatherDay* today = d.haveWeather ? &d.weather.today : nullptr;
  const OpenClaw::WeatherDay* tomorrow = (d.haveWeather && d.weather.hasTomorrow) ? &d.weather.tomorrow : nullptr;
  drawWeatherHalf(r, b, lx, divX - 6 - lx, innerTop, todayLabel, today, todayLine);
  drawWeatherHalf(r, b, rx, b.x + b.w - kCardPad - rx, innerTop, tr(STR_WORKBENCH_TOMORROW), tomorrow, tomorrowLine);
}

// Whole days since the epoch for a local-time stamp — the same floor
// division render() applies to the date line (integer division truncates
// toward zero, so a pre-1970 stamp needs the nudge).
int64_t floorDays(const int64_t sec) { return sec >= 0 ? sec / 86400 : (sec - 86399) / 86400; }

// "今天" / "明天" / "后天" for the near days, M/D once a job runs further out:
// a relative "in 3 days" would need a key in every language and reads worse
// than the date it stands for.
void todoDayWord(char* dst, const size_t cap, const int64_t offset, const int64_t days) {
  const char* word = nullptr;
  if (offset == 0) {
    word = tr(STR_WORKBENCH_TODAY);
  } else if (offset == 1) {
    word = tr(STR_WORKBENCH_TOMORROW);
  } else if (offset == 2) {
    word = tr(STR_DAY_AFTER);
  }
  if (word != nullptr) {
    snprintf(dst, cap, "%s", word);
    return;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  ChatHistoryFormat::civilFromDays(days, year, month, day);
  snprintf(dst, cap, "%d/%d", month, day);
}

void drawTodosCard(const GfxRenderer& r, const CardBox& b, const WorkbenchData& d) {
  if (b.h <= 0) return;
  r.drawRoundedRect(b.x, b.y, b.w, b.h, 1, kCardRadius, true);
  const int ix = b.x + kCardPad;
  const int font12 = UI_12_FONT_ID;
  const int lh = r.getLineHeight(font12);
  int y = b.y + kCardPad;
  if (lineFits(r, b, y, font12)) {
    r.drawText(font12, ix, y, tr(STR_WORKBENCH_TODOS), true, EpdFontFamily::BOLD);
    y += lh + kHeadGap;
  }
  const int innerW = b.w - kCardPad * 2;
  const OpenClaw::CronJob* jobs = OpenClaw::Session::cronJobs();
  const size_t jobCount = OpenClaw::Session::cronJobCount();
  const bool haveJobs = OpenClaw::Session::cronValid() && jobCount > 0 && jobs != nullptr;

  if (!haveJobs) {
    if (y + lh <= b.y + b.h - kCardPad + 2) {
      UITheme::drawCenteredText(r, Rect{ix, y, innerW, lh}, UI_12_FONT_ID, y, tr(STR_WORKBENCH_NO_TODOS), true);
    }
    return;
  }

  // Right-aligned "enabled / total" on the header line.
  size_t enabled = 0;
  for (size_t i = 0; i < jobCount; ++i) {
    if (jobs[i].enabled) ++enabled;
  }
  char count[16];
  snprintf(count, sizeof(count), "%u/%u", static_cast<unsigned>(enabled), static_cast<unsigned>(jobCount));
  const int countW = r.getTextWidth(font12, count);
  if (countW < innerW) r.drawText(font12, ix + innerW - countW, b.y + kCardPad, count, true);

  // Row: "[ ]1.接孩子放学 明天下午12:20". The box is ASCII text rather than
  // the filled square it used to be — `[ ]` renders from the UI face with no
  // glyph risk (the old mark existed because no font was guaranteed to carry
  // U+2713), and the gateway's user to-dos are all armed, so the fill carried
  // no information worth a second draw call.
  const int textW = b.x + b.w - kCardPad - ix;
  const bool haveClock = d.clockValid;
  const int64_t nowSec =
      haveClock ? ChatHistoryFormat::localEpochSeconds(OpenClaw::nowEpochMs(), SETTINGS.clockUtcOffsetQ) : 0;
  const int64_t nowDays = floorDays(nowSec);
  // Windowed rows: kTodoWindowRows of them starting at d.todoFirst, which
  // loop() advances while the list is longer than the window. Re-clamped here
  // too, so a cron list that shrank between two frames cannot index past its
  // own end (the window is never larger than jobCount).
  const size_t window = std::min<size_t>(jobCount, static_cast<size_t>(kTodoWindowRows));
  const size_t first = std::min<size_t>(d.todoFirst, jobCount - window);
  for (size_t i = first; i < first + window; ++i) {
    if (!lineFits(r, b, y, font12)) break;
    const OpenClaw::CronJob& job = jobs[i];
    // Absolute position in the list, not in the window: a windowed row has
    // to say which item it is even when the top of the list is off card.
    char head[16];
    // Two spaces, not one: the UI font's space advance is ~3 px, which on
    // screen collapsed the box to "[ ]"→"[]" with the brackets nearly
    // touching. Measured on device: one space left a 3 px gap, two leave 6 px.
    snprintf(head, sizeof(head), "[  ]%u.", static_cast<unsigned>(i + 1));

    // payload.text is the only human wording the gateway keeps; `name` is a
    // generated slug, so it only ever appears for a job created outside a
    // conversation (the card's fallback, not its normal case).
    const char* raw = job.text[0] != '\0' ? job.text : (job.name[0] != '\0' ? job.name : job.id);
    char title[80];
    OpenClaw::formatTodoTitle(title, sizeof(title), raw);

    char when[40] = {};
    if (haveClock && job.nextRunAtMs != 0) {
      const int64_t nextSec =
          ChatHistoryFormat::localEpochSeconds(static_cast<int64_t>(job.nextRunAtMs), SETTINGS.clockUtcOffsetQ);
      const int64_t nextDays = floorDays(nextSec);
      char dayWord[24];
      todoDayWord(dayWord, sizeof(dayWord), nextDays - nowDays, nextDays);
      const int secsOfDay = static_cast<int>(nextSec - nextDays * 86400);
      char clock[12];
      const char* meridiem = OpenClaw::formatTodoClock(clock, sizeof(clock), secsOfDay / 3600, (secsOfDay % 3600) / 60,
                                                       tr(STR_AM), tr(STR_PM));
      // The format string only decides whether the parts are spaced; the
      // order is the caller's, because Chinese puts the period before the
      // clock ("明天下午12:20") and Latin scripts after it ("Today 12:20 PM").
      if (I18n::isChinese(I18N.getLanguage())) {
        snprintf(when, sizeof(when), tr(STR_TODO_WHEN_FMT), dayWord, meridiem, clock);
      } else {
        snprintf(when, sizeof(when), tr(STR_TODO_WHEN_FMT), dayWord, clock, meridiem);
      }
    }

    char line[192];
    if (when[0] != '\0') {
      snprintf(line, sizeof(line), "%s%s %s", head, title, when);
    } else {
      snprintf(line, sizeof(line), "%s%s", head, title);
    }
    if (r.getTextWidth(font12, line) > textW) {
      // Give the shortfall back to the wording: the box/number and the due
      // time are the facts, the middle sentence is what may give.
      const int headW = r.getTextWidth(font12, head);
      const int whenW = when[0] != '\0' ? r.getTextWidth(font12, when) + r.getTextWidth(font12, " ") : 0;
      const int titleW = std::max(0, textW - headW - whenW);
      const std::string shortened = r.truncatedText(font12, title, titleW);
      if (when[0] != '\0') {
        snprintf(line, sizeof(line), "%s%s %s", head, shortened.c_str(), when);
      } else {
        snprintf(line, sizeof(line), "%s%s", head, shortened.c_str());
      }
      if (r.getTextWidth(font12, line) > textW) {
        r.drawText(font12, ix, y, r.truncatedText(font12, line, textW).c_str(), true);
        y += lh;
        continue;
      }
    }
    r.drawText(font12, ix, y, line, true);
    y += lh;
  }
}

void drawReadingCard(const GfxRenderer& r, const CardBox& b, const WorkbenchData& d) {
  if (b.h <= 0) return;
  r.drawRoundedRect(b.x, b.y, b.w, b.h, 1, kCardRadius, true);
  const int ix = b.x + kCardPad;
  const int font12 = UI_12_FONT_ID;
  const int lh = r.getLineHeight(font12);
  int y = b.y + kCardPad;
  if (lineFits(r, b, y, font12)) {
    r.drawText(font12, ix, y, tr(STR_WORKBENCH_NOW_READING), true, EpdFontFamily::BOLD);
    const char* chev = ">";
    r.drawText(font12, b.x + b.w - kCardPad - r.getTextWidth(font12, chev), y, chev, true);
    y += lh + kHeadGap;
  }
  const int innerBottom = b.y + b.h - kCardPad;
  const int bodyH = innerBottom - y;
  if (bodyH < lh) return;
  if (d.book == nullptr) {
    UITheme::drawCenteredText(r, Rect{ix, y, b.w - kCardPad * 2, lh}, font12, y, tr(STR_WORKBENCH_NO_BOOK), true);
    return;
  }

  int textX = ix;
  int maxTextW = b.w - kCardPad * 2;
  if (d.hasCover) {
    HalFile file;
    if (Storage.openFileForRead("WBA", d.coverThumb, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        const int colW = (b.w - kCardPad * 2) / kCoverColDiv;
        float sc = 1.0f;
        if (bitmap.getWidth() > colW) sc = static_cast<float>(colW) / bitmap.getWidth();
        if (bitmap.getHeight() > bodyH) sc = std::min(sc, static_cast<float>(bodyH) / bitmap.getHeight());
        const int dw = static_cast<int>(bitmap.getWidth() * sc);
        const int dh = static_cast<int>(bitmap.getHeight() * sc);
        r.drawBitmap(bitmap, ix + (colW - dw) / 2, y + (bodyH - dh) / 2, colW, bodyH);
        textX = ix + colW + 10;
        maxTextW = b.x + b.w - kCardPad - textX;
      }
    }
  }

  // Third line is chapter + book % once the reader has reported progress.
  const bool hasProgress = d.book->progressPercent > 0 || !d.book->chapterTitle.empty();
  const int lines = (d.book->author.empty() ? 1 : 2) + (hasProgress ? 1 : 0);
  int ty = y + (bodyH - (lines * lh + 2)) / 2;
  if (ty < y) ty = y;
  if (ty + lh <= innerBottom + 2) {
    const std::string title = r.truncatedText(font12, d.book->title.c_str(), maxTextW, EpdFontFamily::BOLD);
    r.drawText(font12, textX, ty, title.c_str(), true, EpdFontFamily::BOLD);
    ty += lh + 2;
  }
  if (!d.book->author.empty() && ty + lh <= innerBottom + 2) {
    char author[160];
    snprintf(author, sizeof(author), tr(STR_WORKBENCH_AUTHOR_FMT), d.book->author.c_str());
    const std::string line = r.truncatedText(font12, author, maxTextW);
    r.drawText(font12, textX, ty, line.c_str(), true);
    ty += lh + 2;
  }
  if (hasProgress && ty + lh <= innerBottom + 2) {
    char reading[192];
    snprintf(reading, sizeof(reading), tr(STR_WORKBENCH_READING_FMT), d.book->chapterTitle.c_str(),
             d.book->progressPercent);
    const std::string line = r.truncatedText(font12, reading, maxTextW);
    r.drawText(font12, textX, ty, line.c_str(), true);
  }
}

}  // namespace

void WorkbenchActivity::onEnter() {
  Activity::onEnter();
  lastClockTickMs = millis();   // first periodic redraw lands CLOCK_TICK_MS after entry
  todoFirstRow = 0;             // every entry starts at the top of the to-do list
  lastTodoScrollMs = millis();  // ...and gives the first window a full step to read
  loadWeatherCache();
  // Repaint hook for session events while this page is frontmost (link
  // up/down, the cron.list reply). Cleared in onExit before the object dies.
  session_.setNotifier(this, &WorkbenchActivity::onSessionNotifyTrampoline);
  // Page-open sync (D1: 6 h cache / 开页同步 / no polling). With the radio off
  // this page now brings it up itself: requestSync() detours through
  // WifiSelectionActivity (auto-connects the saved network, then resumes the
  // round), so the cards fill in on a cold boot instead of only after some
  // other page — Voice — happened to raise the radio first. The OK key re-arms
  // the same round at any time.
  if (WiFi.status() != WL_CONNECTED || !weatherFresh()) {
    requestSync();  // also arms the gateway link (see ensureGatewayLink)
  } else {
    // Weather is fresh but the to-do card is the socket's, not the cache's:
    // bring it up so the list on screen is this session's, not last boot's.
    ensureGatewayLink();
  }
  requestUpdate();
}

void WorkbenchActivity::onExit() {
  session_.setNotifier(nullptr, nullptr);
  // This page may have dialed the shared socket (ensureGatewayLink), but it
  // never closes it: the link outlives whichever activity opened it, exactly
  // as Voice's onExit records. The notifier is the part that must go before
  // the object does.
  Activity::onExit();
}

void WorkbenchActivity::onSessionNotifyTrampoline(void* ctx) {
  static_cast<WorkbenchActivity*>(ctx)->onSessionNotify();
}

void WorkbenchActivity::onSessionNotify() {
  // Fires from inside the WebSocket dispatch: repaint only, never send.
  // Bounded — the session does not notify for health/tick frames (M0-2).
  requestUpdate();
}

// Brings the shared gateway link up so the to-do card can be refreshed.
// Idempotent and cheap when the link already exists: an established socket is
// left alone (Voice's onExit deliberately keeps it open for this page), and a
// dial that is already running is not started twice.
void WorkbenchActivity::ensureGatewayLink() {
  // Ask for the list on every Sync and on every entry to this page: it rides
  // the socket, costs no tokens, and is the half of "同步" that actually
  // changes between two presses (weather moves once an hour at most). Flag
  // only — poll() does the send when the socket can take it, and repeated
  // calls collapse into the one flag, so a page open that also syncs the
  // weather still sends a single request.
  session_.requestCronList();

  const OpenClaw::Session::State st = session_.state();
  // linkUp() needs no separate test: the session sets it true exactly when it
  // enters Connected and clears it on fail/close/disconnect, so Connected
  // covers "already up" — including the lost-socket window where state stays
  // Connected and poll() is already re-dialing on its own.
  const bool busy = st == OpenClaw::Session::State::Connecting || st == OpenClaw::Session::State::Handshake ||
                    st == OpenClaw::Session::State::Challenge || st == OpenClaw::Session::State::Connected;
  if (busy) return;

  if (!session_.loadConfig()) {
    // Config/token problems are the user's to fix in the web settings page;
    // the card already says 未连接, so log and stay passive.
    LOG_ERR("WBA", "gateway config refused: %s", session_.reason() != nullptr ? session_.reason() : "?");
    return;
  }
  if (!session_.startConnect()) {
    LOG_ERR("WBA", "gateway dial refused: %s", session_.reason() != nullptr ? session_.reason() : "?");
    return;
  }
  // The blocking TCP/TLS/upgrade runs on the next loop()'s session_.poll().
  LOG_INF("WBA", "gateway link requested");
}

bool WorkbenchActivity::weatherFreshWithin(const int64_t maxAgeMs) const {
  if (!haveWeather_ || weather_.fetchedAtMs <= 0) return false;
  // A pinned city outranks the clock: a cache written for another location (the
  // IP lookup said 南京 while the reader is in 常州) must not be called fresh,
  // or the card would keep the wrong city for the whole window. Both labels
  // count, so switching the UI language alone never forces a refetch.
  const WeatherCities::City* pinned = WeatherCities::find(SETTINGS.weatherCityKey);
  if (pinned != nullptr && std::strcmp(weather_.city, pinned->zh) != 0 && std::strcmp(weather_.city, pinned->en) != 0) {
    return false;
  }
  const int64_t nowMs = OpenClaw::nowEpochMs();
  // Unknown clock: age is unknowable, so treat the cache as stale — the
  // sync itself will seed the time from the gateway challenge (F3b).
  if (nowMs < OpenClaw::MIN_SANE_EPOCH_MS) return false;
  const int64_t age = nowMs - weather_.fetchedAtMs;
  return age >= 0 && age < maxAgeMs;
}

bool WorkbenchActivity::weatherFresh() const { return weatherFreshWithin(WEATHER_MAX_AGE_MS); }

// What a manual Sync press asks about the forecast: same city/clock guards,
// the 30 min window instead of the page-open 6 h one.
bool WorkbenchActivity::weatherManualFresh() const { return weatherFreshWithin(WEATHER_MANUAL_MAX_AGE_MS); }

void WorkbenchActivity::requestSync() {
  const bool netUp = WiFi.status() == WL_CONNECTED;
  // The same moment arms both halves of "同步": the HTTP weather round below
  // and the to-do card's socket. No-op when the link is already up — and the
  // to-do list is requested unconditionally by ensureGatewayLink(), so the
  // second half of a Sync press is never skipped.
  if (netUp) ensureGatewayLink();
  if (sync_ != Sync::Idle) return;  // one round at a time
  if (!netUp) {
    // Same detour Voice takes: WifiSelectionActivity auto-connects the saved
    // network on entry and the round resumes from the result callback.
    LOG_INF("WBA", "no network, opening Wi-Fi selection for weather sync");
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) return;  // stay passive, cache unchanged
                             requestSync();
                           });
    return;
  }
  syncError_ = nullptr;
  // The forecast half of the press, gated: within half an hour the page has
  // the same numbers it would fetch again, and the round (ip-api + a server
  // side web_fetch) is the only part of a Sync that costs network. The list
  // above is what the press is really for.
  if (weatherManualFresh()) {
    LOG_INF("WBA", "weather fresh, sync skips the HTTP round (to-do list refreshed over WS)");
    return;
  }
  syncStartMs_ = millis();
  geo_.ok = false;
  haveGeo_ = false;
  // The invoke step needs the gateway host + token. ensureGatewayLink() reads
  // NVS only when it actually dials, and an already-up link skips that, so
  // read it here too — CloudConfig::load() is idempotent.
  CLOUD_CFG.load();
  // Shared token only: the HTTP plane takes the operator Bearer (§11.2);
  // deviceToken is the WebSocket handshake's credential, not this one's.
  if (CLOUD_CFG.host[0] == '\0' || CLOUD_CFG.token[0] == '\0') {
    syncFailed(tr(STR_WORKBENCH_SYNC_FAIL));
    return;
  }
  sync_ = Sync::Geo;
  // Step 1 can be skipped entirely: when the user pinned a city in Settings
  // its coordinates and label are already known, so the ip-api round (which
  // answers 南京 for this device anyway) buys nothing. Jumping straight to
  // Fetch is the whole point of the built-in table.
  const WeatherCities::City* pinned = WeatherCities::find(SETTINGS.weatherCityKey);
  if (pinned != nullptr) {
    geo_.ok = true;
    geo_.lat = pinned->lat;
    geo_.lon = pinned->lon;
    std::snprintf(geo_.city, sizeof(geo_.city), "%s",
                  WeatherCities::label(*pinned, I18n::isChinese(I18N.getLanguage())));
    haveGeo_ = true;
    sync_ = Sync::Fetch;
    LOG_INF("WBA", "weather sync requested (city table: %.2f,%.2f %s)", geo_.lat, geo_.lon, geo_.city);
  } else {
    LOG_INF("WBA", "weather sync requested (ip geo first)");
  }
  requestUpdate();  // card flips to the syncing line
}

void WorkbenchActivity::syncFailed(const char* reason) {
  sync_ = Sync::Idle;
  syncError_ = reason != nullptr ? reason : tr(STR_WORKBENCH_SYNC_FAIL);
  LOG_ERR("WBA", "weather sync failed: %s", syncError_);
  requestUpdate();
}

// Drives one sync round. Called from loop() only. Both HTTP steps are
// blocking esp_http_client calls on the main task — acceptable here because
// the page promises "正在同步" and each step runs at most once per round
// (see Sync enum comment for why this is NOT driven from notify()).
void WorkbenchActivity::advanceSync() {
  if (sync_ == Sync::Idle) return;
  if (static_cast<uint32_t>(millis() - syncStartMs_) > SYNC_TOTAL_MS) {
    syncFailed(tr(STR_WORKBENCH_SYNC_FAIL));
    return;
  }

  if (sync_ == Sync::Geo) {
    // Step 1: this device's own public-IP location. lang follows the UI so
    // Chinese UIs get 省/市 as 汉字 straight from the source.
    char lang[IpGeo::LANG_CODE_SIZE];
    IpGeo::langCodeFor(LANGUAGE_CODES[static_cast<int>(I18N.getLanguage())], lang, sizeof(lang));
    char url[128];
    snprintf(url, sizeof(url), GEO_URL_FMT, lang);
    LOG_INF("WBA", "geo: %s", url);
    std::string resp;
    if (!HttpDownloader::fetchUrl(url, resp) || !OpenClaw::parseIpGeoJson(resp.data(), resp.size(), geo_) || !geo_.ok ||
        geo_.lat < GEO_LAT_MIN || geo_.lat > GEO_LAT_MAX || geo_.lon < GEO_LON_MIN || geo_.lon > GEO_LON_MAX) {
      LOG_ERR("WBA", "ip geo failed (%u bytes)", static_cast<unsigned>(resp.size()));
      syncFailed(tr(STR_WORKBENCH_SYNC_FAIL));
      return;
    }
    haveGeo_ = true;
    LOG_INF("WBA", "geo ok: %s %s %.2f,%.2f", geo_.country, geo_.city, geo_.lat, geo_.lon);
    sync_ = Sync::Fetch;
    return;
  }

  // Sync::Fetch — step 2: the open-meteo forecast for the geo coordinates,
  // via the gateway's tools/invoke web_fetch (Bearer = the gateway token
  // loadConfig() already reads). Body measured ~5 KB, ~1.3 s server-side.
  // The body is built by the tested formatter in WeatherFormat — an inline
  // snprintf with a guessed cap was what made every sync die here in silence
  // (see the caps' comment in WeatherFormat.h).
  // Heap, not stack: WEATHER_INVOKE_BODY_CAP (320 B) is over the <256 B
  // local budget, and makeUniqueNoThrow keeps the returns below leak-free.
  auto body = makeUniqueNoThrow<char[]>(OpenClaw::WEATHER_INVOKE_BODY_CAP);
  if (!body) {
    LOG_ERR("WBA", "OOM: invoke body");
    syncFailed(tr(STR_WORKBENCH_SYNC_FAIL));
    return;
  }
  const size_t bodyLen =
      OpenClaw::formatWeatherInvokeBody(body.get(), OpenClaw::WEATHER_INVOKE_BODY_CAP, geo_.lat, geo_.lon);
  if (bodyLen == 0) {
    LOG_ERR("WBA", "invoke body refused (%.2f, %.2f)", geo_.lat, geo_.lon);
    syncFailed(tr(STR_WORKBENCH_SYNC_FAIL));
    return;
  }
  std::string resp;
  if (!HttpDownloader::postJson(invokeUrl(), std::string(body.get(), bodyLen), CLOUD_CFG.token, resp,
                                INVOKE_TIMEOUT_MS)) {
    LOG_ERR("WBA", "tools/invoke request failed");
    syncFailed(tr(STR_WORKBENCH_SYNC_FAIL));
    return;
  }
  LOG_INF("WBA", "tools/invoke reply: %u bytes", static_cast<unsigned>(resp.size()));
  OpenClaw::WeatherSnapshot snapshot{};
  if (!OpenClaw::parseWeatherInvokeResponse(resp.data(), resp.size(), geo_.city, snapshot)) {
    LOG_ERR("WBA", "invoke reply is not the weather contract (%u bytes)", static_cast<unsigned>(resp.size()));
    syncFailed(tr(STR_WORKBENCH_SYNC_FAIL));
    return;
  }
  snapshot.fetchedAtMs = OpenClaw::nowEpochMs();
  weather_ = snapshot;  // whole-struct publish before the flag (see WorkbenchData)
  haveWeather_ = true;
  saveWeatherCache(snapshot);
  sync_ = Sync::Idle;
  syncError_ = nullptr;
  LOG_INF("WBA", "weather ok: %s %d/%d code=%s", weather_.city, weather_.today.hi, weather_.today.lo,
          weather_.today.cond);
  requestUpdate();
}

void WorkbenchActivity::loadWeatherCache() {
  weather_ = OpenClaw::WeatherSnapshot{};
  haveWeather_ = false;
  // Heap, not stack: WEATHER_CACHE_CAP (768 B) blows the <256 B local budget;
  // makeUniqueNoThrow keeps the early returns leak-free.
  auto buf = makeUniqueNoThrow<char[]>(OpenClaw::WEATHER_CACHE_CAP);
  if (!buf) {
    LOG_ERR("WBA", "OOM: weather cache read");
    return;
  }
  const size_t n = Storage.readFileToBuffer(WEATHER_CACHE_PATH, buf.get(), OpenClaw::WEATHER_CACHE_CAP);
  if (n == 0) return;  // first run — the empty state says so
  if (!OpenClaw::parseWeatherJson(buf.get(), n, weather_)) {
    LOG_ERR("WBA", "weather cache unreadable (%u bytes)", static_cast<unsigned>(n));
    weather_ = OpenClaw::WeatherSnapshot{};
    return;
  }
  haveWeather_ = true;
  LOG_INF("WBA", "weather cache: %s %d/%d, stamp %lld", weather_.city, weather_.today.hi, weather_.today.lo,
          static_cast<long long>(weather_.fetchedAtMs));
}

void WorkbenchActivity::saveWeatherCache(const OpenClaw::WeatherSnapshot& snapshot) {
  auto buf = makeUniqueNoThrow<char[]>(OpenClaw::WEATHER_CACHE_CAP);
  if (!buf) {
    LOG_ERR("WBA", "OOM: weather cache write");
    return;
  }
  const size_t n = OpenClaw::formatWeatherJson(buf.get(), OpenClaw::WEATHER_CACHE_CAP, snapshot);
  if (n == 0) {
    LOG_ERR("WBA", "weather cache does not fit %u bytes", static_cast<unsigned>(OpenClaw::WEATHER_CACHE_CAP));
    return;
  }
  if (!Storage.ensureDirectoryExists(WEATHER_CACHE_DIR)) {
    LOG_ERR("WBA", "mkdir failed: %s", WEATHER_CACHE_DIR);
    return;
  }
  HalFile f = Storage.open(WEATHER_CACHE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (!f.isOpen()) {
    LOG_ERR("WBA", "open for write failed: %s", WEATHER_CACHE_PATH);
    return;
  }
  const size_t written = f.write(buf.get(), n);
  if (written != n) {
    LOG_ERR("WBA", "short weather cache write %u/%u", static_cast<unsigned>(written), static_cast<unsigned>(n));
    return;
  }
  LOG_INF("WBA", "weather cached (%u bytes)", static_cast<unsigned>(n));
}

// Side keys step the selection along the layout's reading order (render()
// publishes that order in cardOrder): a portrait page is one column, a
// landscape page two, and "previous in reading order" is the right question
// to ask of both without an orientation branch. With nothing selected yet the
// first press lands on the first card, so the key is never a dead press.
void WorkbenchActivity::moveSelection(const int dir) {
  int rank = -1;
  for (int i = 0; i < 4; ++i) {
    if (cardOrder[i] == selectedCard) {
      rank = i;
      break;
    }
  }
  const int nextRank = rank < 0 ? 0 : (rank + dir + 4) % 4;
  const int8_t next = static_cast<int8_t>(cardOrder[nextRank]);
  if (next == selectedCard) return;
  selectedCard = next;
  requestUpdate();
}

// Front Left/Right belong to the selected card; on the to-do card they walk
// the window by hand, wrapping at both ends. The ticker's timer restarts so
// the next automatic step does not undo the press a moment later.
void WorkbenchActivity::scrollTodoWindow(const int dir) {
  if (selectedCard != 2) return;
  const size_t count = OpenClaw::Session::cronJobCount();
  if (!OpenClaw::Session::cronValid() || count <= static_cast<size_t>(kTodoWindowRows)) return;
  const size_t maxFirst = count - static_cast<size_t>(kTodoWindowRows);
  size_t next;
  if (dir < 0) {
    next = todoFirstRow == 0 ? maxFirst : todoFirstRow - 1;
  } else {
    next = todoFirstRow >= maxFirst ? 0 : todoFirstRow + 1;
  }
  if (next == todoFirstRow) return;
  todoFirstRow = next;
  lastTodoScrollMs = millis();
  requestUpdate();
}

// What Confirm does for the card under the selection: the to-do card talks to
// the gateway, the reading card opens the book, everything else (including no
// selection at all) is 同步 — the behaviour the page had before it had cards.
void WorkbenchActivity::activateSelected() {
  switch (selectedCard) {
    case 2: {
      // One voice round trip: the transcript is sent to the gateway exactly
      // as the AI助手 screen sends it, so "明早八点提醒我" lands as a new
      // automation. startActivityForResult keeps this page on the stack, and
      // the list is pulled twice on the way back — immediately, and once more
      // when VOICE_TODO_REFRESH_MS expires in case the job was created after
      // the reply.
      auto voice = std::make_unique<VoiceActivity>(renderer, mappedInput);
      startActivityForResult(std::move(voice), [this](const ActivityResult&) {
        // VoiceActivity takes the session notifier in its onEnter and clears
        // it in onExit(), which ActivityManager::loop() runs before this
        // handler — re-arm ours before anything else can arrive.
        session_.setNotifier(this, &WorkbenchActivity::onSessionNotifyTrampoline);
        session_.requestCronList();
        cronRefreshQueued_ = true;
        cronRefreshAtMs_ = millis() + VOICE_TODO_REFRESH_MS;
      });
      return;
    }
    case 3:
      // The card shows RECENT_BOOKS' front entry; open that same one rather
      // than re-deriving anything (a book added since the last render is
      // simply the new front entry, which is what the card would show).
      if (RECENT_BOOKS.getCount() > 0) {
        activityManager.goToReader(RECENT_BOOKS.getBooks().front().path);
      }
      return;
    default:
      requestSync();
      return;
  }
}

void WorkbenchActivity::loop() {
  // One-shot second list pull after a voice "add a to-do" round trip.
  if (cronRefreshQueued_ && static_cast<int32_t>(millis() - cronRefreshAtMs_) >= 0) {
    cronRefreshQueued_ = false;
    session_.requestCronList();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // Back drops a selection first and only then leaves the page — one key
    // must not both cancel and exit in the same press.
    if (selectedCard >= 0) {
      selectedCard = -1;
      requestUpdate();
      return;
    }
    activityManager.popActivity();  // last activity on the stack -> goHome()
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    moveSelection(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    moveSelection(1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Unselected, the front pair navigates the cards — the side keys do the
    // same, but nothing on screen can advertise keys it has no room for.
    if (selectedCard < 0) {
      moveSelection(-1);
    } else {
      scrollTodoWindow(-1);
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (selectedCard < 0) {
      moveSelection(1);
    } else {
      scrollTodoWindow(1);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // On RELEASE, not press — the same convention Home opens a book with
    // (HomeActivity.cpp:182). Activating on the press leaves the matching
    // release for whoever is current next, and the reader's own menu opens
    // on wasReleased(Confirm) (EpubReaderActivity.cpp:386): the book would
    // come up and its menu would follow it a beat later.
    activateSelected();
  }
  // Pump the shared session: heartbeat + disconnect detection keep the status
  // card honest while this page is frontmost (nothing polls it elsewhere),
  // and the sync state machine advances here — never in the notify callback,
  // which runs inside the WebSocket dispatch where sendTXT is forbidden.
  session_.poll();
  advanceSync();
  // The gateway challenge seeds the system clock asynchronously; if this page
  // rendered before that happened, redraw once so the date line fills in.
  if (!clockValidAtRender && NetBootstrap::systemTimeValid()) {
    clockValidAtRender = true;
    requestUpdate();
  }
  // Periodic tick: the clock band is static between renders, so without this it
  // freezes on whatever the last render showed. Capped at CLOCK_TICK_MS between
  // redraws (unsigned subtract is millis()-wraparound safe). Gated on
  // clockValidAtRender so an unsynced clock never drives pointless e-ink
  // refreshes; the one-shot above takes over until the gateway seeds the time.
  if (clockValidAtRender && static_cast<uint32_t>(millis() - lastClockTickMs) >= CLOCK_TICK_MS) {
    lastClockTickMs = millis();
    requestUpdate();
  }
  // To-do ticker: a list longer than the window creeps up one row per step,
  // dwelling on the last window before wrapping to the top. Runs only while
  // this page owns the screen (loop() stops when it doesn't) and only when
  // there is actually something to scroll — a short list repaints never.
  // The card is a static snapshot, so the refresh has to be timer-driven for
  // the same reason the clock band is (see CLOCK_TICK_MS).
  const size_t todoCount = OpenClaw::Session::cronJobCount();
  if (OpenClaw::Session::cronValid() && todoCount > static_cast<size_t>(kTodoWindowRows)) {
    const size_t maxFirst = todoCount - static_cast<size_t>(kTodoWindowRows);
    // A list that shrank under the ticker lands past maxFirst; atEnd then
    // wraps it to 0 on the next step, so no separate resync is needed.
    const bool atEnd = todoFirstRow >= maxFirst;
    const uint32_t wait = atEnd ? TODO_SCROLL_HOLD_MS : TODO_SCROLL_STEP_MS;
    if (static_cast<uint32_t>(millis() - lastTodoScrollMs) >= wait) {
      lastTodoScrollMs = millis();
      todoFirstRow = atEnd ? 0 : todoFirstRow + 1;
      requestUpdate();
    }
  }
}

void WorkbenchActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pw = renderer.getScreenWidth();
  const int ph = renderer.getScreenHeight();
  const int pad = metrics.contentSidePadding;
  const int gap = metrics.verticalSpacing;

  WorkbenchData d;
  const int64_t nowMs = OpenClaw::nowEpochMs();
  const uint8_t offQ = SETTINGS.clockUtcOffsetQ;
  d.clockValid = nowMs >= OpenClaw::MIN_SANE_EPOCH_MS;
  if (d.clockValid) {
    ChatHistoryFormat::formatTimeForOffset(d.timeBuf, sizeof(d.timeBuf), nowMs, offQ);
    const int64_t localSec = ChatHistoryFormat::localEpochSeconds(nowMs, offQ);
    const int64_t days = localSec >= 0 ? localSec / 86400 : (localSec - 86399) / 86400;
    int year = 0;
    int month = 0;
    int day = 0;
    ChatHistoryFormat::civilFromDays(days, year, month, day);
    const int wday = static_cast<int>(((days % 7) + 4) % 7);  // 1970-01-01 was a Thursday
    char dateBuf[32];
    snprintf(dateBuf, sizeof(dateBuf), tr(STR_WORKBENCH_DATE_FMT), year, month, day);
    snprintf(d.dateOnly, sizeof(d.dateOnly), "%s", dateBuf);
    snprintf(d.weekday, sizeof(d.weekday), "%s", weekdayStr(wday));
    snprintf(d.dateLine, sizeof(d.dateLine), "%s %s", d.dateOnly, d.weekday);
  }
  clockValidAtRender = d.clockValid;

  d.linkUp = OpenClaw::Session::linkUp();
  // Weather card inputs. haveWeather_ is read first so a sync that lands
  // mid-render can only cost one stale frame (publish order in advanceSync).
  d.haveWeather = haveWeather_;
  d.syncActive = sync_ != Sync::Idle;
  d.syncError = syncError_;
  d.weather = weather_;
  d.todoFirst = todoFirstRow;
  // "运行时间" is the GATEWAY's uptime, not this e-reader's — the card reports
  // whether the OpenClaw service is alive, and how long *this device* has been
  // on says nothing about that. Source is the one AIWatch_Ver2.0's web
  // dashboard renders: `payload.snapshot.uptimeMs` of the connect response
  // (Handshake::snapshotUptimeMs), which Session::gatewayUptimeMs() then
  // extrapolates with local time so the seconds tick between handshakes —
  // the gateway's pushed health event has no uptime field at all (probed
  // 2026-10-02), which is why the earlier health-based read showed nothing.
  // Hours are uncapped so a long-lived service prints "720:15:33" instead of
  // wrapping a byte-short. Zero means no snapshot has landed yet: show the
  // unknown marker rather than a confident 00:00:00 that reads as a
  // crash-loop.
  const uint64_t gwUpMs = OpenClaw::Session::gatewayUptimeMs();
  if (gwUpMs == 0) {
    snprintf(d.uptime, sizeof(d.uptime), "%s", "--:--:--");
  } else {
    const uint64_t upS = gwUpMs / 1000;
    snprintf(d.uptime, sizeof(d.uptime), "%02lu:%02lu:%02lu", static_cast<unsigned long>(upS / 3600),
             static_cast<unsigned long>((upS / 60) % 60), static_cast<unsigned long>(upS % 60));
  }

  d.book = RECENT_BOOKS.getCount() > 0 ? &RECENT_BOOKS.getBooks().front() : nullptr;
  if (d.book != nullptr && !d.book->coverBmpPath.empty()) {
    d.coverThumb = UITheme::getCoverThumbPath(d.book->coverBmpPath, metrics.homeCoverHeight);
    d.hasCover = Storage.exists(d.coverThumb.c_str());
  }

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pw, metrics.headerHeight}, tr(STR_WORKBENCH));

  const int contentX = pad;
  const int contentW = pw - pad * 2;
  const int topY = metrics.topPadding + metrics.headerHeight + gap;
  const int bottomY = ph - metrics.buttonHintsHeight - gap;
  const int availH = bottomY - topY;

  const int hs[4] = {clockBandH(renderer), weatherCardH(renderer), todosCardH(renderer),
                     readingCardH(renderer, d.hasCover)};
  CardBox boxes[4];
  if (pw <= ph) {
    const int sum = hs[0] + hs[1] + hs[2] + hs[3];
    const double scale = clampScale(static_cast<double>(availH - gap * 3) / sum);
    int y = topY;
    for (int i = 0; i < 4; ++i) {
      const int h = static_cast<int>(hs[i] * scale + 0.5);
      boxes[i] = CardBox{contentX, y, contentW, h};
      if (boxes[i].y + boxes[i].h > bottomY) boxes[i].h = bottomY - boxes[i].y;
      y += h + gap;
    }
  } else {
    // Landscape: clock+reading left, weather+todos right (per-column scale).
    const int colGap = gap;
    const int colW = (contentW - colGap) / 2;
    const double sl = clampScale(static_cast<double>(availH - colGap) / (hs[0] + hs[3]));
    const double sr = clampScale(static_cast<double>(availH - colGap) / (hs[1] + hs[2]));
    const int h0 = static_cast<int>(hs[0] * sl + 0.5);
    const int h1 = static_cast<int>(hs[1] * sr + 0.5);
    const int h2 = static_cast<int>(hs[2] * sr + 0.5);
    const int h3 = static_cast<int>(hs[3] * sl + 0.5);
    boxes[0] = CardBox{contentX, topY, colW, h0};
    boxes[3] = CardBox{contentX, topY + h0 + gap, colW, h3};
    boxes[1] = CardBox{contentX + colW + colGap, topY, colW, h1};
    boxes[2] = CardBox{contentX + colW + colGap, topY + h1 + gap, colW, h2};
    if (boxes[3].y + boxes[3].h > bottomY) boxes[3].h = bottomY - boxes[3].y;
    if (boxes[2].y + boxes[2].h > bottomY) boxes[2].h = bottomY - boxes[2].y;
  }

  drawClockBand(renderer, boxes[0], d);
  drawWeatherCard(renderer, boxes[1], d);
  drawTodosCard(renderer, boxes[2], d);
  drawReadingCard(renderer, boxes[3], d);

  // Reading order (top to bottom, left to right inside a row) for the card
  // selection: the index order already is that order in portrait, but the two
  // landscape columns have independent heights, so box 3 can sit above box 2.
  // A hand-rolled insertion sort of four entries — std::stable_sort would
  // heap-allocate a scratch buffer to do this.
  int order[4] = {0, 1, 2, 3};
  for (int i = 1; i < 4; ++i) {
    const int key = order[i];
    int j = i - 1;
    while (j >= 0 && (boxes[order[j]].y > boxes[key].y ||
                      (boxes[order[j]].y == boxes[key].y && boxes[order[j]].x > boxes[key].x))) {
      order[j + 1] = order[j];
      --j;
    }
    order[j + 1] = key;
  }
  for (int i = 0; i < 4; ++i) cardOrder[i] = static_cast<uint8_t>(order[i]);

  const int sel = selectedCard;
  if (sel >= 0 && sel < 4) drawSelectionFrame(renderer, boxes[sel]);

  // The bottom row follows the selection. 返回 sits in the first slot in both
  // states: Back drops a selection before it leaves the page, so there is no
  // moment where that key reads as 取消. One 切换卡片 label, on the front key
  // that walks the cards forward — a second identical label on its neighbour
  // only reads as duplication — and once a card is picked the front pair has
  // nothing to advertise: the to-do card scrolls itself, the rest do nothing
  // with those keys.
  const char* backHint = tr(STR_BACK);
  const char* confirmHint = tr(STR_WORKBENCH_SYNC);
  const char* leftHint = "";
  const char* rightHint = "";
  if (sel < 0) {
    rightHint = tr(STR_WORKBENCH_SWITCH_CARD);
  } else if (sel == 2) {
    confirmHint = tr(STR_WORKBENCH_ADD_TODO);
  } else if (sel == 3 && d.book != nullptr) {
    confirmHint = tr(STR_OPEN);
  }
  const auto labels = mappedInput.mapLabels(backHint, confirmHint, leftHint, rightHint);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
