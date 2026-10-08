#pragma once

#include <optional>
#include <string>
#include <vector>

#include "apex/core/log.hpp"
#include "apex/core/market_data.hpp"
#include "apex/core/order.hpp"

namespace apex {

// The only surface a strategy may use to touch the outside world. The engine
// implements it; strategies never see Webull, threads, or sockets. This is what
// makes strategies portable and trivially testable with a fake context.
class IStrategyContext {
 public:
  virtual ~IStrategyContext() = default;

  // --- Trading -------------------------------------------------------------
  virtual OrderId submit_order(const OrderRequest& request) = 0;
  virtual bool cancel_order(const OrderId& order_id) = 0;

  // Cancel every working order for a symbol; returns how many were cancelled.
  virtual int cancel_all(const Symbol& symbol) = 0;

  // Convenience helpers built on submit_order.
  OrderId market_buy(const Symbol& symbol, Quantity quantity,
                     ClientOrderId client_order_id = {}) {
    return submit_order(make_request(symbol, Side::Buy, OrderType::Market,
                                     quantity, std::nullopt, std::nullopt,
                                     std::move(client_order_id)));
  }
  OrderId market_sell(const Symbol& symbol, Quantity quantity,
                      ClientOrderId client_order_id = {}) {
    return submit_order(make_request(symbol, Side::Sell, OrderType::Market,
                                     quantity, std::nullopt, std::nullopt,
                                     std::move(client_order_id)));
  }
  OrderId limit_buy(const Symbol& symbol, Quantity quantity, Price limit,
                    ClientOrderId client_order_id = {}) {
    return submit_order(make_request(symbol, Side::Buy, OrderType::Limit,
                                     quantity, limit, std::nullopt,
                                     std::move(client_order_id)));
  }
  OrderId limit_sell(const Symbol& symbol, Quantity quantity, Price limit,
                     ClientOrderId client_order_id = {}) {
    return submit_order(make_request(symbol, Side::Sell, OrderType::Limit,
                                     quantity, limit, std::nullopt,
                                     std::move(client_order_id)));
  }

  // --- Read-only state -----------------------------------------------------
  virtual std::optional<Position> position(const Symbol& symbol) const = 0;
  virtual std::vector<Position> positions() const = 0;
  virtual std::optional<Order> order(const OrderId& order_id) const = 0;
  virtual std::vector<Order> open_orders() const = 0;
  virtual std::optional<Quote> last_quote(const Symbol& symbol) const = 0;
  virtual std::optional<Price> last_price(const Symbol& symbol) const = 0;

  // --- Meta ----------------------------------------------------------------
  virtual Timestamp time() const = 0;
  virtual void log(LogLevel level, std::string_view message) const = 0;

 protected:
  static OrderRequest make_request(const Symbol& symbol, Side side,
                                   OrderType type, Quantity quantity,
                                   std::optional<Price> limit,
                                   std::optional<Price> stop,
                                   ClientOrderId client_order_id) {
    OrderRequest request;
    request.symbol = symbol;
    request.side = side;
    request.type = type;
    request.quantity = quantity;
    request.limit_price = limit;
    request.stop_price = stop;
    request.client_order_id = std::move(client_order_id);
    return request;
  }
};

}  // namespace apex
