#pragma once

#include <optional>
#include <string>
#include <utility>

#include "apex/core/types.hpp"

namespace apex {

// An intent to trade, produced by a strategy and handed to a broker gateway.
// The strategy fills in what it wants; the gateway assigns the canonical id.
struct OrderRequest {
  Symbol symbol;
  Side side{Side::Buy};
  OrderType type{OrderType::Market};
  Quantity quantity{0};
  std::optional<Price> limit_price{};
  std::optional<Price> stop_price{};
  TimeInForce time_in_force{TimeInForce::Day};
  ClientOrderId client_order_id{};
  std::string strategy{};  // attribution: which strategy originated this
};

// Server-authoritative view of an order's lifecycle.
struct Order {
  OrderId id{};
  OrderRequest request{};
  OrderStatus status{OrderStatus::New};
  Quantity filled_quantity{0};
  Price average_fill_price{0.0};
  Timestamp created_at{};
  Timestamp updated_at{};
  std::string reject_reason{};
};

// A single execution against an order.
struct Fill {
  OrderId order_id{};
  Symbol symbol{};
  Side side{Side::Buy};
  Quantity quantity{0};
  Price price{0.0};
  Timestamp ts{};
};

// Current net position in one instrument.
struct Position {
  Symbol symbol;
  Quantity quantity{0};
  Price average_price{0.0};
  Price last_price{0.0};

  bool is_flat() const { return quantity == 0; }

  Price market_value() const { return last_price * static_cast<Price>(quantity); }

  Price unrealized_pnl() const {
    return (last_price - average_price) * static_cast<Price>(quantity);
  }
};

}  // namespace apex
