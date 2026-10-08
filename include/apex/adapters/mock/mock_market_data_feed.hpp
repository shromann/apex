#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "apex/market/market_data_feed.hpp"

namespace apex {

struct MockFeedConfig {
  std::chrono::milliseconds interval{25};  // delay between ticks (background mode)
  unsigned seed{42};
  Price start_price{100.0};
  double volatility{0.20};  // per-tick standard deviation, in price units
  double drift{0.0};        // per-tick drift, in price units
  std::size_t bar_every{1};  // emit a Bar every N quotes
  bool background{true};     // false => drive manually with emit_tick()
};

// Simulated feed producing a geometric random walk per symbol. Fully functional
// so the engine runs end to end without a live market connection.
class MockMarketDataFeed final : public IMarketDataFeed {
 public:
  explicit MockMarketDataFeed(MockFeedConfig config = {});

  std::string_view name() const override { return "mock-feed"; }

  void set_quote_callback(QuoteCallback callback) override;
  void set_bar_callback(BarCallback callback) override;
  bool subscribe(const Symbol& symbol) override;
  void unsubscribe(const Symbol& symbol) override;
  bool start() override;
  void stop() override;
  bool is_running() const override { return running_.load(); }
  bool is_connected() const override { return running_.load(); }

  // Deterministic helpers (used by tests and by background=false callers).
  std::size_t emit_tick();  // advances every subscribed symbol once
  void set_price(const Symbol& symbol, Price price);
  std::size_t ticks() const { return tick_count_; }

 private:
  void run_loop();
  void next_price(const Symbol& symbol);

  MockFeedConfig config_;
  mutable std::mutex mutex_;
  QuoteCallback quote_cb_;
  BarCallback bar_cb_;
  std::vector<Symbol> symbols_;
  std::unordered_map<Symbol, Price> prices_;
  std::unordered_map<Symbol, Price> session_open_;
  std::mt19937 rng_;
  std::normal_distribution<double> gaussian_{0.0, 1.0};
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::size_t tick_count_{0};
};

}  // namespace apex
