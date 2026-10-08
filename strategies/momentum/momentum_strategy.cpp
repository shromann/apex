// Example plugin strategy: lookback breakout with a trailing stop.
//
// Compiled into its own shared library (apex_momentum) and loaded at runtime by
// PluginLoader. It uses the very same IStrategy/StrategyParams contract as the
// built-in strategies and registers into the same global StrategyRegistry, which
// is exactly the point: nothing about it is special-cased by the engine.

#include <algorithm>
#include <cstddef>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "apex/core/log.hpp"
#include "apex/core/market_data.hpp"
#include "apex/core/order.hpp"
#include "apex/core/types.hpp"
#include "apex/plugin/plugin_entry.hpp"
#include "apex/strategy/strategy.hpp"
#include "apex/strategy/strategy_context.hpp"
#include "apex/strategy/strategy_registrar.hpp"

namespace apex {
namespace {

class MomentumStrategy final : public IStrategy {
 public:
  std::string_view name() const override { return "momentum"; }

  void configure(const StrategyParams& params) override {
    lookback_ = static_cast<std::size_t>(std::max(1.0, params.number("lookback", 10.0)));
    quantity_ = static_cast<Quantity>(std::max(0.0, params.number("quantity", 100.0)));
    trailing_stop_ = params.number("trailing_stop", 0.02);
    if (trailing_stop_ <= 0.0) trailing_stop_ = 0.02;
    symbols_ = params.symbols();
  }

  void on_start(IStrategyContext& ctx) override {
    ctx.log(LogLevel::Info,
            std::format("momentum started: lookback={} qty={} trailing_stop={:.4f} symbols={}",
                        lookback_, quantity_, trailing_stop_, symbols_.size()));
  }

  void on_bar(const Bar& bar, IStrategyContext& ctx) override {
    // Highest close over the *previous* lookback window (before this bar).
    std::deque<Price>& window = closes_[bar.symbol];
    const bool enough_history = window.size() >= lookback_;
    double prior_high = 0.0;
    if (enough_history) {
      prior_high = *std::max_element(window.begin(), window.end());
    }
    window.push_back(bar.close);
    while (window.size() > lookback_) window.pop_front();

    const std::optional<Position> position = ctx.position(bar.symbol);
    const bool is_long = position.has_value() && position->quantity > 0;

    if (!is_long) {
      // Enter on a breakout above the recent range.
      if (enough_history && quantity_ > 0 && bar.close > prior_high) {
        ctx.market_buy(bar.symbol, quantity_, "momentum");
        entry_[bar.symbol] = bar.close;
        peak_[bar.symbol] = bar.close;
        ctx.log(LogLevel::Info,
                std::format("momentum {} breakout close={:.4f} > high={:.4f} -> BUY {}",
                            bar.symbol, bar.close, prior_high, quantity_));
      }
      return;
    }

    // Manage an open long with a trailing stop off the peak close.
    double& peak = peak_[bar.symbol];
    if (peak <= 0.0) peak = bar.close;
    peak = std::max(peak, bar.close);

    double& entry = entry_[bar.symbol];
    if (entry <= 0.0) entry = position->average_price;

    const double stop = peak * (1.0 - trailing_stop_);
    if (bar.close < stop) {
      ctx.market_sell(bar.symbol, position->quantity, "momentum");
      ctx.log(LogLevel::Info,
              std::format("momentum {} trailing stop close={:.4f} < stop={:.4f} -> SELL {}",
                          bar.symbol, bar.close, stop, position->quantity));
      entry = 0.0;
      peak = 0.0;
    }
  }

  void on_fill(const Fill& fill, IStrategyContext& ctx) override {
    ctx.log(LogLevel::Info,
            std::format("momentum fill {} {} {} @ {:.4f}", fill.symbol,
                        to_string(fill.side), fill.quantity, fill.price));
  }

 private:
  std::size_t lookback_{10};
  Quantity quantity_{100};
  double trailing_stop_{0.02};
  std::vector<Symbol> symbols_;
  std::unordered_map<Symbol, std::deque<Price>> closes_;
  std::unordered_map<Symbol, double> entry_;
  std::unordered_map<Symbol, double> peak_;
};

}  // namespace

APEX_REGISTER_STRATEGY(MomentumStrategy, "momentum");

}  // namespace apex

// C-linkage entry point resolved by PluginLoader after dlopen().
APEX_PLUGIN_ENTRY() {}
