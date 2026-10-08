#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "apex/broker/broker_gateway.hpp"

namespace apex {

struct MockBrokerConfig {
  Price starting_cash{100000.0};
  Price commission_per_order{0.0};
  bool fill_marketable_immediately{true};
};

// Paper-trading broker. Market orders fill immediately at the latest price the
// gateway has been told about (see update_price); resting limit orders fill when
// update_price crosses them. No real order ever leaves the process.
class MockBrokerGateway final : public IBrokerGateway {
 public:
  explicit MockBrokerGateway(MockBrokerConfig config = {});

  std::string_view name() const override { return "mock-broker"; }

  void set_order_update_callback(OrderUpdateCallback callback) override;
  void set_fill_callback(FillCallback callback) override;

  bool connect() override;
  void disconnect() override;
  bool is_connected() const override { return connected_.load(); }

  OrderId submit_order(const OrderRequest& request) override;
  bool cancel_order(const OrderId& order_id) override;

  std::optional<Order> get_order(const OrderId& order_id) const override;
  std::vector<Order> open_orders() const override;

  std::vector<Position> positions() const override;
  std::optional<Position> position(const Symbol& symbol) const override;

  // Not part of the IBrokerGateway port: the simulation's price book. Wire this
  // to market data (e.g. via the engine EventBus) so fills use real prices.
  void update_price(const Symbol& symbol, Price price);
  Price cash() const { return cash_; }
  Price equity() const;

 private:
  OrderId next_id();
  Fill fill(Order& order, Quantity quantity, Price price);
  void mark_position(const Symbol& symbol, Side side, Quantity quantity, Price price);

  MockBrokerConfig config_;
  mutable std::mutex mutex_;
  OrderUpdateCallback order_cb_;
  FillCallback fill_cb_;
  std::unordered_map<Symbol, Price> prices_;
  std::unordered_map<OrderId, Order> orders_;
  std::unordered_map<Symbol, Position> positions_;
  std::atomic<bool> connected_{false};
  std::atomic<std::uint64_t> sequence_{0};
  Price cash_{0.0};
};

}  // namespace apex
