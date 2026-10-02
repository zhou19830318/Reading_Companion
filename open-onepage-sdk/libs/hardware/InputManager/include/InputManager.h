#pragma once

#include <Arduino.h>

class InputManager {
 public:
  InputManager();
  void begin();
  uint8_t getState();

  /**
   * Updates the button states. Should be called regularly in the main loop.
   */
  void update();

  /**
   * Inject synthetic "just pressed" events for the given button bitmask, as if
   * those buttons were physically pressed this frame. Must be called from the
   * same task/context as update() (e.g. right after HalGPIO calls update()),
   * NOT from an ISR or another task — it is not synchronized. The injected bits
    * live for the current frame only; the next update() clears pressedEvents.
    * The injection also timestamps buttonPressStart when no button was down, so
    * getHeldTime() reports a ~1-frame hold instead of the time since the last
    * real press (otherwise every long-press consumer would see a long press).
    * Used by the BLE page-turner to feed page-turn key events into the normal
    * input pipeline so no activity code needs to change.
   *
   * @param buttonMask OR of (1 << BTN_*) bits to mark as just-pressed
   */
  void injectPressedEvents(uint8_t buttonMask);

  /**
   * Returns true if the button was being held at the time of the last #update() call.
   *
   * @param buttonIndex the button indexes
   * @return the button current press state
   */
  bool isPressed(uint8_t buttonIndex) const;

 /**
   * Returns true if the button went from unpressed to pressed between the last two #update() calls.
   *
   * This differs from #isPressed() in that pressing and holding a button will cause this function
   * to return true after the first #update() call, but false on subsequent calls, whereas #isPressed()
   * will continue to return true.
   *
   * @param buttonIndex
   * @return the button pressed state
   */
  bool wasPressed(uint8_t buttonIndex) const;

  /**
   * Returns true if any button started being pressed between the last two #update() calls
   *
   * @return true if any button started being pressed between the last two #update() calls
   */
  bool wasAnyPressed() const;

  /**
   * Returns true if the button went from pressed to unpressed between the last two #update() calls
   *
   * @param buttonIndex the button indexes
   * @return the button release state
   */
  bool wasReleased(uint8_t buttonIndex) const;

  /**
   * Returns true if any button was released between the last two #update() calls
   *
   * @return  true if any button was released between the last two #update() calls
   */
  bool wasAnyReleased() const;

  /**
   * Returns the time between any button starting to be depressed and all buttons between released
   *
   * @return duration in milliseconds
   */
  unsigned long getHeldTime() const;

    /**
   * Returns the time the power button has been held
   *
   * @return duration in milliseconds
   */
  unsigned long getPowerButtonHeldTime() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;

  // Pins
#ifdef ONEPAGE_C61
  static constexpr int BUTTON_ADC_PIN = 4;    // front 4-key ADC ladder (GPIO4 = ADC1_CH2)
  static constexpr int BTN_UP_PIN = 6;        // PREV side key (GPIO, active-low)
  static constexpr int BTN_DOWN_PIN = 9;      // NEXT side key (GPIO, active-low)
  static constexpr int POWER_BUTTON_PIN = 2;  // WAKE side key (also deep-sleep wake)
#else
  static constexpr int BUTTON_ADC_PIN_1 = 1;
  static constexpr int BUTTON_ADC_PIN_2 = 2;
  static constexpr int POWER_BUTTON_PIN = 3;
#endif

  // Power button methods
  bool isPowerButtonPressed() const;

  // Button names
  static const char* getButtonName(uint8_t buttonIndex);

 private:
  int getButtonFromADC(int adcValue, const int ranges[], int numButtons);

  uint8_t currentState;
  uint8_t lastState;
  uint8_t pressedEvents;
  uint8_t releasedEvents;
  unsigned long lastDebounceTime;
  unsigned long buttonPressStart;
  unsigned long buttonPressFinish;
  unsigned long powerButtonPressStart;
  unsigned long powerButtonPressFinish;


  static constexpr int NUM_BUTTONS_1 = 4;
  static const int ADC_RANGES_1[];

  static constexpr int NUM_BUTTONS_2 = 2;
  static const int ADC_RANGES_2[];

  static constexpr int ADC_NO_BUTTON = 3900;
  static constexpr unsigned long DEBOUNCE_DELAY = 5;

  static const char* BUTTON_NAMES[];
};
