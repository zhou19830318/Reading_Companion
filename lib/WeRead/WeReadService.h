#pragma once

#include <cstddef>

#include "WeReadClient.h"

// Process-wide state for the WeRead screens: CloudConfig credentials and the
// last shelf snapshot. The client stays stateless so the detail screen can
// reuse the same transport.
class WeReadService {
 public:
  static WeReadService& getInstance();

  // Loads CloudConfig. Safe to call on every activity entry (NVS read only).
  // A configured relay does not imply the server has a WeRead key; the shelf
  // fetch reports NotConfigured if the key is missing.
  bool begin();
  bool configured() const;

  // Pull server-managed relay URL/token/device ID and persist only when one
  // changed. Called before the shelf refresh; failure leaves NVS untouched.
  WeReadClient::Result syncConfig();

  WeReadClient::Result refresh();
  WeReadClient::Result fetchBook(const char* bookId, WeRead::BookDetail& out);
  WeReadClient::Result fetchStats(WeRead::Stats& out);

  const WeRead::Book* books() const { return books_; }
  size_t bookCount() const { return bookCount_; }
  size_t total() const { return total_; }
  const char* lastError() const { return lastError_; }

 private:
  WeReadService() = default;

  bool loaded_ = false;
  WeRead::Book books_[WeRead::MAX_BOOKS] = {};
  size_t bookCount_ = 0;
  size_t total_ = 0;
  const char* lastError_ = "";
};

#define WEREAD_SERVICE WeReadService::getInstance()
