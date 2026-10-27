#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "./FileBrowserActivity.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
  int selectorIndex = 0;  // 0 = preview card, 1..N = grid tiles
  int previewIndex = 0;   // which preview book the cover card shows
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool hasOpdsServers = false;
  int32_t reviewDue = -1;
  std::string reviewLabel;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  std::vector<std::string> gridLabels;
  std::vector<UIIcon> gridIcons;
  const HomeMenuItem initialMenuItem;

  // Home menu grid shape: three icon tiles per row (CJK labels need the width).
  static constexpr int GRID_COLUMNS = 3;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, bool hasOpdsUrl) {
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::RECENTS) return i;
    ++i;
    if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsUrl ? i : 0;
    if (hasOpdsUrl) ++i;
    if (item == HomeMenuItem::REVIEW) return i;
    ++i;
    if (item == HomeMenuItem::VOICE) return i;
    ++i;
    if (item == HomeMenuItem::WORKBENCH) return i;
    ++i;
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::RECENTS;
    if (hasOpdsUrl && idx == i++) return HomeMenuItem::OPDS_BROWSER;
    if (idx == i++) return HomeMenuItem::REVIEW;
    if (idx == i++) return HomeMenuItem::VOICE;
    if (idx == i++) return HomeMenuItem::WORKBENCH;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onReviewOpen();
  void onVoiceOpen();
  void onWorkbenchOpen();

  void buildMenu();
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPreIndex() const override { return true; }
};
