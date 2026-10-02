#pragma once

#include <cstdint>

// Pre-TLS bootstrap shared by every network client that has to complete a
// certificate-checked handshake on this board: the cloud STT path
// (lib/voice/SttClient, https) and the OpenClaw client (wss).
//
// It exists as its own translation unit so both callers install exactly one
// allocator and one clock policy. Two copies would be a silent hazard: the
// allocator has to be in place *before* the first mbedTLS handshake, and a
// second, later installer cannot retroactively fix a failed ssl_setup.
namespace NetBootstrap {

// True once time(nullptr) is past MIN_VALID_EPOCH, i.e. SNTP has set the
// system clock at least once. false on a board that has never synced.
bool systemTimeValid();

// Bounded (5 s) SNTP sync when the clock looks unset. Returns true when the
// clock is usable, false when it is still wrong — the caller decides how to
// report that. No-op when the clock is already valid.
bool ensureSystemTime();

// Set the system clock from a remote timestamp (e.g. the gateway challenge
// "ts") when it is still unset. The plaintext ws:// OpenClaw path never runs
// ensureSystemTime(), so without this the clock stays at the 1970 epoch and
// every epoch-derived UI (workbench date, chat history days) is blank.
// epochMs below MIN_VALID_EPOCH is rejected; no-op when the clock is already
// valid. Returns true when the clock is usable afterwards.
bool setSystemTimeFromEpochMs(int64_t epochMs);

// Install the PSRAM-first mbedTLS allocator. Idempotent, cheap to call from
// every TLS client.
void ensureTlsAllocator();

}  // namespace NetBootstrap
