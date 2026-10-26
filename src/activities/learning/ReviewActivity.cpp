#include "ReviewActivity.h"

#include <CloudConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <VoiceRelayChatClient.h>
#include <WiFi.h>
#include <WifiIdleManager.h>
#include <esp_random.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "LearningClient.h"
#include "LearningSync.h"
#include "MappedInputManager.h"
#include "ReviewStore.h"
#include "components/UITheme.h"

void ReviewActivity::onEnter() {
  Activity::onEnter();
  WIFI_IDLE.arm();
  CLOUD_CFG.load();
  relayConfigured_ = SETTINGS.voiceRelayEnabled != 0 && CLOUD_CFG.relayUrl[0] != '\0' &&
                     CLOUD_CFG.relayToken[0] != '\0' && CLOUD_CFG.relayDeviceId[0] != '\0';
  card_ = makeUniqueNoThrow<char[]>(REPLY_CAP + 1);
  if (!card_) {
    LOG_ERR("REVIEW", "OOM: card buffer %u bytes", static_cast<unsigned>(REPLY_CAP + 1));
    failText_ = tr(STR_MIC_OOM);
    state_ = State::Failed;
  } else {
    card_[0] = '\0';
  }
  requestUpdate();
}

void ReviewActivity::onExit() {
  card_.reset();
  queue_.clear();
  lines_.clear();
  Activity::onExit();
}

const char* ReviewActivity::currentWord() const {
  return index_ < queue_.size() ? queue_[index_].word : "";
}

void ReviewActivity::updateDueCache() {
  if (due_ >= 0 && !Learning::ReviewStore::saveDue(due_, static_cast<uint32_t>(time(nullptr)))) {
    LOG_ERR("REVIEW", "due-count cache write failed");
  }
}

void ReviewActivity::loadCachedQueue() {
  auto entries = makeUniqueNoThrow<Learning::ReviewFormat::QueueEntry[]>(QUEUE_CAP);
  if (!entries) {
    failText_ = tr(STR_MIC_OOM);
    state_ = State::Failed;
    return;
  }
  const size_t count = Learning::ReviewStore::loadQueue(entries.get(), QUEUE_CAP);
  queue_.assign(entries.get(), entries.get() + count);
  index_ = 0;
  uint32_t checkedAt = 0;
  int32_t cachedDue = 0;
  due_ = Learning::ReviewStore::loadDue(cachedDue, checkedAt) ? cachedDue : static_cast<int32_t>(count);
  if (queue_.empty()) {
    state_ = State::Empty;
  } else {
    showFront();
  }
}

