// Built-in strategy: simple moving-average crossover.
//
// Demonstrates the minimal shape of a strategy for apex. The class depends only
// on IStrategy (and the IStrategyContext it is handed) -- it never sees a feed,
// a broker, or a thread. It self-registers with the global StrategyRegistry via
// APEX_REGISTER_STRATEGY, so the engine can instantiate it by the name
// "ma_cross" with no engine-side changes.

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
#include "apex/strategy/strategy.hpp"
#include "apex/strategy/strategy_context.hpp"
#include "apex/strategy/strategy_registrar.hpp"

namespace apex {
namespace {

// Mean of the trailing `n` entries of a closed price window.
double trailing_mean(const std::deque<Price>& window, std::size_t n) {
  const std::size_t count = std::min(n, window.size());
  if (count == 0) return 0.0;
  double sum = 0.0;
  for (std::size_t i = window.size() - count; i < window.size(); ++i) {
    sum += window[i];
  }
  return sum / static_cast<double>(count);
}

class MovingAverageCrossStrategy final : public IStrategy {
 public:
  std::string_view name() const override { return "ma_cross"; }

  void configure(const StrategyParams& params) override {
    fast_ = static_cast<std::size_t>(std::max(1.0, params.number("fast", 5.0)));
    slow_ = static_cast<std::size_t>(std::max(1.0, params.number("slow", 20.0)));
    if (fast_ >= slow_) {
      std::swap(fast_, slow_);
      APEX_LOG_WARN("ma_cross",
                    "fast period must be smaller than slow; swapped to fast={} slow={}",
                    fast_, slow_);
    }
    quantity_ = static_cast<Quantity>(std::max(0.0, params.number("quantity", 100.0)));
    symbols_ = params.symbols();
  }

  void on_start(IStrategyContext& ctx) override {
    ctx.log(LogLevel::Info,
            std::format("ma_cross started: fast={} slow={} qty={} symbols={}",
                        fast_, slow_, quantity_, symbols_.size()));
  }

  void on_bar(const Bar& bar, IStrategyContext& ctx) override {
    // Maintain a rolling window of closes, capped at the slow period.
    std::deque<Price>& window = closes_[bar.symbol];
    window.push_back(bar.close);
    while (window.size() > slow_) window.pop_front();

    if (window.size() < slow_) return;  // not enough history yet

    const double fast_ma = trailing_mean(window, fast_);
    const double slow_ma = trailing_mean(window, slow_);
    const int relation = fast_ma > slow_ma ? 1 : (fast_ma < slow_ma ? -1 : 0);

    // First observation for this symbol: remember it, emit nothing.
    auto [it, inserted] = last_relation_.try_emplace(bar.symbol, relation);
    if (inserted) return;

    const int previous = it->second;
    if (relation == previous) return;  // still on the same side
    it->second = relation;

    const std::optional<Position> position = ctx.position(bar.symbol);
    const bool is_long = position.has_value() && position->quantity > 0;

    if (previous < 0 && relation > 0) {
      // Bullish cross: go long if we are flat.
      if (!is_long && quantity_ > 0) {
        ctx.market_buy(bar.symbol, quantity_, "ma_cross");
        ctx.log(LogLevel::Info,
                std::format("ma_cross {} bullish cross fast={:.4f} slow={:.4f} -> BUY {}",
                            bar.symbol, fast_ma, slow_ma, quantity_));
      }
    } else if (previous > 0 && relation < 0) {
      // Bearish cross: flatten any long position.
      if (is_long) {
        ctx.market_sell(bar.symbol, position->quantity, "ma_cross");
        ctx.log(LogLevel::Info,
                std::format("ma_cross {} bearish cross fast={:.4f} slow={:.4f} -> SELL {}",
                            bar.symbol, fast_ma, slow_ma, position->quantity));
      }
    }
  }

  void on_fill(const Fill& fill, IStrategyContext& ctx) override {
    ctx.log(LogLevel::Info,
            std::format("ma_cross fill {} {} {} @ {:.4f}", fill.symbol,
                        to_string(fill.side), fill.quantity, fill.price));
  }

 private:
  std::size_t fast_{5};
  std::size_t slow_{20};
  Quantity quantity_{100};
  std::vector<Symbol> symbols_;
  std::unordered_map<Symbol, std::deque<Price>> closes_;
  std::unordered_map<Symbol, int> last_relation_;
};

}  // namespace

APEX_REGISTER_STRATEGY(MovingAverageCrossStrategy, "ma_cross");

}  // namespace apex
