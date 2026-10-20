#pragma once

#include <I18n.h>
#include <VoiceRelayCodec.h>  // ImageMeta

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// AI 生图: 参数 (风格/画幅, Options) -> prompt (voice + keyboard through
// VoiceActivity's question mode) -> relay POST /api/v1/image -> BMP streamed
// to the SD card -> full-screen view with 设为封面. A small menu fronts it
// (生成新图 / 历史记录); the history replays past images from /ai-images
// without touching the relay. From the viewer, 删除 removes a picture after
// a confirm.
//
// Files are named <keyword>_ai_%04d.bmp where keyword is the head of the
// generating prompt (see imageKeyword in the .cpp).
//
// The generate/edit call blocks the loop for as long as the provider renders
// (tens of seconds) with a static waiting frame on the panel — the same
// contract as AskAiActivity's relay round and SttClient::transcribe.
class AiImageActivity final : public Activity {
 public:
  explicit AiImageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AiImage", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { Menu, Options, Working, Viewing, History, Failed, ConfirmDelete };
  // What resumeRound() runs once the network is up. A failed download retries
  // as a download: the render is stored server-side, re-generating would pay
  // for a second one. openEditor() leaves this at Generate; startRound(false)
  // never overwrites it either.
  enum class PendingAction : uint8_t { Generate, Download };
  static constexpr int MENU_ROWS = 2;
  static constexpr int OPTIONS_ROWS = 3;  // 风格 / 画幅 / 开始生成

  // Flow (each step owns the blocking call the previous one set up).
  void openEditor();
  void startRound(bool downloadOnly);
  void resumeRound();
  void doGenerate();
  void doDownload();
  void retry();
  void loadHistory();
  void applyCover();
  void fail(StrId reason);

  bool relayConfigured();
  std::string nextImagePath();
  bool drawImage(const char* path);

  State state_ = State::Menu;
  PendingAction pendingAction_ = PendingAction::Generate;
  int menuIndex = 0;
  int optionIndex = 0;
  ButtonNavigator buttonNavigator;

  // Options row values: DashScope V1 style enum strings / "W*H" sizes.
  // Index 0 on both rows is 默认 (nullptr -> server default).
  static constexpr int STYLE_COUNT = 10;
  static constexpr int SIZE_COUNT = 3;
  int styleIndex_ = 0;
  int sizeIndex_ = 0;
  const char* styleValue() const;
  const char* sizeValue() const;

  // Last prompt: the editor's prefill (再来一张) and the failed-screen retry.
  char lastPrompt_[VoiceRelay::IMAGE_TEXT_CAP + 1] = {};
  // The last generated asset. Kept after a download failure so Retry re-fetches
  // the stored image instead of paying for another render.
  VoiceRelay::ImageMeta lastMeta_;
  bool downloadPending = false;

  std::string pendingPath;                // where the in-flight download lands
  std::string currentPath;                // what Viewing shows (also the edit base)
  std::vector<std::string> historyNames;  // ascending by name; displayed newest first
  int historyIndex = 0;                   // row index in display order (0 = newest)
  bool viewingFromHistory = false;

  StrId failReason_ = StrId::STR_AI_IMAGE_FAILED;
};
