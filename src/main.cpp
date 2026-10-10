#include <Arduino.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>
#include <esp32-hal-psram.h>
#include <esp_timer.h>

#include <cstring>

#include "CjkUiFontSystem.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#if defined(ONEPAGE_C61) && defined(CONFIG_BT_ENABLED)
#include "bluetooth/BleHidHost.h"
#endif
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    // Fast path for short press
    // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
    return;
  }

  // Give the user up to 1000ms to start holding the power button, and must hold for SETTINGS.getPowerButtonDuration()
  const auto start = millis();
  bool abort = false;
  // Subtract the current time, because inputManager only starts counting the HeldTime from the first update()
  // This way, we remove the time we already took to reach here from the duration,
  // assuming the button was held until now from millis()==0 (i.e. device start time).
  const uint16_t calibration = start;
  const uint16_t calibratedPressDuration =
      (calibration < SETTINGS.getPowerButtonDuration()) ? SETTINGS.getPowerButtonDuration() - calibration : 1;

  gpio.update();
  // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    gpio.update();
  }

  t2 = millis();
  if (gpio.isPressed(HalGPIO::BTN_POWER)) {
    do {
      delay(10);
      gpio.update();
    } while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() < calibratedPressDuration);
    abort = gpio.getPowerButtonHeldTime() < calibratedPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    powerManager.startDeepSleep(gpio);
  }
}
void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isQuickResumeSleep;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

#if defined(ONEPAGE_C61) && defined(CONFIG_BT_ENABLED)
  // Tear down BLE before sleep so the BT power domain is released (and the
  // controller isn't left running against a cut power rail on wake).
  if (BLE_HID_HOST.isStarted()) {
    BLE_HID_HOST.stop();
  }
#endif

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

#ifdef ONEPAGE_C61
  // Load the CJK UI font from SD and wire it as a glyph-miss fallback into the
  // builtin UI fonts, so Chinese UI labels — and the "简体中文"/"繁體中文" entries
  // in the language picker — render even before Chinese is selected. Absent
  // font file degrades gracefully (CJK shows as replacement boxes).
  if (cjkUiFontSystem.loadIfAvailable()) {
    cjkUiFontSystem.attachToUiFonts(ui10RegularFont, ui10BoldFont, ui12RegularFont, ui12BoldFont, smallFont);
    // The Workbench date line draws 年月日/星期 with Noto Serif 14 and the
    // weather line with Serif 16 — pure-Latin builtin faces, so without the
    // fallback they render replacement boxes. notoserif16* lives inside
    // OMIT_FONTS; serif14 (the date line, the actual bug) always exists.
    cjkUiFontSystem.attachTo(notoserif14RegularFont);
#ifndef OMIT_FONTS
    cjkUiFontSystem.attachTo(notoserif16RegularFont);
#endif
  }
#endif

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  t1 = millis();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif

  HalSystem::begin();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  powerManager.begin();
#ifndef ONEPAGE_C61
  halTiltSensor
      .begin();      // X4 IMU (QMI8658, I2C) — absent on OnePage; I2C probe (incl. GPIO0 strap) hangs -> CPU_LOCKUP
  halClock.begin();  // X4 RTC (DS3231, I2C) — absent on OnePage
#endif

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
#ifdef ONEPAGE_C61
  // OnePage bringup: force front buttons to a 1:1 layout (physical BACK/CONFIRM/
  // LEFT/RIGHT = logical), overriding any X4-specific mapping carried in a
  // settings.bin copied from another device.
  SETTINGS.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
  SETTINGS.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
  SETTINGS.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
  SETTINGS.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
