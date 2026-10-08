#include "apex/adapters/mock/mock_broker_gateway.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "apex/core/log.hpp"

namespace apex {
namespace {

bool is_terminal(OrderStatus status) {
  return status == OrderStatus::Filled || status == OrderStatus::Cancelled ||
         status == OrderStatus::Rejected || status == OrderStatus::Expired;
}

double side_sign(Side side) { return side == Side::Buy ? 1.0 : -1.0; }

}  // namespace

MockBrokerGateway::MockBrokerGateway(MockBrokerConfig config) : config_(config) {}

void MockBrokerGateway::set_order_update_callback(OrderUpdateCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  order_cb_ = std::move(callback);
}

void MockBrokerGateway::set_fill_callback(FillCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  fill_cb_ = std::move(callback);
}

bool MockBrokerGateway::connect() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connected_.load()) return true;
  cash_ = config_.starting_cash;
  connected_ = true;
  APEX_LOG_INFO("mock-broker", "connected with cash={:.2f}", cash_);
  return true;
}

void MockBrokerGateway::disconnect() {
  if (!connected_.exchange(false)) return;
  APEX_LOG_INFO("mock-broker", "disconnected");
}

OrderId MockBrokerGateway::next_id() {
  return "MOCK-" + std::to_string(++sequence_);
}

OrderId MockBrokerGateway::submit_order(const OrderRequest& request) {
  OrderUpdateCallback order_cb;
  FillCallback fill_cb;
  Order update;
  std::optional<Fill> execution;
  OrderId id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_.load()) {
      APEX_LOG_ERROR("mock-broker", "rejecting {} order for {}: not connected",
                     std::string(to_string(request.side)), request.symbol);
      return {};
    }

    id = next_id();
    Order order;
    order.id = id;
    order.request = request;
    order.status = OrderStatus::Accepted;
    order.created_at = now();
    order.updated_at = order.created_at;

    if (request.type == OrderType::Market &&
        config_.fill_marketable_immediately) {
      const auto price_it = prices_.find(request.symbol);
      if (price_it != prices_.end()) {
        execution = fill(order, request.quantity, price_it->second);
      }
    }

    orders_[id] = order;
    update = order;
    order_cb = order_cb_;
    fill_cb = fill_cb_;
  }

  if (order_cb) order_cb(update);
  if (execution && fill_cb) fill_cb(*execution);
  return id;
}

Fill MockBrokerGateway::fill(Order& order, Quantity quantity, Price price) {
  // Caller holds mutex_.
  const Quantity previous_filled = order.filled_quantity;
  const Price previous_notional =
      order.average_fill_price * static_cast<Price>(previous_filled);

  order.filled_quantity += quantity;
  order.average_fill_price =
      order.filled_quantity > 0
          ? (previous_notional + price * static_cast<Price>(quantity)) /
                static_cast<Price>(order.filled_quantity)
          : 0.0;
  order.status = order.filled_quantity >= order.request.quantity
                     ? OrderStatus::Filled
                     : OrderStatus::PartiallyFilled;
  order.updated_at = now();

  cash_ -= side_sign(order.request.side) * price * static_cast<Price>(quantity) +
           config_.commission_per_order;

  mark_position(order.request.symbol, order.request.side, quantity, price);

  Fill fill;
  fill.order_id = order.id;
  fill.symbol = order.request.symbol;
  fill.side = order.request.side;
  fill.quantity = quantity;
  fill.price = price;
  fill.ts = order.updated_at;
  return fill;
}

void MockBrokerGateway::mark_position(const Symbol& symbol, Side side,
                                      Quantity quantity, Price price) {
  // Caller holds mutex_.
  Position& position = positions_[symbol];
  position.symbol = symbol;
  position.last_price = price;

  const Quantity signed_quantity =
      side == Side::Buy ? quantity : -quantity;
  const Quantity old_quantity = position.quantity;
  const Quantity new_quantity = old_quantity + signed_quantity;

  const bool is_add_or_open =
      old_quantity == 0 || ((old_quantity > 0) == (signed_quantity > 0));
  if (is_add_or_open && new_quantity != 0) {
    const Price old_notional =
        position.average_price * static_cast<Price>(old_quantity);
    position.average_price =
        (old_notional + price * static_cast<Price>(signed_quantity)) /
        static_cast<Price>(new_quantity);
  } else if (new_quantity == 0) {
    position.average_price = 0.0;
  } else if ((new_quantity > 0) != (old_quantity > 0)) {
    // Flipped through zero into the opposite side; basis resets to this fill.
    position.average_price = price;
  }
  // Otherwise: a partial reduction in the same direction keeps the basis.

  position.quantity = new_quantity;
}

void MockBrokerGateway::update_price(const Symbol& symbol, Price price) {
  OrderUpdateCallback order_cb;
  FillCallback fill_cb;
  std::vector<Order> updates;
  std::vector<Fill> executions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    prices_[symbol] = price;

    const auto position_it = positions_.find(symbol);
    if (position_it != positions_.end()) {
      position_it->second.last_price = price;
    }

    for (auto& [id, order] : orders_) {
      (void)id;
      if (order.request.symbol != symbol) continue;
      if (order.status != OrderStatus::Accepted &&
          order.status != OrderStatus::PartiallyFilled) {
        continue;
      }
      if (order.request.type != OrderType::Limit &&
          order.request.type != OrderType::StopLimit) {
        continue;
      }
      if (!order.request.limit_price.has_value()) continue;

      const Price limit = *order.request.limit_price;
      const bool crossed = order.request.side == Side::Buy ? price <= limit
                                                           : price >= limit;
      if (!crossed) continue;

      const Quantity remaining = order.request.quantity - order.filled_quantity;
      if (remaining <= 0) continue;

      executions.push_back(fill(order, remaining, limit));
      updates.push_back(order);
    }

    order_cb = order_cb_;
    fill_cb = fill_cb_;
  }

  for (const auto& order : updates) {
    if (order_cb) order_cb(order);
  }
  for (const auto& execution : executions) {
    if (fill_cb) fill_cb(execution);
  }
}

bool MockBrokerGateway::cancel_order(const OrderId& order_id) {
  OrderUpdateCallback order_cb;
  Order update;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = orders_.find(order_id);
    if (it == orders_.end() || is_terminal(it->second.status)) return false;

    it->second.status = OrderStatus::Cancelled;
    it->second.updated_at = now();
    update = it->second;
    order_cb = order_cb_;
  }
  if (order_cb) order_cb(update);
  return true;
}

std::optional<Order> MockBrokerGateway::get_order(const OrderId& order_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = orders_.find(order_id);
  if (it == orders_.end()) return std::nullopt;
  return it->second;
}

std::vector<Order> MockBrokerGateway::open_orders() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Order> result;
  for (const auto& [id, order] : orders_) {
    (void)id;
    if (order.status == OrderStatus::Accepted ||
        order.status == OrderStatus::PartiallyFilled) {
      result.push_back(order);
    }
  }
  return result;
}

std::vector<Position> MockBrokerGateway::positions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Position> result;
  result.reserve(positions_.size());
  for (const auto& [symbol, position] : positions_) {
    if (position.quantity != 0) result.push_back(position);
  }
  return result;
}

std::optional<Position> MockBrokerGateway::position(const Symbol& symbol) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = positions_.find(symbol);
  if (it == positions_.end()) return std::nullopt;
  return it->second;
}

Price MockBrokerGateway::equity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Price total = cash_;
  for (const auto& [symbol, position] : positions_) {
    total += position.last_price * static_cast<Price>(position.quantity);
  }
  return total;
}

}  // namespace apex
