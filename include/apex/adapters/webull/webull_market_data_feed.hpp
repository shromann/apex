#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "apex/adapters/webull/webull_config.hpp"
#include "apex/adapters/webull/webull_rest_client.hpp"
#include "apex/market/market_data_feed.hpp"

namespace apex::net {
class MqttClient;
}

namespace apex {

enum class WebullStreamMode {
  Rest,  // poll snapshots/bars on a timer (no MQTT)
  Mqtt,  // subscribe to the real-time stream (falls back to Rest on failure)
};

struct WebullFeedOptions {
  WebullStreamMode mode{WebullStreamMode::Mqtt};
  std::string category{"US_STOCK"};      // Webull instrument category
  std::string timespan{"M1"};            // bar granularity
  std::chrono::milliseconds poll_interval{1000};
  std::chrono::milliseconds bar_interval{60000};
  bool emit_bars{true};
};

// Market data adapter. Real-time quotes arrive over Webull's MQTT stream
// (streaming on 1883/TLS); a REST poller provides snapshots and historical bars
// and serves as a fallback when MQTT is unavailable.
class WebullMarketDataFeed final : public IMarketDataFeed {
 public:
  explicit WebullMarketDataFeed(WebullConfig config,
                                WebullFeedOptions options = {});
  ~WebullMarketDataFeed() override;

  std::string_view name() const override { return "webull-feed"; }

  void set_quote_callback(QuoteCallback callback) override;
  void set_bar_callback(BarCallback callback) override;
  bool subscribe(const Symbol& symbol) override;
  void unsubscribe(const Symbol& symbol) override;
  bool start() override;
  void stop() override;
  bool is_running() const override { return running_.load(); }
  bool is_connected() const override { return connected_.load(); }

  const WebullConfig& config() const { return config_; }
  WebullRestClient& rest() { return *rest_; }

 private:
  void rest_loop();
  void mqtt_loop();
  void emit_snapshots();
  void emit_bars();
  void deliver_quote(const Quote& quote);
  void deliver_bar(const Bar& bar);

  WebullConfig config_;
  WebullFeedOptions options_;
  std::unique_ptr<WebullRestClient> rest_;
  std::unique_ptr<net::MqttClient> mqtt_;

  mutable std::mutex mutex_;
  QuoteCallback quote_cb_;
  BarCallback bar_cb_;
  std::unordered_set<Symbol> subscriptions_;
  std::unordered_map<Symbol, std::chrono::steady_clock::time_point> last_bar_;

  std::thread rest_thread_;
  std::thread mqtt_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> mqtt_active_{false};
};

}  // namespace apex
