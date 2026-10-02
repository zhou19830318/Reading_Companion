#include "InputManager.h"

// Recorded ADC values from real devices
// BACK CONF LEFT RGHT   UP DOWN
// 3597 2760 1530    6 2300    6
// 3470 2666 1480    6 2222    5
// 3470 2655 1470    3 2205    3

// Averages
// BACK CONF LEFT RGHT   UP DOWN
// 3512 2694 1493    5 2242    5

// Setup ranges, if ADC value is between value `i` and `i + 1`, button `i` is being pressed
// These ranges are based on real world values above, and are much more tolerant of different
// devices than a fixed threshold check
// These values are calculated by taking the midpoint of the pairs of averaged values above
const int InputManager::ADC_RANGES_1[] = {ADC_NO_BUTTON, 3100, 2090, 750, INT32_MIN};
const int InputManager::ADC_RANGES_2[] = {ADC_NO_BUTTON, 1120, INT32_MIN};
const char* InputManager::BUTTON_NAMES[] = {"Back", "Confirm", "Left", "Right", "Up", "Down", "Power"};

InputManager::InputManager()
    : currentState(0),
      lastState(0),
      pressedEvents(0),
      releasedEvents(0),
      lastDebounceTime(0),
      buttonPressStart(0),
      buttonPressFinish(0),
      powerButtonPressStart(0),
      powerButtonPressFinish(0) {}

void InputManager::begin() {
#ifdef ONEPAGE_C61
  analogSetAttenuation(ADC_11db);
  pinMode(BTN_UP_PIN, INPUT_PULLUP);
  pinMode(BTN_DOWN_PIN, INPUT_PULLUP);
  pinMode(POWER_BUTTON_PIN, INPUT_PULLUP);
#else
  pinMode(BUTTON_ADC_PIN_1, INPUT);
  pinMode(BUTTON_ADC_PIN_2, INPUT);
  pinMode(POWER_BUTTON_PIN, INPUT_PULLUP);
  analogSetAttenuation(ADC_11db);
#endif
}

int InputManager::getButtonFromADC(const int adcValue, const int ranges[], const int numButtons) {
  for (int i = 0; i < numButtons; i++) {
    if (ranges[i + 1] < adcValue && adcValue <= ranges[i]) {
      return i;
    }
  }

  return -1;
}

uint8_t InputManager::getState() {
  uint8_t state = 0;

#ifdef ONEPAGE_C61
  // OnePage: 4-key front ADC ladder (GPIO4) -> BACK/CONFIRM(ENTER)/LEFT/RIGHT,
  // plus 3 side GPIO keys (active-low) -> UP(PREV)/DOWN(NEXT)/POWER(WAKE).
  // Use calibrated mV directly (rest ~3100 = no key; ENTER is a 0-ohm short ~0 mV).
  const int mv = analogReadMilliVolts(BUTTON_ADC_PIN);
  if (mv >= 2400 && mv <= 2800)      state |= (1 << BTN_BACK);    // ~2592
  else if (mv >= 1780 && mv <= 2140) state |= (1 << BTN_LEFT);    // ~1956
  else if (mv >= 1140 && mv <= 1500) state |= (1 << BTN_RIGHT);   // ~1316
  else if (mv >= 0 && mv <= 250)     state |= (1 << BTN_CONFIRM); // ~0 (ENTER)

  if (digitalRead(BTN_UP_PIN) == LOW)       state |= (1 << BTN_UP);
  if (digitalRead(BTN_DOWN_PIN) == LOW)     state |= (1 << BTN_DOWN);
  if (digitalRead(POWER_BUTTON_PIN) == LOW) state |= (1 << BTN_POWER);
  return state;
#else
  // Read GPIO1 buttons
  const int adcValue1 = analogRead(BUTTON_ADC_PIN_1);
  const int button1 = getButtonFromADC(adcValue1, ADC_RANGES_1, NUM_BUTTONS_1);
  if (button1 >= 0) {
    state |= (1 << button1);
  }

  // Read GPIO2 buttons
  const int adcValue2 = analogRead(BUTTON_ADC_PIN_2);
  const int button2 = getButtonFromADC(adcValue2, ADC_RANGES_2, NUM_BUTTONS_2);
  if (button2 >= 0) {
    state |= (1 << (button2 + 4));
  }

  // Read power button (digital, active LOW)
  if (digitalRead(POWER_BUTTON_PIN) == LOW) {
    state |= (1 << BTN_POWER);
  }

  return state;
#endif
}

void InputManager::update() {
  const unsigned long currentTime = millis();
  const uint8_t state = getState();

  // Always clear events first
  pressedEvents = 0;
  releasedEvents = 0;

  // Debounce
  if (state != lastState) {
    lastDebounceTime = currentTime;
    lastState = state;
  }

  if ((currentTime - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (state != currentState) {
      // Calculate pressed and released events
      pressedEvents = state & ~currentState;
      releasedEvents = currentState & ~state;

      // If pressing buttons and wasn't before, start recording time
      if (pressedEvents > 0 && currentState == 0) {
        buttonPressStart = currentTime;
      }

      // If releasing a button and no other buttons being pressed, record finish time
      if (releasedEvents > 0 && state == 0) {
        buttonPressFinish = currentTime;
      }

      // Track power button press time separately
      if (pressedEvents & (1 << BTN_POWER)) {
        powerButtonPressStart = currentTime;
      }

      // Track power button release
      if (releasedEvents & (1 << BTN_POWER)) {
        powerButtonPressFinish = currentTime;
      }

      currentState = state;
    }
  }
}

bool InputManager::isPressed(const uint8_t buttonIndex) const {
  return currentState & (1 << buttonIndex);
}

bool InputManager::wasPressed(const uint8_t buttonIndex) const {
  return pressedEvents & (1 << buttonIndex);
}

void InputManager::injectPressedEvents(const uint8_t buttonMask) {
  // OR into this frame's events; also set currentState so isPressed() agrees for
  // this frame. Both are recomputed from hardware on the next update().
  const bool wasIdle = currentState == 0;
  pressedEvents |= buttonMask;
  currentState |= buttonMask;
  // Timestamp the synthetic press the way a hardware edge would: getHeldTime()
  // is read by every long-press consumer (chapter skip, file delete, bookmark,
  // go-home) and would otherwise report the time since the last *real* press —
  // i.e. essentially always a long press. The matching release is produced by
  // the next update(), which sets buttonPressFinish as it does for hardware.
  if (wasIdle) {
    buttonPressStart = millis();
  }
}

bool InputManager::wasAnyPressed() const {
  return pressedEvents > 0;
}

bool InputManager::wasReleased(const uint8_t buttonIndex) const {
  return releasedEvents & (1 << buttonIndex);
}

bool InputManager::wasAnyReleased() const {
  return releasedEvents > 0;
}

unsigned long InputManager::getHeldTime() const {
  // Still hold a button
  if (currentState > 0) {
    return millis() - buttonPressStart;
  }

  return buttonPressFinish - buttonPressStart;
}

unsigned long InputManager::getPowerButtonHeldTime() const {
  // Power button is currently pressed
  if (isPressed(BTN_POWER)) {
    return millis() - powerButtonPressStart;
  }

  // Power button was released
  return powerButtonPressFinish - powerButtonPressStart;
}

const char* InputManager::getButtonName(const uint8_t buttonIndex) {
  if (buttonIndex <= BTN_POWER) {
    return BUTTON_NAMES[buttonIndex];
  }
  return "Unknown";
}

bool InputManager::isPowerButtonPressed() const {
  return isPressed(BTN_POWER);
}
