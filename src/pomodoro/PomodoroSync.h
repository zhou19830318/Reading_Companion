#pragma once

// Uploads the focus-timer session queue (PomodoroSession's RAM queue) to the
// relay. Never connects Wi-Fi itself — this firmware has no silent-connect
// path — and never blocks when offline: the gate returns before any HTTP
// work. Called from the alert screen, where the user is already looking at
// the panel and a short upload is invisible.
namespace PomodoroSync {

// True when an upload was attempted (whether or not the relay accepted it).
// On success the uploaded records are dropped from the queue; on failure they
// stay queued for the next alert.
bool flushIfOnline();

}  // namespace PomodoroSync
