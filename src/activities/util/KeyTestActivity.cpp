#include "KeyTestActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Long interval keeps e-ink refreshes sane while a key is held (repeat is
// still observable, just not at the 250 ms default used by reader screens).
constexpr uint16_t kTestRepeatIntervalMs = 1000;

constexpr MappedInputManager::Button kRowButtons[KeyTestActivity::ROW_COUNT] = {
    MappedInputManager::Button::Back,    MappedInputManager::Button::Left, MappedInputManager::Button::Right,
    MappedInputManager::Button::Confirm, MappedInputManager::Button::Up,   MappedInputManager::Button::Down,
    MappedInputManager::Button::Power,
};
}  // namespace

StrId KeyTestActivity::gestureStrId(const KeyGesture::Event event) {
  switch (event) {
    case KeyGesture::Event::LongStart:
      return StrId::STR_GESTURE_LONG;
    case KeyGesture::Event::Repeat:
      return StrId::STR_GESTURE_REPEAT;
    case KeyGesture::Event::ShortRelease:
      return StrId::STR_GESTURE_SHORT;
    case KeyGesture::Event::Release:
      return StrId::STR_GESTURE_RELEASE;
    case KeyGesture::Event::Press:
    case KeyGesture::Event::None:
      break;
  }
  return StrId::STR_GESTURE_PRESS;
}

void KeyTestActivity::onEnter() {
  Activity::onEnter();

  for (size_t i = 0; i < ROW_COUNT; ++i) {
    KeyGesture::Config config;
    config.repeatIntervalMs = kTestRepeatIntervalMs;
    config.enableRepeat = (i != BACK_ROW && i != ROW_COUNT - 1);  // power: long only
    rows[i] = Row{};
    rows[i].gesture = KeyGesture(kRowButtons[i], config);
  }

  requestUpdate();
}

void KeyTestActivity::loop() {
  bool dirty = false;

  for (size_t i = 0; i < ROW_COUNT; ++i) {
    Row& row = rows[i];
    const KeyGesture::Event event = row.gesture.update(mappedInput);
    if (event == KeyGesture::Event::None) {
      continue;
    }

    if (event == KeyGesture::Event::Press) {
      row.heldNow = true;
      dirty = true;
      continue;
    }

    const unsigned long held = mappedInput.getHeldTime();
    row.heldMs = static_cast<uint16_t>(held > 60000 ? 60000 : held);
    row.event = gestureStrId(event);
    row.hasEvent = true;
    if (event == KeyGesture::Event::ShortRelease || event == KeyGesture::Event::Release) {
      row.heldNow = false;
    }
    dirty = true;
    LOG_DBG("KEYTEST", "row %u event %d held %lu ms", static_cast<unsigned>(i), static_cast<int>(event), held);

    // Short-pressing Back exits (its own row doubles as the exit gesture);
    // long-press on Back still gets recorded before the release.
    if (i == BACK_ROW && event == KeyGesture::Event::ShortRelease) {
      finish();
      return;
    }
  }

  if (dirty) {
    requestUpdate();
  }
}

void KeyTestActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_KEY_TEST));

  // Column x must fit the LOGICAL canvas (480 wide in landscape orientation,
  // where logical x maps onto the panel's 480-pixel axis — an 800-wide layout
  // would be clipped off-canvas).
  constexpr int kStateCol = 170;
  constexpr int kEventCol = 270;
  const int startY = metrics.topPadding + metrics.headerHeight + 14;
  const int hintZone = 54;
  const int rowH = (pageHeight - startY - hintZone) / static_cast<int>(ROW_COUNT);

  static constexpr StrId kLabels[ROW_COUNT] = {
      StrId::STR_LBTN_BACK, StrId::STR_LBTN_LEFT, StrId::STR_LBTN_RIGHT, StrId::STR_LBTN_CONFIRM,
      StrId::STR_LBTN_UP,   StrId::STR_LBTN_DOWN, StrId::STR_LBTN_POWER,
  };

  char eventBuf[48];
  for (size_t i = 0; i < ROW_COUNT; ++i) {
    const Row& row = rows[i];
    const int y = startY + static_cast<int>(i) * rowH + rowH - 10;

    renderer.drawText(UI_10_FONT_ID, safe.x + 8, y, I18N.get(kLabels[i]), true);

    renderer.drawText(UI_10_FONT_ID, safe.x + kStateCol, y, row.heldNow ? tr(STR_GESTURE_PRESS) : "-", true);

    if (row.hasEvent) {
      snprintf(eventBuf, sizeof(eventBuf), "%s %ums", I18N.get(row.event), row.heldMs);
    } else {
      snprintf(eventBuf, sizeof(eventBuf), "%s", "-");
    }
    renderer.drawText(UI_10_FONT_ID, safe.x + kEventCol, y, eventBuf, true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
