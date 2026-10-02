#pragma once
#include <GfxRenderer.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct KeyDef {
  char primary;
  char secondary;
};

enum class SpecialKeyType { Shift, Mode, Space, Del, Ok };

enum class InputType { Text, Password, Url };

// Voice-dictation hooks: while editing a voice bookmark (语音书签) the power
// key is the talk key of the voice screen — it captures one utterance through
// the underlying VoiceActivity and the transcript is inserted at the cursor.
// All pointers stay null in ordinary text entry, where the power key keeps its
// global meaning (long press = sleep, POWER+DOWN = screenshot).
struct VoiceDictationHooks {
  void* ctx = nullptr;
  bool (*start)(void* ctx) = nullptr;         // begin capture; false = read error()
  int (*poll)(void* ctx) = nullptr;           // 0 = recording, 1 = ready, -1 = failed
  const char* (*text)(void* ctx) = nullptr;   // transcript, valid after poll returns 1
  const char* (*error)(void* ctx) = nullptr;  // reason, valid after a failed start/poll
};

class KeyboardEntryActivity : public Activity {
 public:
  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "",
                                 const size_t maxLength = 0, InputType inputType = InputType::Text,
                                 VoiceDictationHooks dictationHooks = {})
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        maxLength(maxLength),
        inputType(inputType),
        dictation(dictationHooks) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string title;
  std::string text;
  size_t maxLength;
  InputType inputType;
  bool passwordVisible = false;

  // Voice dictation (语音书签 edit screen only; hooks null elsewhere).
  // dictating swallows all input — the capture owns the loop, exactly like
  // VoiceActivity's Listening state (no repaint may touch the panel while
  // the mic rail is live).
  VoiceDictationHooks dictation;
  bool dictating = false;
  const char* dictationMsg = nullptr;  // translated static; shown ~4 s after a failure
  uint32_t dictationMsgTime = 0;

  ButtonNavigator buttonNavigator;

  int selectedRow = 0;
  int selectedCol = 0;
  int shiftState = 0;
  bool symMode = false;
  bool confirmHeld = false;
  bool confirmLongHandled = false;

  bool cursorMode = false;
  bool togglePos = false;
  size_t cursorPos = 0;
  bool upHeld = false;
  bool upLongHandled = false;
  bool downHeld = false;
  bool downLongHandled = false;
  bool rightHeld = false;
  bool rightLongHandled = false;
  size_t savedCursorPos = 0;
  size_t rightStartCursorPos = 0;

  bool urlMode = false;
  static constexpr int URL_SNIPPET_COUNT = 9;
  static constexpr const char* const urlSnippets[URL_SNIPPET_COUNT] = {
      "https://", "www.", ".com", "http://", "192.168.", ".org", "/opds", ":8080", ".net"};

  int delPressCount = 0;
  bool hintVisible = false;
  unsigned long hintShowTime = 0;

  void onComplete(std::string text);
  void onCancel();

  static constexpr uint16_t LONG_PRESS_MS = 500;
  static constexpr uint16_t DEL_LONG_PRESS_MS = 1500;

  static constexpr int COLS = 10;
  static constexpr int ABC_ROWS = 4;
  static constexpr int SYM_ROWS = 4;
  static constexpr int BOTTOM_KEY_COUNT = 5;

  static constexpr KeyDef abcLayout[ABC_ROWS][COLS] = {
      {{'1', '!'},
       {'2', '@'},
       {'3', '#'},
       {'4', '$'},
       {'5', '%'},
       {'6', '^'},
       {'7', '&'},
       {'8', '*'},
       {'9', '('},
       {'0', ')'}},
      {{'q', 'Q'},
       {'w', 'W'},
       {'e', 'E'},
       {'r', 'R'},
       {'t', 'T'},
       {'y', 'Y'},
       {'u', 'U'},
       {'i', 'I'},
       {'o', 'O'},
       {'p', 'P'}},
      {{'a', 'A'},
       {'s', 'S'},
       {'d', 'D'},
       {'f', 'F'},
       {'g', 'G'},
       {'h', 'H'},
       {'j', 'J'},
       {'k', 'K'},
       {'l', 'L'},
       {'-', '_'}},
      {{'z', 'Z'},
       {'x', 'X'},
       {'c', 'C'},
       {'v', 'V'},
       {'b', 'B'},
       {'n', 'N'},
       {'m', 'M'},
       {'=', '+'},
       {'.', '>'},
       {',', '<'}},
  };

  static constexpr KeyDef urlLayout[ABC_ROWS][COLS] = {
      {{'1', '!'},
       {'2', '@'},
       {'3', '#'},
       {'4', '$'},
       {'5', '%'},
       {'6', '^'},
       {'7', '&'},
       {'8', '*'},
       {'9', '('},
       {'0', ')'}},
      {{'q', 'Q'},
       {'w', 'W'},
       {'e', 'E'},
       {'r', 'R'},
       {'t', 'T'},
       {'y', 'Y'},
       {'u', 'U'},
       {'i', 'I'},
       {'o', 'O'},
       {'p', 'P'}},
      {{'a', 'A'},
       {'s', 'S'},
       {'d', 'D'},
       {'f', 'F'},
       {'g', 'G'},
       {'h', 'H'},
       {'j', 'J'},
       {'k', 'K'},
       {'l', 'L'},
       {'-', '_'}},
      {{'z', 'Z'},
       {'x', 'X'},
       {'c', 'C'},
       {'v', 'V'},
       {'b', 'B'},
       {'n', 'N'},
       {'m', 'M'},
       {':', '+'},
       {'.', '>'},
       {'/', '<'}},
  };

  static constexpr KeyDef symLayout[SYM_ROWS][COLS] = {
      {{'1', '\0'},
       {'2', '\0'},
       {'3', '\0'},
       {'4', '\0'},
       {'5', '\0'},
       {'6', '\0'},
       {'7', '\0'},
       {'8', '\0'},
       {'9', '\0'},
       {'0', '\0'}},
      {{'!', '\0'},
       {'@', '\0'},
       {'#', '\0'},
       {'$', '\0'},
       {'%', '\0'},
       {'^', '\0'},
       {'&', '\0'},
       {'*', '\0'},
       {'(', '\0'},
       {')', '\0'}},
      {{'-', '\0'},
       {'_', '\0'},
       {'=', '\0'},
       {'+', '\0'},
       {'[', '\0'},
       {']', '\0'},
       {'{', '\0'},
       {'}', '\0'},
       {';', '\0'},
       {':', '\0'}},
      {{'\'', '\0'},
       {'"', '\0'},
       {'/', '\0'},
       {'\\', '\0'},
       {'|', '\0'},
       {'?', '\0'},
       {'.', '\0'},
       {',', '\0'},
       {'~', '\0'},
       {'`', '\0'}},
  };

  static const char* const shiftString[2];

  int getContentRowCount() const;
  int getContentColCount() const;
  int getTotalRowCount() const;
  bool isBottomRow(int row) const;
  char getSelectedChar() const;
  char getAlternativeChar() const;
  bool handleKeyPress();
  bool insertChar(char c);
  void insertString(const std::string& str);
  void mapColContentBottom(int& col, bool goingUp) const;
};
