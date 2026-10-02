# Reading Page-Turn Baseline (P0)

Measured on device (`[env:onepage]`, `LOG_LEVEL=2`) before any page-turn optimisation.
Purpose: split the perceived page-turn latency into buckets so P1 targets the right one.

## Method

- Firmware: `onepage-dev`, flashed 2026-10-01.
- Book: `/世界少年文学经典文库·中国经典篇（全套30册）.epub` (2086 spine items, CJK,
  reader font = SD card font, text anti-aliasing on).
- Driver: `/tmp/opencode/pt_base4.py` — `CMD:KEY` injection, full serial capture.
  Sequence: open book → 8 × page-back (chapter hops) → 20 × page-forward.
- Timing sources (already in the firmware, no instrumentation added):
  - `EpubReaderActivity.cpp` — `Page render (tiled): prewarm=… total=…`
  - `EpubReaderActivity.cpp` — `Rendered page in %dms`
  - `GfxRenderer.cpp:1372` — `Time = … ms from clearScreen to displayBuffer`
  - `EInkDisplay` — `Wait complete: fast/half (… ms)`

Settings relevant to the numbers: 刷新频率 = 15 页, 文字抗锯齿 = 开, 竖屏。

## Panel floor (hard, not reducible by CPU work)

| Refresh mode | Panel wait | Observed `display` bucket |
|---|---|---|
| `FAST_REFRESH` | **598 ms** | 623–624 ms |
| grayscale plane push | **61–62 ms** | `gray_display` 63–64 ms |
| `HALF_REFRESH` | **1356 ms** | 1394–1470 ms |

So a single ordinary page turn pays **≈ 688 ms of panel time** (624 + 64) before any
CPU work, and **≈ 1394 ms** whenever the page lands on the `HALF_REFRESH` path.

## Steady-state page turn (n=10, content pages, cold first page and chapter-build outliers excluded)

| Bucket | min | median | max | What it is |
|---|---:|---:|---:|---|
| `prewarm` | 24 | 379 | 763 | scan pass: renders the page once to collect glyphs |
| `bw_render` | 65 | 119 | 146 | real BW render into the framebuffer |
| `gray_lsb` | 17 | 70 | 114 | re-render #3 — LSB plane |
| `gray_msb` | 16 | 73 | 121 | re-render #4 — MSB plane |
| `display` | 623 | 624 | 1394 | panel (FAST, or HALF on the ghost-cleanup path) |
| `gray_display` | 63 | 64 | 64 | second panel push for the gray planes |
| `cleanup` | 12 | 12 | 13 | controller RAM re-sync from the live framebuffer |
| **`total`** | **829** | **≈1380** | **2086** | input-to-settled, excluding input handling |

\* `prewarm` is bimodal: pages either land at ~20–40 ms or at ~300–760 ms, with no
correlation to `SDCF Overflow` glyph loads (see Open questions).

Cold first page after opening the book: **1919–1928 ms** (`display` = HALF).
Chapter change with a cold section cache: **8891 ms** worst observed
(`bw_render` 5185 ms = `createSectionFile` on the SD card).

## Findings

1. **The page is rendered four times per turn** when text anti-aliasing is on:
   prewarm scan + BW + LSB + MSB (`renderContents`, `EpubReaderActivity.cpp:1163`).
   CPU share of a turn is therefore `prewarm + bw_render + 2 × gray render`
   ≈ 200–900 ms on top of the ~688 ms panel floor.
2. **Any page containing an image forces the *next* page onto `HALF_REFRESH`**
   (`pagesUntilFullRefresh = 1`, `EpubReaderActivity.cpp:1216`), i.e. +770 ms on
   that next page. Observed HALF pages followed image-bearing pages, not the
   configured 15-page cadence.
3. **No prefetch at all**: `silentIndexNextChapterIfNeeded`
   (`EpubReaderActivity.cpp:1054`) only builds the *next chapter's* section cache,
   and it does so on the render task while the current page is still on screen.
   The next page's glyphs/layout are never prepared during idle time.
4. **SD font overflow glyph loads cost ≈ 36 ms each** (`SDCF Overflow … on demand`,
   128-slot LRU). At boot the splash alone paid 2288 ms for 59 new glyphs. During
   steady turns only 10 such loads occurred, so this is a cold-cache cost, not the
   steady-state bottleneck.
