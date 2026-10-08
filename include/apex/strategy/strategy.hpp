#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "apex/core/market_data.hpp"
#include "apex/core/order.hpp"
#include "apex/strategy/strategy_context.hpp"

namespace apex {

// Free-form, string-keyed configuration so strategies can be tuned without the
// engine knowing anything about their parameters.
class StrategyParams {
 public:
  StrategyParams() = default;
  explicit StrategyParams(std::vector<Symbol> symbols)
      : symbols_(std::move(symbols)) {}

  const std::vector<Symbol>& symbols() const { return symbols_; }
  StrategyParams& set_symbols(std::vector<Symbol> symbols) {
    symbols_ = std::move(symbols);
    return *this;
  }

  StrategyParams& set(std::string key, double value) {
    numbers_.insert_or_assign(std::move(key), value);
    return *this;
  }
  StrategyParams& set(std::string key, std::string value) {
    strings_.insert_or_assign(std::move(key), std::move(value));
    return *this;
  }

  bool has(std::string_view key) const {
    return numbers_.find(std::string(key)) != numbers_.end() ||
           strings_.find(std::string(key)) != strings_.end();
  }

  double number(std::string_view key, double fallback = 0.0) const {
    auto it = numbers_.find(std::string(key));
    return it == numbers_.end() ? fallback : it->second;
  }

  std::string string(std::string_view key, std::string fallback = {}) const {
    auto it = strings_.find(std::string(key));
    return it == strings_.end() ? std::move(fallback) : it->second;
  }

 private:
  std::vector<Symbol> symbols_;
  std::unordered_map<std::string, double> numbers_;
  std::unordered_map<std::string, std::string> strings_;
};

// The extension point. Every strategy implements this interface and reacts to
// market and execution events. It has no knowledge of the transport, broker, or
// threading model -- only the IStrategyContext it is handed.
class IStrategy {
 public:
  virtual ~IStrategy() = default;

  // Stable identifier used for registry lookup and CLI selection.
  virtual std::string_view name() const = 0;

  // Called once before on_start with the resolved parameters.
  virtual void configure(const StrategyParams& params) { (void)params; }

  virtual void on_start(IStrategyContext& ctx) { (void)ctx; }
  virtual void on_quote(const Quote& quote, IStrategyContext& ctx) {
    (void)quote;
    (void)ctx;
  }
  virtual void on_bar(const Bar& bar, IStrategyContext& ctx) {
    (void)bar;
    (void)ctx;
  }
  virtual void on_order_update(const Order& order, IStrategyContext& ctx) {
    (void)order;
    (void)ctx;
  }
  virtual void on_fill(const Fill& fill, IStrategyContext& ctx) {
    (void)fill;
    (void)ctx;
  }
  virtual void on_timer(Timestamp time, IStrategyContext& ctx) {
    (void)time;
    (void)ctx;
  }
  virtual void on_stop(IStrategyContext& ctx) { (void)ctx; }
};

}  // namespace apex
