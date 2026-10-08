#include "apex/engine/engine.hpp"

#include <algorithm>
#include <utility>

#include "apex/core/log.hpp"

namespace apex {

Engine::Engine(EngineConfig config, std::unique_ptr<IMarketDataFeed> feed,
               std::unique_ptr<IBrokerGateway> broker)
    : config_(std::move(config)),
      feed_(std::move(feed)),
      broker_(std::move(broker)) {}

Engine::~Engine() { stop(); }

void Engine::set_strategy(std::unique_ptr<IStrategy> strategy) {
  if (running_.load()) {
    APEX_LOG_WARN("engine", "cannot replace strategy while running");
    return;
  }
  strategy_ = std::move(strategy);
}

void Engine::configure_strategy(const StrategyParams& params) {
  if (strategy_) strategy_->configure(params);
}

bool Engine::start() {
  if (running_.load()) return true;
  if (!feed_ || !broker_) {
    APEX_LOG_ERROR("engine", "missing feed or broker gateway");
    return false;
  }

  if (!broker_->connect()) {
    APEX_LOG_ERROR("engine", "broker '{}' failed to connect",
                   std::string(broker_->name()));
    return false;
  }

  // Adapters push into the engine; the engine fans out to observers and then the
  // strategy. Publish-before-dispatch lets observers (e.g. a paper broker price
  // book) see the event before the strategy acts on it.
  feed_->set_quote_callback([this](const Quote& quote) { handle_quote(quote); });
  feed_->set_bar_callback([this](const Bar& bar) { handle_bar(bar); });
  broker_->set_order_update_callback(
      [this](const Order& order) { handle_order_update(order); });
  broker_->set_fill_callback([this](const Fill& fill) { handle_fill(fill); });

  if (config_.subscribe_on_start) {
    for (const auto& symbol : config_.symbols) feed_->subscribe(symbol);
  }

  if (strategy_) strategy_->on_start(*this);

  if (!feed_->start()) {
    APEX_LOG_ERROR("engine", "market data feed '{}' failed to start",
                   std::string(feed_->name()));
    if (strategy_) strategy_->on_stop(*this);
    broker_->disconnect();
    return false;
  }

  running_.store(true);
  APEX_LOG_INFO("engine", "started (feed={}, broker={}, strategy={})",
                std::string(feed_->name()), std::string(broker_->name()),
                strategy_ ? std::string(strategy_->name()) : std::string("<none>"));
  return true;
}

void Engine::stop() {
  if (!running_.load()) return;
  running_.store(false);
  if (feed_) feed_->stop();
  if (strategy_) strategy_->on_stop(*this);
  if (broker_) broker_->disconnect();
  APEX_LOG_INFO("engine", "stopped");
}

void Engine::handle_quote(const Quote& quote) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_quotes_[quote.symbol] = quote;
  }
  if (config_.publish_market_data) bus_.publish(QuoteEvent{quote});
  if (strategy_) strategy_->on_quote(quote, *this);
}

void Engine::handle_bar(const Bar& bar) {
  if (config_.publish_market_data) bus_.publish(BarEvent{bar});
  if (strategy_) strategy_->on_bar(bar, *this);
}

void Engine::handle_order_update(const Order& order) {
  bus_.publish(OrderUpdateEvent{order});
  if (strategy_) strategy_->on_order_update(order, *this);
}

void Engine::handle_fill(const Fill& fill) {
  bus_.publish(FillEvent{fill});
  if (strategy_) strategy_->on_fill(fill, *this);
}

OrderId Engine::submit_order(const OrderRequest& request) {
  if (!broker_) return {};
  OrderRequest routed = request;
  if (routed.strategy.empty() && strategy_) routed.strategy = strategy_->name();
  return broker_->submit_order(routed);
}

bool Engine::cancel_order(const OrderId& order_id) {
  return broker_ ? broker_->cancel_order(order_id) : false;
}

int Engine::cancel_all(const Symbol& symbol) {
  if (!broker_) return 0;
  int cancelled = 0;
  for (const auto& order : broker_->open_orders()) {
    if (order.request.symbol == symbol && broker_->cancel_order(order.id)) {
      ++cancelled;
    }
  }
  return cancelled;
}

std::optional<Position> Engine::position(const Symbol& symbol) const {
  return broker_ ? broker_->position(symbol) : std::nullopt;
}

std::vector<Position> Engine::positions() const {
  return broker_ ? broker_->positions() : std::vector<Position>{};
}

std::optional<Order> Engine::order(const OrderId& order_id) const {
  return broker_ ? broker_->get_order(order_id) : std::nullopt;
}

std::vector<Order> Engine::open_orders() const {
  return broker_ ? broker_->open_orders() : std::vector<Order>{};
}

std::optional<Quote> Engine::last_quote(const Symbol& symbol) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = last_quotes_.find(symbol);
  if (it == last_quotes_.end()) return std::nullopt;
  return it->second;
}

std::optional<Price> Engine::last_price(const Symbol& symbol) const {
  auto quote = last_quote(symbol);
  if (!quote) return std::nullopt;
  if (quote->last > 0.0) return quote->last;
  if (quote->mid() > 0.0) return quote->mid();
  return std::nullopt;
}

void Engine::log(LogLevel level, std::string_view message) const {
  Logger::instance().write(level, strategy_ ? strategy_->name() : "strategy",
                           message);
}

}  // namespace apex