5. **The chapter-build path blocks the UI** for up to ~9 s with no incremental
   feedback beyond the indexing popup.
6. Memory is not the constraint: Free heap 68–70 KB (min 64.7 KB), **PSRAM free
   ≈ 7.9 MB** — a 48 KB second framebuffer fits easily outside internal RAM.

## Where the time actually goes (resolved)

Instrumentation added during P0/P1 (`scan`/`fontpw` split, `SDCF prewarm`/`io`
lines) answers every open question above:

- **`prewarm` bimodality is `fontpw`, not `scan`.** The scan pass is 0–3 ms;
  `fontpw` is 19–886 ms and is exactly the SD glyph-cache miss cost.
- **Measured on a cold chapter** (`n=207` unique glyphs, style 0):
  `meta=302ms/150seek` + `bmp=540ms/149seek/28930B` = **842 ms of SD I/O**.
  A random SD read costs ~2 ms — that is the card's seek latency, not SPI
  throughput (SPI runs at 20–40 MHz).
- **Bitmaps cannot be batched**: span `[553..5821865]` for 2465 B of payload.
  The glyph bitmaps are scattered across ~5.8 MB, so a contiguous read is
  hopeless. Batching the metadata is possible (index span `[32..42782]` for
  155 glyphs) but would save at most ~200 ms of the 842 ms.
- **The PSRAM glyph cache works** (`GLYPH_CACHE_SLOTS=12289`,
  `GLYPH_CACHE_MAX_FILL=9000`, alloc OK, no eviction). Once a page's glyphs are
  resident the same prewarm costs **19–37 ms** with `0` seeks. It persists
  across `clearCache()`.
- **What actually differs page to page is vocabulary coverage**: consecutive
  pages in a chapter share most glyphs (warm), while a chapter jump introduces
  a new glyph set (cold). That is why `fontpw` looked bimodal.

## P1a — idle glyph prefetch (DONE, measured)

`EpubReaderActivity::prefetchPageGlyphs()` reads the *next* page's text out of
the section cache with `Section::getTextFromSectionFile()` and feeds it to
`FontCacheManager::prewarmCache()` on the render task, right after the current
page has been pushed to the panel. The render task is otherwise idle until the
reader presses a key, so the cost lands in dwell time.

Trigger widened in `silentIndexNextChapterIfNeeded()` from
`currentPage == pageCount - 2` to `currentPage + 2 >= pageCount`, so the next
chapter's section cache (and therefore its glyph prefetch) is built from the
penultimate page onward rather than only on it.

**A/B** — same script, same cold landing chapter (spine 2046, never visited
before), `/tmp/opencode/pt_cold.py 40 24`, one flash per arm
(`-DDISABLE_GLYPH_PREFETCH` for the control):

| | `fontpw` median | `fontpw` mean | page `total` median | page `total` mean |
|---|---|---|---|---|
| prefetch **off** | 370 ms | 359 ms | **1423 ms** | 1484 ms |
| prefetch **on** | **34 ms** | 122 ms* | **1084 ms** | 1280 ms |

\* mean still includes the unavoidable cold renders of the chapter jump itself
(933/665 ms) before the pipeline warms; the steady pages run 20–40 ms.
Median page-turn latency drops **1423 → 1084 ms (−24%)**, and the long tail of
500–900 ms glyph stalls disappears from ordinary page turns.

Cost when the target is already warm: 20–40 ms per page, once (guarded by
`prefetchedSpine_`/`prefetchedPage_`/`prefetchedFont_`).

## P1b — whole-page pre-render into PSRAM (DONE, measured)

Glyph prefetch moved the font cost out of the critical path but left the rest of
the pipeline on every turn: `loadPageFromSectionFile` → `renderContents`
(BW raster + `fontpw` + `gray_lsb` + `gray_msb`) ≈ 363 ms of CPU, paid *before*
the panel even starts waiting. P1b renders the **next** page in full on the
render task's idle dwell time and stashes the two 48 000-byte gray planes in
PSRAM, so a turn becomes `memcpy` + status bar + panel.