#endif
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
#if defined(ONEPAGE_C61) && defined(CONFIG_BT_ENABLED)
  // BLE page-turner: only start at boot if the user's on/off switch is ON. Load
  // the bound device + learned patterns so it reconnects to the paired remote.
  if (SETTINGS.blePageTurnerOn) {
    BleReportPattern fwd;
    fwd.attrHandle = SETTINGS.bleFwdAttrHandle;
    fwd.len = SETTINGS.bleFwdLen;
    memcpy(fwd.bytes, SETTINGS.bleFwdBytes, sizeof(fwd.bytes));
    fwd.valid = SETTINGS.bleFwdLen > 0;
    BleReportPattern back;
    back.attrHandle = SETTINGS.bleBackAttrHandle;
    back.len = SETTINGS.bleBackLen;
    memcpy(back.bytes, SETTINGS.bleBackBytes, sizeof(back.bytes));
    back.valid = SETTINGS.bleBackLen > 0;
    BLE_HID_HOST.setPattern(BleHidAction::PageForward, fwd);
    BLE_HID_HOST.setPattern(BleHidAction::PageBack, back);
    // Only auto-reconnect the specific bound remote (not any nearby HID device).
    if (SETTINGS.bleDeviceValid) {
      BLE_HID_HOST.setBoundDevice(SETTINGS.bleDeviceAddr, SETTINGS.bleDeviceAddrType);
    }
    BLE_HID_HOST.start();
  }