void ReviewActivity::loadQueue() {
  if (!card_) {
    failText_ = tr(STR_MIC_OOM);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  if (!relayConfigured_ || WiFi.status() != WL_CONNECTED) {
    status_ = Status::Offline;
    loadCachedQueue();
    requestUpdate();
    return;
  }

  const auto words = Learning::LearningSync::syncPending(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId);
  const auto reviews = Learning::LearningSync::syncReviews(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId);
  if (reviews.error != nullptr) {
    LOG_ERR("REVIEW", "pending ratings could not sync: %s", reviews.error);
    status_ = Status::SyncFailed;
    loadCachedQueue();
    requestUpdate();
    return;
  }

  auto items = makeUniqueNoThrow<VoiceRelay::ReviewItem[]>(QUEUE_CAP);
  if (!items) {
    failText_ = tr(STR_MIC_OOM);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  size_t stored = 0;
  int32_t due = 0;
  VoiceRelay::LearningClient::Stats stats;
  const auto result = VoiceRelay::LearningClient::fetchReviewQueue(
      CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, items.get(), QUEUE_CAP, stored, due, &stats);
  if (result != VoiceRelay::LearningClient::Result::Ok) {
    LOG_ERR("REVIEW", "queue failed: %s (http %d)", VoiceRelay::LearningClient::resultName(result), stats.httpStatus);
    status_ = Status::Offline;
    loadCachedQueue();
    requestUpdate();
    return;
  }

  queue_.clear();
  queue_.reserve(stored);
  for (size_t i = 0; i < stored; ++i) {
    Learning::ReviewFormat::QueueEntry entry;
    entry.lastRating = items[i].lastRating;
    snprintf(entry.word, sizeof(entry.word), "%s", items[i].word);
    queue_.push_back(entry);
  }
  index_ = 0;
  due_ = due;
  if (!Learning::ReviewStore::saveQueue(queue_.data(), queue_.size())) {
    LOG_ERR("REVIEW", "queue cache write failed");
  }
  updateDueCache();
  status_ = words.error != nullptr ? Status::SyncFailed : Status::None;
  if (queue_.empty()) {
    state_ = State::Empty;
  } else {
    showFront();
  }
  requestUpdate();
}

void ReviewActivity::showFront() {
  state_ = State::Front;
  cardReady_ = false;
  cardAttempted_ = false;
  linesBuilt_ = false;
  scrollLine_ = 0;
  maxScroll_ = 0;
  ratingChoice_ = 0;
  if (card_) card_[0] = '\0';
}

void ReviewActivity::reveal() {
  if (!card_ || index_ >= queue_.size()) return;
  {
    RenderLock lock(*this);
    cardReady_ = Learning::ReviewStore::loadCard(currentWord(), card_.get(), REPLY_CAP + 1) > 0;
    linesBuilt_ = false;
    scrollLine_ = 0;
    if (cardReady_ || !relayConfigured_ || WiFi.status() != WL_CONNECTED) {
      state_ = State::Back;
    } else {
      cardAttempted_ = false;
      state_ = State::FetchingCard;
    }
  }
  requestUpdate();
}

void ReviewActivity::fetchCard() {
  bool truncated = false;
  VoiceRelay::ChatClient::Stats stats;
  const auto result = VoiceRelay::ChatClient::ask(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId,
                                                  currentWord(), card_.get(), REPLY_CAP + 1, truncated, nullptr, 0,
                                                  "word", &stats);
  cardReady_ = result == VoiceRelay::ChatClient::Result::Ok && card_[0] != '\0';
  if (cardReady_) {
    if (!Learning::ReviewStore::saveCard(currentWord(), card_.get())) {
      LOG_ERR("REVIEW", "card cache write failed: %s", currentWord());
    }
  } else {
    LOG_ERR("REVIEW", "card failed for %s: %s (http %d)", currentWord(),
            VoiceRelay::ChatClient::resultName(result), stats.httpStatus);
    card_[0] = '\0';
    status_ = Status::SyncFailed;
  }
  linesBuilt_ = false;
  state_ = State::Back;
  requestUpdate();
}

void ReviewActivity::openRating(const int choice) {
  ratingChoice_ = std::max(0, std::min(2, choice));
  state_ = State::Rating;
  requestUpdate();
}

void ReviewActivity::submitRating() {
  if (index_ >= queue_.size()) return;
  char eventId[Learning::ReviewFormat::EVENT_ID_CAP];
  VoiceRelay::formatRequestId(eventId, esp_random(), esp_random(), esp_random(), esp_random());
  if (!Learning::ReviewStore::enqueue(currentWord(), static_cast<uint8_t>(ratingChoice_ + 1), eventId)) {
    LOG_ERR("REVIEW", "rating queue write failed: %s", currentWord());
    status_ = Status::SaveFailed;
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(index_));
    if (index_ >= queue_.size()) index_ = 0;
    if (queue_.empty()) {
      state_ = State::Done;
    } else {
      showFront();
    }
  }
  if (due_ > 0) --due_;
  updateDueCache();
  if (!Learning::ReviewStore::saveQueue(queue_.data(), queue_.size())) {
    LOG_ERR("REVIEW", "queue cache update failed");
  }

  if (!relayConfigured_ || WiFi.status() != WL_CONNECTED) {
    status_ = Status::SyncFailed;
    requestUpdate();
    return;
  }
  status_ = Status::Syncing;
  requestUpdateAndWait();
  const auto outcome = Learning::LearningSync::syncReviews(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId);
  status_ = outcome.error == nullptr ? Status::Synced : Status::SyncFailed;
  if (outcome.error != nullptr) LOG_ERR("REVIEW", "rating sync failed: %s", outcome.error);
  requestUpdate();
}

void ReviewActivity::moveCard(const int delta) {
  if (queue_.empty()) return;
  const int count = static_cast<int>(queue_.size());
  {
    RenderLock lock(*this);
    index_ = static_cast<size_t>((static_cast<int>(index_) + delta + count) % count);
    showFront();
  }
  requestUpdate();
}

void ReviewActivity::moveRating(const int delta) {
  ratingChoice_ = (ratingChoice_ + delta + 3) % 3;
  requestUpdate();
}

void ReviewActivity::buildLines(const int fontId, const int textWidth) {
  lines_.clear();
  linesBuilt_ = true;
  builtFontId_ = fontId;
  builtWidth_ = textWidth;
  if (!cardReady_ || !card_) return;

  const char* p = card_.get();
  while (*p != '\0') {
    const char* nl = strchr(p, '\n');
    const size_t len = nl != nullptr ? static_cast<size_t>(nl - p) : strlen(p);
    std::string line(p, len);
    if (line.empty()) {
      lines_.emplace_back();
    } else {
      auto wrapped = renderer.wrappedText(fontId, line.c_str(), textWidth, MAX_WRAP_LINES);
      lines_.insert(lines_.end(), std::make_move_iterator(wrapped.begin()), std::make_move_iterator(wrapped.end()));
    }
    if (nl == nullptr) break;
    p = nl + 1;
  }
}

void ReviewActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state_ == State::Rating) {
      state_ = State::Back;
      requestUpdate();
    } else if (state_ == State::Back) {
      {
        RenderLock lock(*this);
        showFront();
      }
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  switch (state_) {
    case State::LoadingQueue:
      if (!started_) {
        started_ = true;
        requestUpdateAndWait();
        loadQueue();
      }
      break;
    case State::FetchingCard:
      if (!cardAttempted_) {
        cardAttempted_ = true;
        requestUpdateAndWait();
        fetchCard();
      }
      break;
    case State::Failed:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        started_ = false;
        failText_ = nullptr;
        state_ = State::LoadingQueue;
        requestUpdate();
      }
      break;
    case State::Empty:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        started_ = false;
        state_ = State::LoadingQueue;
        requestUpdate();
      }
      break;
    case State::Front:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        reveal();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                 mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
        moveCard(-1);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                 mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
        moveCard(1);
      }
      break;
    case State::Back:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        openRating(0);
      } else if ((mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                  mappedInput.wasReleased(MappedInputManager::Button::PageBack)) &&
                 scrollLine_ > 0) {
        --scrollLine_;
        requestUpdate();
      } else if ((mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                  mappedInput.wasReleased(MappedInputManager::Button::PageForward)) &&
                 scrollLine_ < maxScroll_) {
        ++scrollLine_;
        requestUpdate();
      }
      break;
    case State::Rating:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        submitRating();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                 mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
        moveRating(-1);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                 mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
        moveRating(1);
      }
      break;
    case State::Done:
      break;
  }
}

void ReviewActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int bodyFont = SETTINGS.getReaderFontId();
  const int left = safe.x + metrics.contentSidePadding;
  const int right = safe.x + safe.width - metrics.contentSidePadding;
  const int textWidth = right - left;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineH = renderer.getLineHeight(bodyFont);

  char title[80];
  if (!queue_.empty() && index_ < queue_.size()) {
    snprintf(title, sizeof(title), "%s %u / %u", tr(STR_REVIEW_TODAY), static_cast<unsigned>(index_ + 1),
             static_cast<unsigned>(queue_.size()));
  } else {
    snprintf(title, sizeof(title), "%s", tr(STR_REVIEW_TODAY));
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);

  const char* statusText = nullptr;
  switch (status_) {
    case Status::Offline:
      statusText = tr(STR_REVIEW_OFFLINE);
      break;
    case Status::Syncing:
      statusText = tr(STR_WORD_SYNCING);
      break;
    case Status::Synced:
      statusText = tr(STR_WORD_SYNCED);
      break;
    case Status::SyncFailed:
      statusText = tr(STR_WORD_SYNC_FAILED);
      break;
    case Status::SaveFailed:
      statusText = tr(STR_WORD_SAVE_FAILED);
      break;
    case Status::None:
      break;
  }
  const int statusH = statusText != nullptr ? lineH + 4 : 0;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing - statusH;

  const auto centered = [&](const char* text, const int y, const bool bold = false) {
    if (text == nullptr) return;
    const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int width = renderer.getTextWidth(bodyFont, text, style);
    renderer.drawText(bodyFont, left + (textWidth - width) / 2, y, text, true, style);
  };

  if (state_ == State::LoadingQueue) {
    centered(tr(STR_REVIEW_LOADING), contentTop + lineH * 2);
  } else if (state_ == State::Failed) {
    centered(tr(STR_REVIEW_FAILED), contentTop + lineH * 2, true);
    centered(failText_, contentTop + lineH * 3 + 8);
  } else if (state_ == State::Empty) {
    centered(tr(STR_REVIEW_DONE), contentTop + lineH * 2, true);
    auto hint = renderer.wrappedText(bodyFont, tr(STR_REVIEW_EMPTY_HINT), textWidth, 3);
    int y = contentTop + lineH * 4;
    for (const auto& line : hint) {
      centered(line.c_str(), y);
      y += lineH + 4;
    }
  } else if (state_ == State::Done) {
    centered(tr(STR_REVIEW_COMPLETE), contentTop + lineH * 2, true);
  } else if (state_ == State::Front) {
    centered(currentWord(), contentTop + lineH * 2, true);
  } else if (state_ == State::Rating) {
    centered(currentWord(), contentTop, true);
    centered(tr(STR_REVIEW_RATE_TITLE), contentTop + lineH * 2);
    const char* choices[] = {tr(STR_REVIEW_FORGOT), tr(STR_REVIEW_FUZZY), tr(STR_REVIEW_KNOWN)};
    const int optionH = lineH + 16;
    const int optionW = std::min(textWidth, 280);
    const int optionX = left + (textWidth - optionW) / 2;
    int y = contentTop + lineH * 4;
    for (int i = 0; i < 3; ++i) {
      if (i == ratingChoice_) renderer.fillRect(optionX, y - 6, optionW, optionH, true);
      const int width = renderer.getTextWidth(bodyFont, choices[i], EpdFontFamily::BOLD);
      renderer.drawText(bodyFont, optionX + (optionW - width) / 2, y, choices[i], i != ratingChoice_,
                        EpdFontFamily::BOLD);
      y += optionH + 8;
    }
  } else {
    centered(currentWord(), contentTop, true);
    const int ruleY = contentTop + lineH + 6;
    renderer.drawLine(left, ruleY, right - 1, ruleY, true);
    const int bodyTop = ruleY + 12;
    if (state_ == State::FetchingCard) {
      centered(tr(STR_REVIEW_LOADING), bodyTop + lineH);
    } else if (!cardReady_) {
      centered(tr(STR_REVIEW_NO_CARD), bodyTop + lineH);
    } else {
      if (!linesBuilt_ || builtFontId_ != bodyFont || builtWidth_ != textWidth) buildLines(bodyFont, textWidth);
      const int bodyLineH = lineH + 2;
      const int visibleLines = bodyLineH > 0 ? (contentBottom - bodyTop) / bodyLineH : 0;
      maxScroll_ = std::max(0, static_cast<int>(lines_.size()) - visibleLines);
      scrollLine_ = std::min(scrollLine_, maxScroll_);
      int y = bodyTop;
      for (size_t i = static_cast<size_t>(scrollLine_); i < lines_.size() && y <= contentBottom - bodyLineH; ++i) {
        renderer.drawText(bodyFont, left, y, lines_[i].c_str(), true);
        y += bodyLineH;
      }
    }
  }

  if (statusText != nullptr) {
    centered(statusText, safe.y + safe.height - metrics.verticalSpacing - lineH);
  }
  if (state_ == State::Back && maxScroll_ > 0) {
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  } else if (state_ == State::Front) {
    GUI.drawSideButtonHints(renderer, tr(STR_REVIEW_PREV), tr(STR_REVIEW_NEXT));
  } else if (state_ == State::Rating) {
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }

  const char* confirm = "";
  if (state_ == State::Front) {
    confirm = tr(STR_REVIEW_REVEAL);
  } else if (state_ == State::Back) {
    confirm = tr(STR_REVIEW_RATE_TITLE);
  } else if (state_ == State::Rating) {
    confirm = tr(STR_REVIEW_SUBMIT);
  } else if (state_ == State::Failed || state_ == State::Empty) {
    confirm = tr(STR_RETRY);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