**Implementation** (`src/activities/reader/EpubReaderActivity.{h,cpp}`):
`StageKey` (18 fields: spine/page/fontId/viewport/pageCount/lineCompression/
lineSpacing/alignment/extraPara/hyphenation/embeddedStyle/focusReading/
imageRendering/textAA/rule settings/orientation, defaulted `operator==`) →
`makeStageKey()` → `stageNextPage()` → `displayStagedContents()`.
`stageNextPage()` loads the next `Section`, compares keys, rasterises through
`beginStripTarget`/`clearScreen(0xFF)`/`endStripTarget` twice (LSB, MSB) with the
same `PrewarmScope`/`scan`/`endScanAndPrewarm` sequence as the normal path, then
stores `stagedKey_` + three `makeUniqueNoThrow<uint8_t[]>(48000)` buffers (heap
>4 KiB lands in PSRAM automatically). `render()` now tries the staged fast path
first and falls back to `prefetchPageGlyphs()` only if staging failed.

**Skips** (all logged): missing target/section cache, `page >= pageCount`,
page load failure, and **`section->hasImages()`** — image pages stay on the
double-FAST path, so the staged buffer never holds a bitmap page.

**Measured** (`/tmp/opencode/pt_cold.py 40 24`, same cold chapter as P1a):

| | page `total` median | mean | staged hit rate |
|---|---|---|---|
| baseline (P0) | 1423 ms | 1484 ms | — |
| P1a (glyph prefetch) | 1084 ms | 1280 ms | — |
| **P1b (page staging)** | **800 ms** | 942 ms | 17 / 26 (65 %) |

**−44 % against baseline.** Steady state from turn 6 onward is **17/17 staged**;
the misses are the cold chapter jump itself plus a test-harness race (the script
injects `right` while the landing render is still in flight, so `pageTurn()`
mutates `section->currentPage` concurrently — a pre-existing race, not new).

Staged page cost breakdown, median of a staged turn:

```
copy=3ms status=64ms display=624ms gray_lsb=16ms gray_msb=16ms
gray_display=64ms cleanup=13ms   total≈801ms
```

Panel = 688 ms (86 %), CPU = 112 ms (14 %). The panel is now the floor.

**P1 is closed.** Even with CPU at zero the floor is 686 ms, so the remaining
headroom on a text page is ≤ 13 %; measured after the battery fix below it is
`status=45ms`, i.e. the remaining CPU is 93 ms and the achievable floor is
≈ 730 ms. Both remaining CPU items are listed under "Remaining P1 candidates"
and neither is worth its risk/complexity.

**Correctness verified bit-for-bit**: turn right onto page P (staged, 893 ms),
turn left back onto P (slow path, 1851 ms) — the two 48 000-byte framebuffers
are **md5-identical** (`a92f7e02…`), and screenshots of every state (initial
load, staged text page, image page, menu overlay, menu return) are pixel-correct.
Regression after the battery fix: 6/6 consecutive turns staged, 0 `ERR`.

## P2 — embedded in-reader typography panel (DONE, measured)

The reading page had no reachable entry to the typography settings: a layout
change could only be made from the root Settings app, which cannot be reached
without leaving the book.

**Shape decision — A′ (embedded `SettingsActivity`), ~40 lines, zero new
rendering code.** `src/SettingsList.h` already declares the whole
`STR_CAT_READER` catalogue as a table (`SettingInfo` rows + `buildFontFamilySetting`)
with full i18n; only the *reachability* from the reader was missing. Rejected:

- **B — extract a `SettingInfo` row renderer + new `ReaderSettingsActivity`**
  (~250 lines plus a refactor of `SettingsActivity::render`).
- **C — hand-roll a reader-only panel** (~150 lines duplicating the row/value/
 switch rendering that `SettingsList` already provides).

Changes:

- `SettingsActivity` gains a `bool embedded` ctor arg. `onEnter()` starts on
  the Reader category (`selectedCategoryIndex = embedded ? 1 : 0`) instead of
  Display; back with a row selected returns to row 0, and back at row 0 calls
  `finish()` when embedded (root launch keeps `onGoHome()`).
- `EpubReaderMenuActivity` adds `MenuAction::READER_SETTINGS` /
  `STR_READER_SETTINGS` ("Reading Settings" / "阅读设置") between the ruled-line
  item and Home; `items.reserve(14)` → `16`.
