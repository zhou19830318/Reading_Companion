#include "CjkUiFontSystem.h"

#ifdef ONEPAGE_C61

#include <EpdFontFamily.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

CjkUiFontSystem cjkUiFontSystem;

constexpr const char* CjkUiFontSystem::kCandidatePaths[];

bool CjkUiFontSystem::loadIfAvailable() {
  if (loaded_) return true;

  for (const char* path : kCandidatePaths) {
    if (!Storage.exists(path)) continue;
    if (font_.load(path)) {
      loaded_ = true;
      LOG_INF("CJKUI", "Loaded CJK UI font: %s", path);
      return true;
    }
    LOG_ERR("CJKUI", "Found but failed to load: %s", path);
  }

  LOG_DBG("CJKUI", "No CJK UI font on SD; Chinese UI will show replacement glyphs");
  return false;
}

bool CjkUiFontSystem::attachTo(EpdFont& font, const bool bold) {
  if (!loaded_) return false;
  for (size_t i = 0; i < slotCount_; i++) {
    if (slotFont_[i] == &font) return true;  // already attached (idempotent)
  }
  if (slotCount_ >= kMaxSlots) {
    LOG_ERR("CJKUI", "slot table full (%u); cannot attach another font", static_cast<unsigned>(kMaxSlots));
    return false;
  }

  const size_t s = slotCount_;
  slotFont_[s] = &font;
  slotOrig_[s] = font.data;

  // Copy the flash EpdFontData (POD) into a DRAM-resident mutable slot. The
  // const pointer members keep pointing at the builtin flash arrays; only the
  // glyph-miss hook changes, so the builtin Latin/Cyrillic glyphs still render
  // from flash and only missing (CJK) codepoints route to the SD font.
  memcpy(&slotData_[s], font.data, sizeof(EpdFontData));

  // LXGW WenKai is a single-style file; resolveStyle() maps bold -> the closest
  // present style (regular). CJK has no synthetic bold, so this is expected.
  const uint8_t style = font_.resolveStyle(bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  font_.wireAsFallback(slotData_[s], style);
  font.data = &slotData_[s];
  slotCount_++;
  attached_ = true;
  return true;
}

void CjkUiFontSystem::attachToUiFonts(EpdFont& ui10Reg, EpdFont& ui10Bold, EpdFont& ui12Reg, EpdFont& ui12Bold,
                                      EpdFont& small) {
  if (!loaded_) return;
  // `small` is the 8pt font used by some themes' button hints — it must get
  // the fallback too or those hints render garbled/blank in Chinese.
  const bool ok = attachTo(ui10Reg) && attachTo(ui10Bold, /*bold=*/true) && attachTo(ui12Reg) &&
                  attachTo(ui12Bold, /*bold=*/true) && attachTo(small);
  if (ok) LOG_INF("CJKUI", "CJK fallback attached to UI fonts");
}

void CjkUiFontSystem::detachAll() {
  if (!attached_) return;
  for (size_t i = 0; i < slotCount_; i++) {
    if (slotOrig_[i]) slotFont_[i]->data = slotOrig_[i];
    slotFont_[i] = nullptr;
    slotOrig_[i] = nullptr;
  }
  slotCount_ = 0;
  attached_ = false;
}

#endif  // ONEPAGE_C61
