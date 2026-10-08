#pragma once

#include <functional>
#include <string_view>

#include "apex/core/market_data.hpp"

namespace apex {

using QuoteCallback = std::function<void(const Quote&)>;
using BarCallback = std::function<void(const Bar&)>;

// Port: a source of market data. The engine depends only on this interface, so
// Webull, a simulator, or a backtest replay are interchangeable adapters.
class IMarketDataFeed {
 public:
  virtual ~IMarketDataFeed() = default;

  virtual std::string_view name() const = 0;

  virtual void set_quote_callback(QuoteCallback callback) = 0;
  virtual void set_bar_callback(BarCallback callback) = 0;

  // Begin streaming the given instrument. Returns false if unsupported.
  virtual bool subscribe(const Symbol& symbol) = 0;
  virtual void unsubscribe(const Symbol& symbol) = 0;

  virtual bool start() = 0;
  virtual void stop() = 0;
  virtual bool is_running() const = 0;
  virtual bool is_connected() const = 0;
};

}  // namespace apex