- `EpubReaderActivity` gains `LayoutSignature` + `makeLayoutSignature()` +
  `applyLayoutChanges(before)`, installed as the menu's `READER_SETTINGS`
  result handler. It snapshots the signature *before* the panel opens, and on
  return: re-projects orientation if it changed, then preserves
  `cachedSpineIndex` / `currentSpineIndex` / `cachedChapterTotalPageCount` /
  `nextPageNumber` and `section.reset()` — the same contract as
  `applyOrientation`. `Section::loadSectionFile` returns false on any of its
  10-parameter mismatch (`lib/Epub/Epub/Section.cpp:77-121`) so the pages
  rebuild; `StageKey` already carries fontId / lineCompression / vw / vh /
  orientation, so the staged PSRAM buffer falls through to the slow path and
  re-stages with the new settings.

**Bug found and fixed during verification — Back release leak.** `main.cpp`
calls `gpio.update()` once per frame and edge flags survive to the next frame;
`CMD:KEY` injects "pressed this frame, released on the next update"
(`src/main.cpp:648-656`). The embedded panel used `wasPressed(Back)`, which
pops on the *press* frame — the following frame's *release* edge then landed on
`EpubReader`, whose `wasReleased(Back) && heldTime < GO_HOME_MS` handler sent
the book to Home (log: `Exiting activity: Settings` → `Popped … size = 0` →
`Exiting activity: EpubReader` → `Entering activity: Home`). The other in-reader
sub-activities already do this correctly (`EpubReaderMenuActivity.cpp:89`,
`RuledLineSettingsActivity.cpp:72` both use `wasReleased`), so embedded back now
fires on `wasReleased`:

```cpp
const bool backEvent = embedded ? mappedInput.wasReleased(MappedInputManager::Button::Back)
                                : mappedInput.wasPressed(MappedInputManager::Button::Back);
```

**Verified on device** (`/tmp/opencode/p2c.py`, then settings restored via
`p2d.py`):

- Menu shows 阅读设置 between 行下虚线设置 and 回到主页; `down`×9 reaches it.
- Panel opens on the **阅读** category with the full 14-row reader catalogue.
- Toggling 额外段落间距 开→关: `Deserialization succeeded: 3 pages` (was 7),
  reader shows `1/3` and `md5(reader before) ≠ md5(reader after)` — re-layout
  confirmed, progress preserved (`Progress saved: spine=366 page=0`).
- Back returns to the book, **not** Home (`Entering activity: Settings` →
  `Exiting activity: Settings` and nothing else).
- After `p2d.py` restored both settings, `md5(reader)` is bit-identical
  (`adfb9f46`) to the pre-test frame — no residue.
- Errors during the run are the pre-existing `No decoder found for image`
  GIFs plus the deliberate `Deserialization failed: Parameters do not match`
  cache-miss line; `pio check -e onepage` stays at **1 H / 1 M / 33 L**.

**Finding turned into P3a (see below): the font-size row appeared to have no
effect for SD fonts.** `CrossPointSettings::getReaderFontId()`
(`src/CrossPointSettings.cpp:351`) resolves through
`sdFontIdResolver(ctx, sdFontFamilyName, fontSize)`, but
`SdCardFontSystem::resolveFontId()` (`src/SdCardFontSystem.cpp:106-111`)
deliberately ignores `fontSizeEnum` and returns `manager_.getFontId(familyName)` —
the id of whatever face is *currently loaded*. Only `ensureLoaded()` swaps that
face, and its sole caller was `ReaderActivity::onEnter()`, which the embedded
panel never passes through. Root Settings only appeared to work because the
next library visit re-ran `ensureLoaded()`. The re-layout proof above therefore
used the extra-paragraph-spacing toggle instead of font size.

## P3a — SD font size actually applies (DONE, measured)

`ensureLoaded()` now runs on both paths the reader can be reached from:

- `EpubReaderActivity::onEnter()` (`src/activities/reader/EpubReaderActivity.cpp`,
  after `ReaderUtils::applyOrientation`) — covers silent resume, root Settings,
  KOReader sync and any other direct entry that skips `ReaderActivity`.
- `EpubReaderActivity::applyLayoutChanges()`, **before** sampling `after` —
  `makeLayoutSignature()` reads `getReaderFontId()`, which reports the loaded
  face, so without the reload a pure size change would compare equal and be
  dropped. Both are inside the `RenderLock`; `ensureLoaded()` early-returns
  when the wanted point size is already current
  (`SdCardFontSystem.cpp:83`), so the steady-state cost is a string compare.

