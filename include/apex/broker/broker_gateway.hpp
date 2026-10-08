#pragma once

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "apex/core/order.hpp"

namespace apex {

using OrderUpdateCallback = std::function<void(const Order&)>;
using FillCallback = std::function<void(const Fill&)>;

// Port: an execution venue. The engine depends only on this interface, so the
// live Webull gateway and a paper-trading simulator are interchangeable.
class IBrokerGateway {
 public:
  virtual ~IBrokerGateway() = default;

  virtual std::string_view name() const = 0;

  virtual void set_order_update_callback(OrderUpdateCallback callback) = 0;
  virtual void set_fill_callback(FillCallback callback) = 0;

  virtual bool connect() = 0;
  virtual void disconnect() = 0;
  virtual bool is_connected() const = 0;

  // Returns the gateway-assigned order id (empty on immediate rejection).
  virtual OrderId submit_order(const OrderRequest& request) = 0;
  virtual bool cancel_order(const OrderId& order_id) = 0;

  virtual std::optional<Order> get_order(const OrderId& order_id) const = 0;
  virtual std::vector<Order> open_orders() const = 0;

  virtual std::vector<Position> positions() const = 0;
  virtual std::optional<Position> position(const Symbol& symbol) const = 0;
};

}  // namespace apex
