#pragma once

#include "apex/core/types.hpp"

namespace apex {

// Top-of-book snapshot plus last trade.
struct Quote {
  Symbol symbol;
  Price bid{0.0};
  Price ask{0.0};
  Price last{0.0};
  Quantity bid_size{0};
  Quantity ask_size{0};
  Quantity last_size{0};
  Timestamp ts{};

  double mid() const {
    if (bid > 0.0 && ask > 0.0) return (bid + ask) / 2.0;
    return last;
  }
};

// Aggregated OHLCV candle for a single instrument.
struct Bar {
  Symbol symbol;
  Timestamp ts{};
  Price open{0.0};
  Price high{0.0};
  Price low{0.0};
  Price close{0.0};
  Quantity volume{0};
};

}  // namespace apex
