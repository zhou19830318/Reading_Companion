#pragma once

#include <CloudConfig.h>

#include <cstdint>

// One-shot reachability + authentication check for the OpenClaw endpoint, run
// synchronously inside the web server's request handler so the browser gets a
// single verdict instead of streaming events.
//
// It repeats the connection sequence in OpenClawActivity::startConnect()
// rather than sharing it: that path is incremental (its loop() drives
// WebSocketsClient::loop() across UI frames) and cannot be collapsed into a
// blocking call. Keep the two in step when the handshake changes.
//
// Config and Result are fixed-size structs with no heap of their own; callers
// allocate them (the web handler uses makeUniqueNoThrow) so nothing of this
// size lands on the small default task stack.
namespace OpenClawProbe {

struct Config {
  char host[64] = {};
  char path[96] = "/";
  char protocol[40] = {};
  char caPath[96] = {};
  uint16_t port = 443;
  bool tls = true;
  // Credentials for the connect request. Copied out of CloudConfig by the
  // handler; an empty deviceKeyHex makes the probe derive a throwaway
  // identity so the "no device key yet" state is still testable without
  // writing to NVS.
  char token[CloudConfig::TOKEN_SIZE] = {};
  char deviceToken[CloudConfig::TOKEN_SIZE] = {};
  char deviceKeyHex[CloudConfig::DEVICE_KEY_SIZE] = {};
};

struct Result {
  bool ok = false;
  // Flash-resident label for the stage that settled the probe:
  // config | network | clock | ca | tcp | connect | ws | auth
  const char* stage = "config";
  // Why it stopped: our own note, or the library/server's rejection text.
  char detail[128] = {};
  // First text frame, when one arrives before the deadline — the stage S1
  // acceptance criterion is the server speaking first.
  char firstFrame[160] = {};
  // Populated when the gateway rejects a valid auth with NOT_PAIRED: the id
  // the operator feeds to `openclaw devices approve <id>`. Empty otherwise.
  char requestId[48] = {};
  // "wss://host:port/path" as actually dialled, for the report.
  char url[224] = {};
  uint32_t elapsedMs = 0;
};

// Blocking. Bounded by WEBSOCKETS_TCP_TIMEOUT plus the frame/auth deadlines,
// so a hung endpoint cannot pin the web server indefinitely.
void run(const Config& cfg, Result& out);

}  // namespace OpenClawProbe
