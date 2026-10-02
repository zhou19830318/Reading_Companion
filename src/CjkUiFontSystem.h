#pragma once

// CJK UI font fallback for the OnePage (ESP32-C61 / PSRAM) build.
//
// The builtin UI fonts (ubuntu_10/12, flash-resident EpdFontData) carry no CJK
// glyphs, so Chinese UI labels — including the "简体中文" / "繁體中文" entries in
// the language picker — would render as replacement boxes. This system loads a
// CJK .cpfont from the SD card and wires it as a glyph-miss fallback into a
// fixed table of EpdFont objects: EpdFont::getGlyph() then back-fills any
// codepoint the builtin font lacks from the SD font's on-demand overflow path.
// Because both the bitmap render (renderCharImpl) and the layout measurement
// (getTextBounds) go through the same getGlyph(), the fallback covers
// rendering AND metrics. Besides the five UI fonts, builtin Noto faces that
// draw user-visible CJK (e.g. the Workbench date line's Noto Serif 14) are
// attached individually via attachTo().
//
// ONEPAGE_C61 only: the X4 build has no PSRAM to hold the CJK glyph cache.

#ifdef ONEPAGE_C61

#include <EpdFont.h>
#include <EpdFontData.h>
#include <SdCardFont.h>

class CjkUiFontSystem {
 public:
  CjkUiFontSystem() = default;
  ~CjkUiFontSystem() = default;
  CjkUiFontSystem(const CjkUiFontSystem&) = delete;
  CjkUiFontSystem& operator=(const CjkUiFontSystem&) = delete;

  // Load the CJK UI font from the SD card (first existing candidate path).
  // Safe to call more than once (no-op once loaded). Returns true on success;
  // logs a warning and returns false if no font file is present — callers must
  // tolerate this (UI simply shows replacement boxes for CJK).
  bool loadIfAvailable();

  // Wire the CJK fallback into UI EpdFont objects. Must be called after
  // loadIfAvailable() returned true. `attachToUiFonts()` covers the five
  // builtin UI fonts the themes draw with (kept for call-site convenience);
  // `attachTo()` is the generic slot-based entry any other builtin EpdFont
  // can use — needed e.g. for the Workbench date line (Noto Serif 14 draws
  // 年月日/星期, which the pure-Latin builtin has no glyphs for). Copies each
  // builtin EpdFontData into a mutable DRAM slot, sets its glyphMissHandler
  // to the CJK SdCardFont, and redirects EpdFont::data to the copy.
  // Idempotent (a font already in a slot is left alone).
  void attachToUiFonts(EpdFont& ui10Reg, EpdFont& ui10Bold, EpdFont& ui12Reg, EpdFont& ui12Bold, EpdFont& small);
  // Generic attach: `bold` picks the fallback style slot (LXGW WenKai ships
  // one style; resolveStyle maps bold->regular). Returns false if the font is
  // not loaded, the slot table is full, or this exact EpdFont is attached.
  bool attachTo(EpdFont& font, bool bold = false);

  // Restore the original builtin EpdFontData pointers on every attached slot.
  void detachAll();

  bool isLoaded() const { return loaded_; }
  bool isAttached() const { return attached_; }

 private:
  // Candidate SD paths, tried in order. The LXGW WenKai 12pt .cpfont already
  // present on OnePage cards serves both Simplified and Traditional; 10pt UI
  // reuses the same file (slightly large but legible).
  static constexpr const char* kCandidatePaths[] = {
      "/.fonts/LXGWWenKai-Regular/LXGWWenKai-Regular_12.cpfont",
      "/fonts/LXGWWenKai-Regular/LXGWWenKai-Regular_12.cpfont",
      "/.fonts/LXGWWenKai/LXGWWenKai_12.cpfont",
      "/fonts/LXGWWenKai/LXGWWenKai_12.cpfont",
  };

  SdCardFont font_;
  bool loaded_ = false;
  bool attached_ = false;

  // Fixed slot table for attached fonts: the five UI fonts plus the builtin
  // Noto faces the Workbench draws CJK with (dates/weather) — capacity 7 is
  // the whole planned set, so no dynamic allocation. Each slot is a mutable
  // EpdFontData copy (BSS, ~144B x7 in DRAM). All const pointer members still
  // reference the builtin flash arrays; only glyphMissHandler / glyphMissCtx
  // are overwritten.
  static constexpr size_t kMaxSlots = 7;
  EpdFontData slotData_[kMaxSlots]{};
  const EpdFontData* slotOrig_[kMaxSlots] = {};
  EpdFont* slotFont_[kMaxSlots] = {};
  size_t slotCount_ = 0;
};

extern CjkUiFontSystem cjkUiFontSystem;

#endif  // ONEPAGE_C61
