#pragma once

// The custom sleep cover: one BMP copied over /sleep.bmp plus the settings
// flip that makes the boot/sleep screen draw it. Shared by the image viewer
// (BmpViewerActivity) and the AI 生图 screen — the copy is the same 48 KB
// stream and the failure modes are the same, so the file layout (a magic
// path, a settings enum) lives in exactly one place.
namespace SleepCover {

// Copies `srcPath` over /sleep.bmp and sets SETTINGS.sleepScreen = CUSTOM
// (saved immediately: the next sleep must draw the new cover even if the
// battery dies first). Streams in 2 KB chunks — no full-image buffer.
// Returns false (and touches nothing else) when either file cannot be
// opened or a write fails; the popups and delays are the caller's job.
bool apply(const char* srcPath);

}  // namespace SleepCover