#endif
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                   SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (loadSleepFrameBuffer()) {
        // Frame restored: swap the sleep moon for the loading icon.
        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
#if defined(ONEPAGE_C61) && defined(CONFIG_BT_ENABLED)
  // Feed BLE page-turner key events into the input pipeline as synthetic side-
  // button presses, right after the per-frame gpio.update() and before any
  // activity->loop() reads wasPressed(). Forward -> BTN_DOWN, back -> BTN_UP:
  // the BLE keys behave like the physical side buttons, so the reader's
  // PageForward/PageBack mapping (incl. sideButtonLayout swap) applies. No-op
  // when the BLE host isn't running (drainPendingActions returns 0).
  {
    const uint8_t bleActions = BLE_HID_HOST.drainPendingActions();
    if (bleActions & (1u << static_cast<uint8_t>(BleHidAction::PageForward))) {
      gpio.injectPress(HalGPIO::BTN_DOWN);
    }
    if (bleActions & (1u << static_cast<uint8_t>(BleHidAction::PageBack))) {
      gpio.injectPress(HalGPIO::BTN_UP);
    }
  }
#endif
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    // Raw heap_caps_* for the PSRAM columns, not ESP.getPsramSize()/FreePsram():
    // those gate on psramFound(), which reports 0 on this build even though the
    // SPIRAM heap is live (measured: OpenClawActivity's chat buffer is served
    // from MALLOC_CAP_SPIRAM while the same line prints PsramT: 0). A column
    // that always says "no PSRAM" would send every reader down the wrong path.
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes PsramT: %u PsramF: %u",
            ESP.getFreeHeap(), ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(),
            static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        uint8_t* buf = display.getFrameBuffer();
        uint32_t crc = 2166136261u;
        for (uint32_t i = 0; i < bufferSize; i++) crc = (crc ^ buf[i]) * 16777619u;
        logSerial.printf("FBPTR:%p CRC:%08X\n", static_cast<void*>(buf), crc);
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        // HWCDC's TX ring is 256 B (HWCDC::begin -> setTxBufferSize(256)) and
        // write() gives up the moment it fills, so one 48 KB write delivers only
        // the first ~256 B. Chunk to half the ring, retry whatever write() could
        // not take (the ISR drains 64 B per USB packet), and flush between
        // chunks so the next one always finds room. SCREENSHOT_SENT reports what
        // actually made it out — a short count means the host stopped reading.
        constexpr uint32_t CHUNK = 128;
        uint32_t sent = 0;
        for (uint32_t off = 0; off < bufferSize; off += CHUNK) {
          const uint32_t n = (bufferSize - off < CHUNK) ? bufferSize - off : CHUNK;
          uint32_t part = 0;
          uint32_t spins = 0;
          // Allow 1 s of no progress per chunk: observed stalls while another
          // task drives the panel/SD (flash cache off) run past the old 100 ms
          // budget, and a chunk abandoned halfway shortens the payload.
          while (part < n && spins < 1000) {
            const size_t written = logSerial.write(buf + off + part, n - part);
            if (written == 0) {
              delay(1);
              ++spins;
              continue;
            }
            part += static_cast<uint32_t>(written);
            spins = 0;
          }
          sent += part;
          logSerial.flush();
        }
        logSerial.printf("SCREENSHOT_SENT:%u\n", static_cast<unsigned>(sent));
        logSerial.printf("SCREENSHOT_END\n");
      } else if (cmd == "SDTEST") {
        // SD throughput vs CPU frequency. SdFat polls the wire byte by byte, so
        // the transfer rate may track the CPU rather than the SPI clock — and
        // main() drops the CPU to 10 MHz after IDLE_POWER_SAVING_MS (2 s) of
        // no *button* activity, which a serial command is not. Every earlier
        // probe therefore ran at 10 MHz. Run the same benchmark twice with the
        // frequency clamped by hand: if the full-speed pass is many times
        // faster, power management is what starves the card. Internal-DRAM
        // buffer so the source of the bytes cannot be a variable.
        constexpr size_t kPayload = 16 * 1024;
        uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(kPayload, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!raw) {
          logSerial.println("SDTEST OOM");
        } else {
          for (size_t i = 0; i < kPayload; i++) raw[i] = static_cast<uint8_t>(i * 31 + 7);
          const char* path = "/.crosspoint/_sdtest.bin";
          struct Shape {
            const char* name;
            size_t call;
          };
          static const Shape shapes[] = {{"1 x 16K", kPayload}, {"4 x 4K", 4096}, {"32 x 512", 512}};
          auto ms = [](int64_t us) { return static_cast<double>(us) / 1000.0; };
          auto kbs = [](size_t n, int64_t us) {
            return us > 0 ? static_cast<double>(n) / static_cast<double>(us) * 1e6 / 1024.0 : 0.0;
          };

          auto run = [&](const char* tag) {
            logSerial.printf("SDTEST [%s] cpu=%d MHz\n", tag, getCpuFrequencyMhz());
            // Bus ceiling with the card deselected (SdFat leaves CS high
            // between transactions, so the card ignores these clocks). Only the
            // SPI driver and the wire are in the path.
            {
              constexpr uint32_t kChunk = 1024;
              constexpr uint32_t kIters = 64;
              SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
              const int64_t t0 = esp_timer_get_time();
              for (uint32_t i = 0; i < kIters; i++) SPI.transferBytes(raw, raw + kChunk, kChunk);
              const int64_t t1 = esp_timer_get_time();
              SPI.endTransaction();
              const size_t n = kChunk * kIters;
              logSerial.printf("SDTEST [%s] spi-bulk CS-high %u B  %.1f ms (%.1f KB/s)\n", tag,
                               static_cast<unsigned>(n), ms(t1 - t0), kbs(n, t1 - t0));
              delay(1);
            }
            for (const Shape& s : shapes) {
              int64_t t0 = 0, t1 = 0, t2 = 0;
              size_t done = 0;
              bool opened = false;
              {
                HalFile f;
                opened = Storage.openFileForWrite("SDTEST", path, f);
                if (opened) {
                  t0 = esp_timer_get_time();
                  while (done < kPayload) {
                    const size_t n = (s.call < kPayload - done) ? s.call : kPayload - done;
                    if (f.write(raw, n) != n) break;
                    done += n;
                  }
                  t1 = esp_timer_get_time();
                  f.flush();
                  t2 = esp_timer_get_time();
                }
              }
              if (!opened) {
                logSerial.printf("SDTEST [%s] open failed\n", tag);
                return;
              }
              logSerial.printf("SDTEST [%s] write %-9s %u B  %.1f ms (%.1f KB/s)  flush %.1f ms\n", tag, s.name,
                               static_cast<unsigned>(done), ms(t1 - t0), kbs(done, t1 - t0), ms(t2 - t1));
              delay(1);
            }
            HalFile f;
            if (Storage.openFileForRead("SDTEST", path, f)) {
              const int64_t t0 = esp_timer_get_time();
              size_t rd = 0;
              while (rd < kPayload) {
                const size_t n = (4096 < kPayload - rd) ? 4096 : kPayload - rd;
                const int r = f.read(raw, n);
                if (r <= 0) break;
                rd += static_cast<size_t>(r);
              }
              const int64_t t1 = esp_timer_get_time();
              logSerial.printf("SDTEST [%s] read  4 x 4K    %u B  %.1f ms (%.1f KB/s)\n", tag,
                               static_cast<unsigned>(rd), ms(t1 - t0), kbs(rd, t1 - t0));
            } else {
              logSerial.printf("SDTEST [%s] read open failed\n", tag);
            }
            // Same payload written through a Print&, which is the path ZipFile
            // and every other Print-taking writer use. HalFile must override
            // Print's bulk write or this falls back to per-byte write(uint8_t).
            {
              HalFile f;
              if (Storage.openFileForWrite("SDTEST", path, f)) {
                Print& out = f;
                int64_t t0 = 0, t1 = 0;
                size_t done = 0;
                t0 = esp_timer_get_time();
                while (done < kPayload) {
                  const size_t n = (4096 < kPayload - done) ? 4096 : kPayload - done;
                  if (out.write(raw + done, n) != n) break;
                  done += n;
                }
                t1 = esp_timer_get_time();
                f.flush();
                logSerial.printf("SDTEST [%s] print  4 x 4K    %u B  %.1f ms (%.1f KB/s)\n", tag,
                                 static_cast<unsigned>(done), ms(t1 - t0), kbs(done, t1 - t0));
              } else {
                logSerial.printf("SDTEST [%s] print open failed\n", tag);
              }
            }
            delay(1);
          };

          powerManager.setPowerSaving(true);
          run("low");
          powerManager.setPowerSaving(false);
          run("full");

          Storage.remove(path);
          heap_caps_free(raw);
          logSerial.println("SDTEST_END");
        }
      } else if (cmd == "GO_VOICE") {
        // Debug: jump straight to the voice screen (no menu navigation).
        activityManager.goToVoice();
      } else if (cmd == "GO_WORKBENCH") {
        // Debug: jump straight to the workbench screen (no menu navigation).
        activityManager.goToWorkbench();
      } else if (cmd.startsWith("VOICE_TX ")) {
        // Debug: stand in for the STT step — sends `text` as one chat round
        // trip so the gateway's answer can be captured from serial without
        // speaking. Rejected while VoiceActivity is not current or is busy.
        String text = cmd.substring(9);
        text.trim();
        const bool accepted = activityManager.injectVoiceTranscript(text.c_str());
        logSerial.printf("VOICE_TX %s (%u bytes)\n", accepted ? "accepted" : "rejected",
                         static_cast<unsigned>(text.length()));
      } else if (cmd.startsWith("KEY ")) {
        // Debug: inject one synthetic press so device tests can drive the UI
        // without hands on the buttons. Same pipeline as the BLE page-turner
        // (InputManager::injectPressedEvents): pressed this frame, released on
        // the next update(), so wasReleased() consumers see a normal key.
        // The power key injects as a short tap: long-press sleep and the
        // POWER+DOWN screenshot combo need held frames, which one injected
        // press never lasts through — it reaches the reader's power branch
        // (short-press → SmartAsk menu) and nothing else.
        String key = cmd.substring(4);
        key.trim();
        int buttonIndex = -1;
        if (key == "back") {
          buttonIndex = HalGPIO::BTN_BACK;
        } else if (key == "ok") {
          buttonIndex = HalGPIO::BTN_CONFIRM;
        } else if (key == "left") {
          buttonIndex = HalGPIO::BTN_LEFT;
        } else if (key == "right") {
          buttonIndex = HalGPIO::BTN_RIGHT;
        } else if (key == "up") {
          buttonIndex = HalGPIO::BTN_UP;
        } else if (key == "down") {
          buttonIndex = HalGPIO::BTN_DOWN;
        } else if (key == "power") {
          buttonIndex = HalGPIO::BTN_POWER;
        }
        if (buttonIndex >= 0) {
          gpio.injectPress(static_cast<uint8_t>(buttonIndex));
          logSerial.printf("KEY %s injected\n", key.c_str());
        } else {
          logSerial.printf("KEY %s unknown (back|ok|left|right|up|down|power)\n", key.c_str());
        }
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (millis() >= allowSleepAt && !activityManager.suppressPowerSleep() && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // Inactive: drop the CPU frequency, which is where the power saving
      // actually comes from. The sleep used to be 50 ms here, which put up to
      // 50 ms between a key press and the gpio.update() that samples it — one
      // of the more visible menu lags — so it now matches the active branch.
      powerManager.setPowerSaving(true);
      delay(10);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
