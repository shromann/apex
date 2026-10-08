#include "apex/adapters/mock/mock_market_data_feed.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "apex/core/log.hpp"

namespace apex {

MockMarketDataFeed::MockMarketDataFeed(MockFeedConfig config)
    : config_(config), rng_(config.seed) {}

void MockMarketDataFeed::set_quote_callback(QuoteCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  quote_cb_ = std::move(callback);
}

void MockMarketDataFeed::set_bar_callback(BarCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  bar_cb_ = std::move(callback);
}

bool MockMarketDataFeed::subscribe(const Symbol& symbol) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::find(symbols_.begin(), symbols_.end(), symbol) == symbols_.end()) {
    symbols_.push_back(symbol);
    prices_[symbol] = config_.start_price;
    session_open_[symbol] = config_.start_price;
  }
  return true;
}

void MockMarketDataFeed::unsubscribe(const Symbol& symbol) {
  std::lock_guard<std::mutex> lock(mutex_);
  symbols_.erase(std::remove(symbols_.begin(), symbols_.end(), symbol),
                 symbols_.end());
  prices_.erase(symbol);
  session_open_.erase(symbol);
}

bool MockMarketDataFeed::start() {
  if (running_.exchange(true)) return false;
  if (config_.background) {
    thread_ = std::thread(&MockMarketDataFeed::run_loop, this);
  }
  APEX_LOG_INFO("mock-feed", "started (interval={}ms, background={})",
                config_.interval.count(), config_.background);
  return true;
}

void MockMarketDataFeed::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  APEX_LOG_INFO("mock-feed", "stopped");
}

void MockMarketDataFeed::next_price(const Symbol& symbol) {
  // Caller holds mutex_.
  const double base = prices_[symbol];
  double next = base + config_.drift + config_.volatility * gaussian_(rng_);
  if (next < 0.01) next = 0.01;
  prices_[symbol] = next;
}

std::size_t MockMarketDataFeed::emit_tick() {
  std::vector<Quote> quotes;
  std::vector<Bar> bars;
  QuoteCallback quote_cb;
  BarCallback bar_cb;
  std::size_t tick = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++tick_count_;
    tick = tick_count_;
    quote_cb = quote_cb_;
    bar_cb = bar_cb_;
    quotes.reserve(symbols_.size());

    for (const auto& symbol : symbols_) {
      const double previous = prices_[symbol];
      next_price(symbol);
      const double price = prices_[symbol];
      const double high = std::max(previous, price);
      const double low = std::min(previous, price);

      Quote quote;
      quote.symbol = symbol;
      quote.bid = price - 0.01;
      quote.ask = price + 0.01;
      quote.last = price;
      quote.bid_size = 100;
      quote.ask_size = 100;
      quote.last_size = 100;
      quote.ts = now();
      quotes.push_back(std::move(quote));

      if (config_.bar_every > 0 && tick % config_.bar_every == 0) {
        Bar bar;
        bar.symbol = symbol;
        bar.ts = now();
        bar.open = session_open_[symbol];
        bar.high = high;
        bar.low = low;
        bar.close = price;
        bar.volume = 100;
        bars.push_back(std::move(bar));
      }
    }
  }

  // Invoke callbacks only after releasing the lock so a re-entrant subscriber
  // cannot deadlock against this feed.
  if (quote_cb) {
    for (const auto& quote : quotes) quote_cb(quote);
  }
  if (bar_cb) {
    for (const auto& bar : bars) bar_cb(bar);
  }
  return tick;
}

void MockMarketDataFeed::set_price(const Symbol& symbol, Price price) {
  std::lock_guard<std::mutex> lock(mutex_);
  prices_[symbol] = price;
}

void MockMarketDataFeed::run_loop() {
  while (running_.load()) {
    emit_tick();
    if (config_.interval.count() > 0) {
      std::this_thread::sleep_for(config_.interval);
    }
  }
}

}  // namespace apex
