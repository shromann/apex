// Webull market data feed.
//
// Two data paths:
//   * REST poller  -> stock snapshots (quotes) and historical bars on a timer.
//   * MQTT stream  -> real-time quotes when options.mode == Mqtt.
// If the MQTT stream cannot be established the REST poller alone still serves
// the feed, so the engine always gets data.
//
// NOTE: the exact MQTT topic layout and streaming credentials are not fully
// specified in the public reference; the topic builder below is centralised and
// marked so it can be corrected in one place. REST paths are from the reference.

#include "apex/adapters/webull/webull_market_data_feed.hpp"

#include <chrono>
#include <format>
#include <utility>

#include "apex/core/log.hpp"
#include "net/mqtt_client.hpp"
#include "util/json.hpp"

namespace apex {
namespace {

double value_of(const json::Value* node, const char* first, const char* second,
                const char* third = nullptr) {
  const char* keys[] = {first, second, third};
  for (const char* key : keys) {
    if (key == nullptr) continue;
    if (const json::Value* value = node->find(key)) {
      if (value->is_number()) return value->number_or(0.0);
      if (value->is_string()) {
        try {
          return std::stod(value->string_or("0"));
        } catch (...) {
          return 0.0;
        }
      }
    }
  }
  return 0.0;
}

Quote quote_from_snapshot(const json::Value& node, const Symbol& fallback) {
  Quote quote;
  quote.symbol = node.find_string("symbol", fallback);
  // "price" is Webull's latest price; tolerate a few aliases.
  quote.last = value_of(&node, "price", "last_price", "close");
  quote.bid = value_of(&node, "bid", "bid_price");
  quote.ask = value_of(&node, "ask", "ask_price");
  quote.last_size = static_cast<Quantity>(value_of(&node, "volume", "last_volume"));
  quote.ts = apex::now();
  return quote;
}

Bar bar_from_json(const json::Value& node, const Symbol& fallback,
                  Price previous_close) {
  Bar bar;
  bar.symbol = node.find_string("symbol", fallback);
  bar.open = value_of(&node, "open", "open_price");
  bar.high = value_of(&node, "high", "high_price");
  bar.low = value_of(&node, "low", "low_price");
  bar.close = value_of(&node, "close", "close_price");
  bar.volume = static_cast<Quantity>(value_of(&node, "volume", "total_volume"));
  if (bar.open == 0.0 && previous_close > 0.0) bar.open = previous_close;
  bar.ts = apex::now();
  return bar;
}

// Centralised topic construction so the layout can be corrected in one place.
// Webull's streaming topics are not fully documented in the public reference.
std::string stream_topic(const std::string& category, const std::string& symbol) {
  return std::format("{}.{}.quote", category, symbol);
}

}  // namespace

WebullMarketDataFeed::WebullMarketDataFeed(WebullConfig config,
                                          WebullFeedOptions options)
    : config_(std::move(config)), options_(options) {
  config_.resolve_hosts();
  rest_ = std::make_unique<WebullRestClient>(config_);
  mqtt_ = std::make_unique<net::MqttClient>();
}

WebullMarketDataFeed::~WebullMarketDataFeed() { stop(); }

void WebullMarketDataFeed::set_quote_callback(QuoteCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  quote_cb_ = std::move(callback);
}

void WebullMarketDataFeed::set_bar_callback(BarCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  bar_cb_ = std::move(callback);
}

bool WebullMarketDataFeed::subscribe(const Symbol& symbol) {
  std::lock_guard<std::mutex> lock(mutex_);
  subscriptions_.insert(symbol);
  return true;
}

void WebullMarketDataFeed::unsubscribe(const Symbol& symbol) {
  std::lock_guard<std::mutex> lock(mutex_);
  subscriptions_.erase(symbol);
}

bool WebullMarketDataFeed::start() {
  if (running_.load()) return false;
  if (!config_.has_app_credentials()) {
    APEX_LOG_ERROR("webull", "cannot start feed: {} / {} not set",
                   webull_env::kAppKey, webull_env::kAppSecret);
    return false;
  }

  running_.store(true);
  rest_thread_ = std::thread(&WebullMarketDataFeed::rest_loop, this);
  if (options_.mode == WebullStreamMode::Mqtt) {
    mqtt_thread_ = std::thread(&WebullMarketDataFeed::mqtt_loop, this);
  }
  connected_.store(true);
  APEX_LOG_INFO("webull", "feed started: host={} mode={} poll={}ms",
                config_.api_host,
                options_.mode == WebullStreamMode::Mqtt ? "mqtt" : "rest",
                options_.poll_interval.count());
  return true;
}

void WebullMarketDataFeed::stop() {
  if (!running_.exchange(false)) return;
  if (mqtt_) mqtt_->disconnect();
  if (rest_thread_.joinable()) rest_thread_.join();
  if (mqtt_thread_.joinable()) mqtt_thread_.join();
  connected_.store(false);
  mqtt_active_.store(false);
  APEX_LOG_INFO("webull", "feed stopped");
}

void WebullMarketDataFeed::rest_loop() {
  while (running_.load()) {
    emit_snapshots();
    if (options_.emit_bars) emit_bars();
    std::this_thread::sleep_for(options_.poll_interval);
  }
}

void WebullMarketDataFeed::mqtt_loop() {
  std::vector<Symbol> symbols;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    symbols.assign(subscriptions_.begin(), subscriptions_.end());
  }

