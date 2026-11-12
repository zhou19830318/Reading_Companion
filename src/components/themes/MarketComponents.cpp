#include <GfxRenderer.h>
#include <I18n.h>
#include <MarketClient.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "BaseTheme.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
const char* title(const Markets::Card& card, bool& known) {
  struct Name {
    const char* id;
    StrId label;
  };
  static constexpr Name names[] = {{"sse", StrId::STR_MARKET_SSE},       {"szse", StrId::STR_MARKET_SZSE},
                                   {"csi300", StrId::STR_MARKET_CSI300}, {"chinext", StrId::STR_MARKET_CHINEXT},
                                   {"hsi", StrId::STR_MARKET_HSI},       {"hscei", StrId::STR_MARKET_HSCEI},
                                   {"sp500", StrId::STR_MARKET_SP500},   {"nasdaq", StrId::STR_MARKET_NASDAQ},
                                   {"dow", StrId::STR_MARKET_DOW},       {"gold", StrId::STR_MARKET_GOLD},
                                   {"silver", StrId::STR_MARKET_SILVER}, {"wti", StrId::STR_MARKET_WTI},
                                   {"brent", StrId::STR_MARKET_BRENT}};
  for (const auto& name : names) {
    if (strcmp(card.id, name.id) == 0) {
      known = true;
      return I18N.get(name.label);
    }
  }
  known = false;
  return card.name;
}

// Rendering uses bounded stack text, never allocates one string per chart point.
void text(const GfxRenderer& renderer, int x, int y, int width, const char* value) {
  char buf[192];
  snprintf(buf, sizeof(buf), "%s", value);
  size_t length = strlen(buf);
  while (length && renderer.getTextWidth(UI_10_FONT_ID, buf) > width) {
    --length;
    while (length && (static_cast<unsigned char>(buf[length]) & 0xC0) == 0x80) --length;
    buf[length] = '\0';
  }
  renderer.drawText(UI_10_FONT_ID, x, y, buf);
}

void plot(const GfxRenderer& renderer, Rect rect, const Markets::Card& card, bool axis, bool baseline) {
  if (rect.width < 4 || rect.height < 4 || card.pointCount < 2) return;
  double low = card.points[0].price, high = low;
  for (size_t i = 1; i < card.pointCount; ++i) {
    low = std::min(low, static_cast<double>(card.points[i].price));
    high = std::max(high, static_cast<double>(card.points[i].price));
  }
  if (baseline && card.hasPrevious) {
    low = std::min(low, card.previousClose);
    high = std::max(high, card.previousClose);
  }
  if (high == low) {
    const double pad = std::max(0.01, std::abs(low) * 0.001);
    high += pad;
    low -= pad;
  }
  if (axis) {
    char label[40];
    snprintf(label, sizeof(label), "%.2f", high);
    text(renderer, rect.x, rect.y, rect.width, label);
    snprintf(label, sizeof(label), "%.2f", low);
    text(renderer, rect.x, rect.y + rect.height - UITheme::getInstance().getMetrics().listRowHeight, rect.width, label);
    const int margin = std::min(rect.width / 3, renderer.getTextWidth(UI_10_FONT_ID, label) + 16);
    rect.x += margin;
    rect.width -= margin;
  }
  const auto y = [&](double value) {
    return rect.y + rect.height - 1 - static_cast<int>((value - low) / (high - low) * (rect.height - 1));
  };
  if (baseline && card.hasPrevious) {
    for (int x = rect.x; x < rect.x + rect.width; x += 8) {
      renderer.drawLine(x, y(card.previousClose), std::min(x + 3, rect.x + rect.width - 1), y(card.previousClose));
    }
  }
  const uint32_t first = card.points[0].time;
  const uint32_t span = card.points[card.pointCount - 1].time - first;
  if (!span) return;
  for (size_t i = 1; i < card.pointCount; ++i) {
    const int x0 = rect.x + static_cast<uint64_t>(card.points[i - 1].time - first) * (rect.width - 1) / span;
    const int x1 = rect.x + static_cast<uint64_t>(card.points[i].time - first) * (rect.width - 1) / span;
    renderer.drawLine(x0, y(card.points[i - 1].price), x1, y(card.points[i].price));
  }
  if (axis) renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
}
}  // namespace