Mechanism (no new data structures, no new allocation):
`findClosestReaderSize()` (`lib/EpdFont/SdCardFontRegistry.cpp:18`) maps the
size enum to a `.cpfont` file — ordinal selection when the family ships ≥ 4
sizes, closest-to-target otherwise — and `manager_.loadFamily()` swaps the
loaded face, which is what changes `getReaderFontId()` and therefore the
10 `Section` parameters and the staged key.

Measured (`/tmp/opencode/p2e.py`, restored by `p2f.py`):

```
[SDFS] Reloading FZPingXYSJW: size 12 -> 14 (enum 3)
[SDFS] Loaded SD font family: FZPingXYSJW
[ERR]  [SCT] Deserialization failed: Parameters do not match   (expected cache miss)
[ERS]  Stage key mismatch: have (366,1,6,...,1948604660) want (366,0,7,...,601702565)
[SCT]  Deserialization succeeded: 7 pages                      (was 6)
```

`fontId` changed, the chapter repaginated 6 → 7 pages, and the reader shows
`1/7`. Reverse run (`p2f.py`, 大→特大→大) logged `size 14 -> 12 (enum 2)` and
the final frame's `md5` is bit-identical to the pre-test frame
(`02a4ae40`).

Cost: Flash **5,465,380 → 5,465,416 = +36 bytes** (linker-reported; note that
`.pio/build/onepage/firmware.bin` is ~24 KB larger than the linked figure
because it carries image padding — always compare the `Flash:` line, not the
file size). `pio check -e onepage` stays at **1 H / 1 M / 33 L**.

**P3 closed.** The remaining font-related latency turned out to be a cold-start
cost that P1a/P1b already route around, not a steady-state one:

| stage | total | `fontpw` | note |
|---|---|---|---|
| first render of a book | 2808 ms | **933 ms** (33 %) | 300 SD seeks / 877 ms / 21.5 KB, `fill=171/9000` |
| chapters 2–3 | — | 506 / 882 ms | cache still filling |
| **steady-state turn** | 1082 ms | **38 ms** (3.5 %) | glyph cache hit |

`SdCardFont`'s read path (merging the meta/bitmap seeks) would buy back a
one-time ~900 ms that disappears on its own once `GLYPH_CACHE_MAX_FILL = 9000`
fills — deliberately not taken. Visual-quality work on CJK mixing, punctuation
and AA was also left out: it is subjective, needs multi-round screenshot
review, and does not touch the page-turn budget this document exists to fix.


## Remaining P1 candidates (ranked by measured value)