  std::string error;
  // Webull uses the session id as the MQTT client id (same id sent to the HTTP
  // subscribe endpoint). Credentials for the broker are not fully documented;
  // the access token is passed as the username when present.
  if (!mqtt_->connect(config_.mqtt_host, config_.mqtt_port,
                      config_.mqtt_session_id, config_.access_token, "",
                      60, config_.http_timeout_ms, error)) {
    APEX_LOG_WARN("webull",
                  "MQTT stream unavailable ({}); falling back to REST polling",
                  error);
    return;
  }

  std::vector<std::string> topics;
  for (const auto& symbol : symbols) {
    topics.push_back(stream_topic(options_.category, symbol));
  }
  if (!mqtt_->subscribe(topics, error)) {
    APEX_LOG_WARN("webull", "MQTT subscribe failed ({}); using REST polling",
                  error);
    mqtt_->disconnect();
    return;
  }

  // Register the session with the HTTP streaming endpoint as well.
  rest_->stream_subscribe(topics, config_.mqtt_session_id);

  mqtt_active_.store(true);
  APEX_LOG_INFO("webull", "MQTT streaming active ({} topics)", topics.size());

  mqtt_->set_message_handler([this](const std::string& topic,
                                    const std::string& payload) {
    std::string parse_error;
    json::Value root = json::Value::parse(payload, parse_error);
    Symbol fallback = topic;
    if (auto dot = topic.find_last_of('.'); dot != std::string::npos) {
      // topic shape <category>.<symbol>.quote -> symbol
      auto first = topic.find('.');
      if (first != std::string::npos && first + 1 < dot)
        fallback = topic.substr(first + 1, dot - first - 1);
    }
    Quote quote = quote_from_snapshot(root, fallback);
    deliver_quote(quote);
  });

  while (running_.load()) {
    std::string poll_error;
    if (!mqtt_->poll(1000, poll_error)) {
      APEX_LOG_WARN("webull", "MQTT stream ended ({})", poll_error);
      break;
    }
  }
  mqtt_active_.store(false);
  mqtt_->disconnect();
}

void WebullMarketDataFeed::emit_snapshots() {
  std::vector<Symbol> symbols;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    symbols.assign(subscriptions_.begin(), subscriptions_.end());
  }
  if (symbols.empty()) return;

  WebullResponse response =
      rest_->stock_snapshots(symbols, options_.category);
  if (!response.ok()) {
    APEX_LOG_DEBUG("webull", "snapshot poll failed: status={}", response.status);
    return;
  }

  std::string error;
  json::Value root = json::Value::parse(response.body, error);
  const json::Value* node = root.find("data");
  const json::Value* list = (node != nullptr && node->is_array()) ? node : &root;
  if (!list->is_array()) return;

  for (const auto& item : list->array()) deliver_quote(quote_from_snapshot(item, {}));
}

void WebullMarketDataFeed::emit_bars() {
  std::vector<Symbol> symbols;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    symbols.assign(subscriptions_.begin(), subscriptions_.end());
  }

  const auto now_time = std::chrono::steady_clock::now();
  for (const auto& symbol : symbols) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = last_bar_.find(symbol);
      if (it != last_bar_.end() &&
          now_time - it->second < options_.bar_interval) {
        continue;
      }
      last_bar_[symbol] = now_time;
    }

    WebullResponse response = rest_->stock_bars(
        symbol, options_.category, options_.timespan, 2);
    if (!response.ok()) {
      APEX_LOG_DEBUG("webull", "bars poll failed for {}: status={}", symbol,
                     response.status);
      continue;
    }

    std::string error;
    json::Value root = json::Value::parse(response.body, error);
    const json::Value* node = root.find("data");
    const json::Value* list = (node != nullptr && node->is_array()) ? node : &root;
    if (!list->is_array() || list->array().empty()) continue;
    deliver_bar(bar_from_json(list->array().front(), symbol, 0.0));
  }
}

void WebullMarketDataFeed::deliver_quote(const Quote& quote) {
  QuoteCallback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    callback = quote_cb_;
  }
  if (callback) callback(quote);
}

void WebullMarketDataFeed::deliver_bar(const Bar& bar) {
  BarCallback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    callback = bar_cb_;
  }
  if (callback) callback(bar);
}

}  // namespace apex
