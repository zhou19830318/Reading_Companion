#pragma once

#include <cstddef>
#include <cstdint>

#include "PigeonClient.h"

using Pigeon::PigeonClient;

// Process-wide state for the pigeon screens: CloudConfig credentials, the last
// heartbeat's peer list, and the unread count. The client stays stateless so
// its HTTP exchange can be reused by the conversation screen.
class PigeonService {
 public:
  static PigeonService& getInstance();

  // Loads CloudConfig. Safe to call on every activity entry (NVS read only).
  bool begin();
  bool configured() const;
  const char* deviceId() const;

  PigeonClient::Result refresh();
  PigeonClient::Result sendText(const char* peerId, const char* text, Pigeon::Message* out = nullptr);
  PigeonClient::Result fetchInbox(uint32_t afterId, Pigeon::Message* out, size_t cap, size_t& count,
                                  uint32_t& latestId);
  PigeonClient::Result fetchHistory(const char* peerId, Pigeon::Message* out, size_t cap, size_t& count);
  PigeonClient::Result ack(const uint32_t* ids, size_t count);
  PigeonClient::Result clearHistory(const char* peerId);

  const Pigeon::Device* devices() const { return devices_; }
  size_t deviceCount() const { return deviceCount_; }
  int unread() const { return unread_; }
  const char* lastError() const { return lastError_; }

 private:
  PigeonService() = default;

  bool loaded_ = false;
  Pigeon::Device devices_[Pigeon::MAX_DEVICES] = {};
  size_t deviceCount_ = 0;
  int unread_ = 0;
  const char* lastError_ = "";
  char eventId_[17] = {};
  void nextEventId();
};

#define PIGEON_SERVICE PigeonService::getInstance()
