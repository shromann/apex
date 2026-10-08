#include <memory>
#include <string_view>
#include <vector>

#include "apex/adapters/mock/mock_broker_gateway.hpp"
#include "apex/adapters/mock/mock_market_data_feed.hpp"
#include "apex/engine/engine.hpp"
#include "test_common.hpp"

namespace {

using namespace apex;

// Buys `quantity` shares on the first bar, then goes quiet. Used to prove that
// quotes reach the strategy, orders reach the broker, and fills come back.
class BuyOnceStrategy final : public IStrategy {
 public:
  std::string_view name() const override { return "buy_once"; }

  void configure(const StrategyParams& params) override {
    quantity_ = static_cast<Quantity>(params.number("quantity", 100.0));
  }

  void on_start(IStrategyContext&) override { ++starts_; }
  void on_quote(const Quote&, IStrategyContext&) override { ++quotes_; }
  void on_bar(const Bar& bar, IStrategyContext& ctx) override {
    ++bars_;
    if (!ordered_) {
      ordered_ = true;
      ctx.market_buy(bar.symbol, quantity_, "buy_once");
    }
  }
  void on_fill(const Fill&, IStrategyContext&) override { ++fills_; }

  int starts_{0};
  int quotes_{0};
  int bars_{0};
  int fills_{0};
  Quantity quantity_{100};
  bool ordered_{false};
};

}  // namespace

int main() {
  using namespace apex;

  MockFeedConfig feed_config;
  feed_config.background = false;  // driven deterministically by the test
  feed_config.bar_every = 1;
  feed_config.seed = 7;
  feed_config.start_price = 100.0;

  auto feed = std::make_unique<MockMarketDataFeed>(feed_config);
  MockMarketDataFeed* feed_ptr = feed.get();

  auto broker = std::make_unique<MockBrokerGateway>(MockBrokerConfig{});
  MockBrokerGateway* broker_ptr = broker.get();

  EngineConfig engine_config;
  engine_config.symbols = {"AAPL"};
  Engine engine(engine_config, std::move(feed), std::move(broker));

  auto strategy = std::make_unique<BuyOnceStrategy>();
  BuyOnceStrategy* strategy_ptr = strategy.get();
  engine.set_strategy(std::move(strategy));

  StrategyParams params;
  params.set_symbols({"AAPL"});
  params.set("quantity", 100.0);
  engine.configure_strategy(params);

  // Paper broker tracks the prices the engine publishes.
  engine.bus().subscribe<QuoteEvent>([broker_ptr](const QuoteEvent& event) {
    Price price = event.quote.last > 0.0 ? event.quote.last : event.quote.mid();
    if (price > 0.0) broker_ptr->update_price(event.quote.symbol, price);
  });

  CHECK(engine.start());
  CHECK(engine.is_running());
  CHECK(strategy_ptr->starts_ == 1);

  for (int i = 0; i < 5; ++i) feed_ptr->emit_tick();

  CHECK(strategy_ptr->quotes_ == 5);
  CHECK(strategy_ptr->bars_ == 5);
  CHECK(strategy_ptr->fills_ == 1);

  auto position = engine.position("AAPL");
  CHECK(position.has_value());
  CHECK(position->quantity == 100);

  auto price = engine.last_price("AAPL");
  CHECK(price.has_value());
  CHECK(price.value() > 0.0);

  engine.stop();
  CHECK(!engine.is_running());

  return apextest::summary("engine");
}
