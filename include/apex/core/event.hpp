#pragma once

#include <variant>

#include "apex/core/market_data.hpp"
#include "apex/core/order.hpp"

namespace apex {

// Payloads broadcast on the EventBus. The engine publishes these as data flows
// in from the feed and broker, so any number of observers (risk, logging, a
// paper broker price book, telemetry) can react without the engine knowing
// about them.

struct QuoteEvent {
  Quote quote;
};

struct BarEvent {
  Bar bar;
};

struct OrderUpdateEvent {
  Order order;
};

struct FillEvent {
  Fill fill;
};

struct PositionEvent {
  Position position;
};

using Event =
    std::variant<QuoteEvent, BarEvent, OrderUpdateEvent, FillEvent, PositionEvent>;

}  // namespace apex