void BaseTheme::drawMarketCard(const GfxRenderer& renderer, Rect rect, const Markets::Card& card, bool selected,
                               bool sparkline) const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int pad = std::max(4, m.verticalSpacing / 2);
  const int line = std::max(20, renderer.getFontAscenderSize(UI_10_FONT_ID) + 4);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height, selected ? 3 : 1, true);
  const int x = rect.x + pad;
  const int width = rect.width - 2 * pad;
  int y = rect.y + pad;
  char label[128];
  bool known = false;
  const char* name = title(card, known);
  if (known) {
    snprintf(label, sizeof(label), "%s", name);
  } else {
    snprintf(label, sizeof(label), "%s  %s", name, card.id);
  }
  text(renderer, x, y, width, label);
  y += line;
  if (!card.available) {
    const char* message = tr(STR_MARKET_NO_DATA);
    if (strcmp(card.error, "rate_limited") == 0) {
      message = tr(STR_MARKET_RATE_LIMIT);
    } else if (strcmp(card.error, "retry_later") == 0) {
      message = tr(STR_MARKET_RETRY_LATER);
    }
    text(renderer, x, y, width, message);
    return;
  }
  const int graphWidth = sparkline ? width / 3 : 0;
  const char* unit = strcmp(card.unit, "index") == 0 ? tr(STR_MARKET_POINTS) : card.currency;
  snprintf(label, sizeof(label), "%.2f %s%s%s", card.price, unit,
           strcmp(card.unit, "oz") == 0 || strcmp(card.unit, "bbl") == 0 ? "/" : "",
           strcmp(card.unit, "oz") == 0 || strcmp(card.unit, "bbl") == 0 ? card.unit : "");
  text(renderer, x, y, width - graphWidth, label);
  y += line;
  if (card.hasChange) {
    if (sparkline) {
      snprintf(label, sizeof(label), "%+.2f (%+.2f%%)", card.change, card.changePct);
    } else {
      snprintf(label, sizeof(label), "%s %+.2f (%+.2f%%)", tr(STR_MARKET_DAILY), card.change, card.changePct);
    }
  } else {
    snprintf(label, sizeof(label), "%s", tr(STR_MARKET_CHANGE_UNKNOWN));
  }
  text(renderer, x, y, width - graphWidth, label);
  if (sparkline) {
    const Rect graph{x + width - graphWidth, rect.y + pad + line, graphWidth - pad, line * 2};
    if (card.pointCount < 2) {
      text(renderer, graph.x, graph.y, graph.width, tr(STR_MARKET_NO_INTRADAY));
    } else {
      plot(renderer, graph, card, false, false);
    }
  }
  y += line;
  if (!sparkline && (card.hasPrevious || card.hasRange) && y + line <= rect.y + rect.height) {
    // Prev and the day range share one line, split left/right: a single
    // "prev high low" string overflows on five-digit prices and the text
    // helper would clip the low value's decimals.
    char stats[96] = {};
    if (card.hasPrevious && card.hasRange) {
      snprintf(stats, sizeof(stats), "%s %.2f", tr(STR_MARKET_PREV), card.previousClose);
      text(renderer, x, y, width * 2 / 5, stats);
      snprintf(stats, sizeof(stats), "%s %.2f  %s %.2f", tr(STR_MARKET_HIGH), card.high, tr(STR_MARKET_LOW), card.low);
      text(renderer, x + width * 2 / 5, y, width - width * 2 / 5, stats);
    } else if (card.hasPrevious) {
      snprintf(stats, sizeof(stats), "%s %.2f", tr(STR_MARKET_PREV), card.previousClose);
      text(renderer, x, y, width, stats);
    } else {
      snprintf(stats, sizeof(stats), "%s %.2f  %s %.2f", tr(STR_MARKET_HIGH), card.high, tr(STR_MARKET_LOW), card.low);
      text(renderer, x, y, width, stats);
    }
    y += line;
  }
  snprintf(label, sizeof(label), "%s %s", card.stale ? tr(STR_MARKET_STALE) : tr(STR_MARKET_DELAYED), card.timeLabel);
  if (y + line <= rect.y + rect.height) text(renderer, x, y, width, label);
}

void BaseTheme::drawMarketChart(const GfxRenderer& renderer, Rect rect, const Markets::Card& card,
                                bool intraday) const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int line = std::max(20, renderer.getFontAscenderSize(UI_10_FONT_ID) + 4);
  const int footer = line * 4;
  if (!card.available || card.pointCount < 2) {
    text(renderer, rect.x, rect.y, rect.width, tr(STR_MARKET_NO_CHART));
  } else {
    const Rect chart{rect.x, rect.y, rect.width, rect.height - footer - m.verticalSpacing};
    plot(renderer, chart, card, true, intraday);
    text(renderer, rect.x, chart.y + chart.height, rect.width / 2, card.startLabel);
    text(renderer, rect.x + rect.width / 2, chart.y + chart.height, rect.width / 2, card.endLabel);
  }
  char label[128];
  int y = rect.y + rect.height - line * 3;
  snprintf(label, sizeof(label), "%s %s  %s %s", tr(STR_MARKET_SOURCE), card.source, tr(STR_MARKET_TIMEZONE),
           card.timezone);
  text(renderer, rect.x, y, rect.width, label);
  y += line;
  text(renderer, rect.x, y, rect.width, intraday ? tr(STR_MARKET_BASELINE) : tr(STR_MARKET_RAW_CLOSE));
  y += line;
  text(renderer, rect.x, y, rect.width, tr(STR_MARKET_DISCLAIMER));
}
