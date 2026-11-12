#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

// Character lookup card: opened from the reader's dictionary picker with the
// single chosen codepoint. The only backend is the relay's /api/v1/chat with
// mode="dict" (the relay applies its dictionary system prompt server-side:
// pinyin, radical and stroke count, meanings, word formations, an example —
// 【】-delimited lines). An unconfigured relay or a failed round is a hard
// failure with Confirm-to-retry; there is no second backend. The reply is laid
// out as a Xinhua-style card with the character set as a hero glyph at the
// top. When the relay supplied a stroke-order payload for the character,
// Confirm on the card opens a 田字格 view that draws every stroke in order;
// Back/Confirm there return to the card. Back returns to the picker with the
// cursor intact; Confirm retries after a failure.
class DictionaryActivity final : public Activity {
 public:
  DictionaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint32_t codepoint)
      : Activity("Dictionary", renderer, mappedInput), codepoint_(codepoint) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override { return state_ == State::Loading; }

 private:
  enum class State : uint8_t {
    Loading,  // relay round in flight (painted before the blocking call)
    Card,     // settled entry, scrollable
    Strokes,  // 田字格 stroke-order view (relay strokes payload)
    Failed,   // round died; Confirm retries from the relay
  };

  // One laid-out card row: a label (【拼音】 …) and its content. Short
  // content shares the label row; longer content wraps onto rows underneath so
  // meanings and examples stay readable instead of overflowing.
  struct CardRow {
    std::string label;
    std::vector<std::string> content;
    bool inlineContent = false;  // content[0] belongs on the label row
  };

  State state_ = State::Loading;
  bool started_ = false;  // one relay round per Loading entry
  uint32_t codepoint_ = 0;
  char charBuf_[8] = {};            // the selected character, UTF-8
  const char* failText_ = nullptr;  // tr() table pointer; valid for our lifetime
  std::unique_ptr<char[]> reply_;
  static constexpr size_t REPLY_CAP = 2048;
  // Encoded stroke-order payload for the hero character (relay "strokes"
  // field): strokes separated by '|', "x,y" points by ';', quantized 0..255
  // (relay tools/build_stroke_data.py). Empty for a character without data —
  // the card then offers no stroke view.
  std::unique_ptr<char[]> strokes_;
  static constexpr size_t STROKES_CAP = 1152;  // data max is 1106 (籲); see STROKES_META.json
  std::string heroChar_;                       // 【字】's content when present, else charBuf_
  std::vector<CardRow> rows_;
  bool rowsBuilt_ = false;  // rows_ matches the current reply + text width
  int scrollLine_ = 0;
  int maxScroll_ = 0;  // recomputed each render from the wrapped rows

  void beginLookup();
  void promptForWifi();
  void buildCard(int fontId, int textWidth);
  void renderStrokesView(int left, int right, int contentTop, int contentBottom);
};
