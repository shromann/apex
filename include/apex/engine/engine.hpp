#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "apex/broker/broker_gateway.hpp"
#include "apex/core/event.hpp"
#include "apex/engine/event_bus.hpp"
#include "apex/market/market_data_feed.hpp"
#include "apex/strategy/strategy.hpp"
#include "apex/strategy/strategy_context.hpp"

namespace apex {

struct EngineConfig {
  std::vector<Symbol> symbols{};
  bool subscribe_on_start{true};
  bool publish_market_data{true};
};

// The orchestrator. It owns a market data feed, a broker gateway, and the active
// strategy; wires the adapters' callbacks to the strategy; and implements
// IStrategyContext so strategies can trade and query state. Swapping a strategy,
// a feed, or a broker means swapping one injected object -- no engine change.
class Engine final : public IStrategyContext {
 public:
  Engine(EngineConfig config, std::unique_ptr<IMarketDataFeed> feed,
         std::unique_ptr<IBrokerGateway> broker);
  ~Engine() override;

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // --- Wiring --------------------------------------------------------------
  void set_strategy(std::unique_ptr<IStrategy> strategy);
  void configure_strategy(const StrategyParams& params);
  IStrategy* strategy() const { return strategy_.get(); }

  IMarketDataFeed& feed() { return *feed_; }
  IBrokerGateway& broker() { return *broker_; }
  EventBus& bus() { return bus_; }
  const EngineConfig& config() const { return config_; }

  // --- Lifecycle -----------------------------------------------------------
  // Connects the broker, subscribes symbols, starts the feed, and calls the
  // strategy's on_start. Returns false if the broker or feed fails to start.
  bool start();
  void stop();
  bool is_running() const { return running_.load(); }

  // --- IStrategyContext ----------------------------------------------------
  OrderId submit_order(const OrderRequest& request) override;
  bool cancel_order(const OrderId& order_id) override;
  int cancel_all(const Symbol& symbol) override;

  std::optional<Position> position(const Symbol& symbol) const override;
  std::vector<Position> positions() const override;
  std::optional<Order> order(const OrderId& order_id) const override;
  std::vector<Order> open_orders() const override;
  std::optional<Quote> last_quote(const Symbol& symbol) const override;
  std::optional<Price> last_price(const Symbol& symbol) const override;

  Timestamp time() const override { return now(); }
  void log(LogLevel level, std::string_view message) const override;

 private:
  void handle_quote(const Quote& quote);
  void handle_bar(const Bar& bar);
  void handle_order_update(const Order& order);
  void handle_fill(const Fill& fill);

  EngineConfig config_;
  std::unique_ptr<IMarketDataFeed> feed_;
  std::unique_ptr<IBrokerGateway> broker_;
  std::unique_ptr<IStrategy> strategy_;
  EventBus bus_;
  std::atomic<bool> running_{false};

  mutable std::mutex mutex_;
  std::unordered_map<Symbol, Quote> last_quotes_;
};

}  // namespace apex