| Action | Saves | Risk |
|---|---|---|
| ~~Idle glyph prefetch (P1a)~~ | 339 ms/turn | DONE |
| ~~Whole-page pre-render into PSRAM (P1b)~~ | 284 ms/turn on top of P1a | DONE |
| ~~Battery ADC time-cache (C61 path)~~ | ≥7 ms/turn, fleet-wide | DONE |
| Overlap the two gray plane renders into the 598 ms BW panel wait | ~0 now — P1b already moved them into dwell | **high** — needs an `EInkDisplay` submit/wait split (SDK surgery) |
| Merge LSB+MSB into one render pass | 32 ms | medium — needs a 2-bit write target in `GfxRenderer` |
| Relax the "image page ⇒ next page HALF" rule | 770 ms on the page after any image (~12 % of pages) | visual — must be screenshot-verified against bug #2190 |
| **Image pages themselves (double FAST refresh + gray)** | **3.6 s cached / 12.6 s cold per image page** | visual — the double refresh is deliberate (pablohc's technique); **ranked #1 post-P3**, it is the last latency that still feels broken |
| **Chapter-jump cold render** | 1.7–2.6 s on every chapter change | medium — section build + `prewarm` SD spans |
| `displayWindow()` for status-bar-only updates | unknown | unmeasured; currently zero callers |

Rejected after evaluation (kept for the record):

- **Draw the status bar into the staged buffer at stage time** — saves 45 ms
  (6 %), but progress %, bookmark flag and auto-turn text are computed from
  *current* state and would be frozen at stage time → a stale always-visible UI
  element. 6 % is not worth a correctness regression.
- **Second task to overlap the 32 ms of gray-plane CPU with the 623 ms panel
  wait** — 4 % and it needs a second render context.

With P1 closed the split is **panel 686 ms (88 %) / CPU 93 ms (12 %)**; page-turn
latency is now bound by the e-ink panel, so the next large wins are the two
outliers above (image pages, chapter jumps), not steady-state turns.

## Fixed alongside P0

- **Corrupt progress spine index self-heal**: `onEnter()` clamped an
  out-of-range persisted index but never repaired it, because the legacy
  6-byte `progress.bin` read ran *unconditionally after* the FR-01 dual-slot
  read and overwrote it with the freshness record's sequence number
  (21328 misparsed as a spine index). The legacy read now only runs when
  `ProgressFile::readNewest()` fails (pre-M1 caches). Verified: the
  `Persisted spine index … out of range` error no longer appears on reopen.

- **Battery sampled twice per status-bar draw**: `BaseTheme::drawStatusBar`
  calls `powerManager.getBatteryPercentage()` at `BaseTheme.cpp:828` for layout
  width and again inside `drawBatteryLeft`/`drawBatteryRight` (`:103`/`:119`).
  On the C61 ADC path `HalPowerManager::getBatteryPercentage()` had **no** poll
  cache, so every call did `pinMode` + `digitalWrite` + **`delay(5)`** + ADC
  sample + `digitalWrite` — ≥ 10 ms per frame, in every activity, plus a
  charging-pause churn on `BAT_CHG_EN`. The I2C branch already had
  `BATTERY_POLL_MS = 1500`; the cache now sits above the backend split and
  both share it (`lib/hal/HalPowerManager.cpp`). Measured: staged `status`
  **52 → 45 ms**, chapter-title spike **194 → 118 ms**, battery % still renders.

## Reproduce

### Four-orientation verification (post P1b / P2)

`/tmp/opencode/p4.py` walks all four orientations through the reader menu's
ROTATE_SCREEN entry (`EpubReaderMenuActivity.cpp:73` cycles a local
`pendingOrientation`; `EpubReaderActivity.cpp:406` applies it on menu exit
even when the menu is cancelled, so the walk is self-restoring: 0→1→2→3→0).

| orientation | md5 | correct view is | checked |
|---|---|---|---|
| PORTRAIT (0) | `02a4ae40` | `fb2png … cw` | ✅ status bar, progress, 1/6, no clipping |
| LANDSCAPE_CW (1) | `afa928a8` | `fb2png … 180` | ✅ 800×480, both landscape rows intact |
| INVERTED (2) | `979becef` | `fb2png … ccw` | ✅ identical layout to PORTRAIT |
| LANDSCAPE_CCW (3) | `f7903e17` | `fb2png` (no rotation) | ✅ native panel orientation |
| back to PORTRAIT | `check` md5 == `m0` md5 | `cw` | ✅ walk restored itself |

The menu screenshot in LANDSCAPE_CW shows the `阅读方向` row highlighted with
value `横屏（顺时针）` and the rotated side-button legend (`返回/上/下/选择`),
so D-pad navigation still maps correctly in landscape. Errors during the walk
were **28 pre-existing `No decoder found for image` (GIF)** plus exactly **4
`[SCT] Deserialization failed: Parameters do not match`** — one per rotation,
i.e. the viewport change correctly invalidated the section cache each time.
No new errors.

```bash
rm -f .pio/build/onepage/firmware.bin && pio run -e onepage      # avoid stale bin
pio run -e onepage -t upload --upload-port /dev/ttyACM0
python3 /tmp/opencode/pt_base4.py 8 20
grep -o "Page render (tiled): .*" /tmp/opencode/pt_base4.log
```

P2 flow (reader → menu → embedded settings → re-layout → back):

```bash
python3 /tmp/opencode/p2c.py      # toggles 额外段落间距, screenshots r_a..r_f
grep -o "SAVED [a-f] md5=.*" /tmp/opencode/p2c.log   # r_a != r_f
python3 /tmp/opencode/p2d.py      # restores both settings, screenshots s_a..s_f
grep -o "SAVED [a-f] md5=.*" /tmp/opencode/p2d.log   # s_f == r_a (bit-identical)
for f in r_e r_f; do python3 /tmp/opencode/fb2png.py /tmp/opencode/$f.raw /tmp/opencode/$f.png cw; done
```
