// Webull broker gateway: order entry and account state over Webull's REST API.
//
// The REST client (WebullRestClient) handles signing and transport; this file
// maps between apex domain types and Webull's order/position/account JSON.
// Field names are read defensively because Webull's response schemas are only
// partially documented in the public reference.

#include "apex/adapters/webull/webull_broker_gateway.hpp"

#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <utility>

#include "apex/core/log.hpp"
#include "util/json.hpp"

namespace apex {
namespace {

std::string to_webull_side(Side side) {
  return side == Side::Buy ? "BUY" : "SELL";
}

Side side_from_webull(std::string_view value) {
  return value == "SELL" ? Side::Sell : Side::Buy;
}

std::string to_webull_order_type(OrderType type) {
  switch (type) {
    case OrderType::Market:
      return "MARKET";
    case OrderType::Limit:
      return "LIMIT";
    case OrderType::Stop:
      return "STOP";
    case OrderType::StopLimit:
      return "STOP_LIMIT";
  }
  return "MARKET";
}

std::string to_webull_tif(TimeInForce tif) {
  switch (tif) {
    case TimeInForce::Day:
      return "DAY";
    case TimeInForce::GoodTillCancel:
      return "GTC";
    case TimeInForce::ImmediateOrCancel:
      return "IOC";
    case TimeInForce::FillOrKill:
      return "FOK";
  }
  return "DAY";
}

OrderStatus status_from_webull(std::string_view value) {
  if (value == "FILLED") return OrderStatus::Filled;
  if (value == "PARTIALLY_FILLED" || value == "PARTIAL_FILLED")
    return OrderStatus::PartiallyFilled;
  if (value == "CANCELLED" || value == "CANCELED") return OrderStatus::Cancelled;
  if (value == "REJECTED" || value == "FAILED") return OrderStatus::Rejected;
  if (value == "EXPIRED") return OrderStatus::Expired;
  if (value == "PENDING" || value == "SUBMITTED") return OrderStatus::Accepted;
  return OrderStatus::New;
}

// Webull returns monetary/quantity fields as strings; accept either.
double numeric(const json::Value* value) {
  if (value == nullptr) return 0.0;
  if (value->is_number()) return value->number_or(0.0);
  if (value->is_string()) {
    try {
      return std::stod(value->string_or("0"));
    } catch (...) {
      return 0.0;
    }
  }
  return 0.0;
}

Order order_from_json(const json::Value& node) {
  Order order;
  order.id = node.find_string("order_id");
  order.request.client_order_id = node.find_string("client_order_id");
  order.request.symbol = node.find_string("symbol");
  order.request.side = side_from_webull(node.find_string("side"));
  order.status = status_from_webull(node.find_string("status"));
  order.request.quantity =
      static_cast<Quantity>(numeric(node.find("total_quantity")));
  order.filled_quantity =
      static_cast<Quantity>(numeric(node.find("filled_quantity")));
  order.average_fill_price = numeric(node.find("avg_filled_price"));
  if (order.average_fill_price == 0.0)
    order.average_fill_price = numeric(node.find("filled_price"));
  order.reject_reason = node.find_string("reject_reason");
  return order;
}

Position position_from_json(const json::Value& node) {
  Position position;
  position.symbol = node.find_string("symbol");
  position.quantity = static_cast<Quantity>(numeric(node.find("quantity")));
  if (position.quantity == 0)
    position.quantity = static_cast<Quantity>(numeric(node.find("position")));
  position.average_price = numeric(node.find("average_cost"));
  position.last_price = numeric(node.find("last_price"));
  return position;
}

// The place-order response wrapper is not fully documented; look for the order
// id at the top level and under a "data" envelope.
const json::Value* unwrap(const json::Value& root) {
  if (const json::Value* data = root.find("data");
      data != nullptr && (data->is_object() || data->is_array())) {
    return data;
  }
  return &root;
}

// Orders/positions may be returned as a bare array or under {data:[...]}.
const json::Value* first_object(const json::Value& root) {
  const json::Value* node = unwrap(root);
  if (node->is_array() && !node->array().empty()) return &node->array().front();
  return node;
}

}  // namespace

WebullBrokerGateway::WebullBrokerGateway(WebullConfig config)
    : config_(std::move(config)) {
  config_.resolve_hosts();
  rest_ = std::make_unique<WebullRestClient>(config_);
}

WebullBrokerGateway::~WebullBrokerGateway() { disconnect(); }

void WebullBrokerGateway::set_order_update_callback(
    OrderUpdateCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  order_cb_ = std::move(callback);
}

void WebullBrokerGateway::set_fill_callback(FillCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  fill_cb_ = std::move(callback);
}

bool WebullBrokerGateway::connect() {
  if (connected_.load()) return true;
  if (!config_.has_app_credentials()) {
    APEX_LOG_ERROR("webull", "missing credentials (set {} / {})",
                   webull_env::kAppKey, webull_env::kAppSecret);
    return false;
  }

  // Resolve the account id if the caller did not supply one.
  if (config_.account_id.empty()) {
    WebullResponse response = rest_->list_accounts();
    if (!response.ok()) {
      APEX_LOG_ERROR("webull", "account list failed: status={} {}", response.status,
                     response.body);
      return false;
    }
    std::string error;
    json::Value root = json::Value::parse(response.body, error);
    const json::Value* account = first_object(root);
    config_.account_id = account->find_string("account_id");
    if (config_.account_id.empty()) {
      APEX_LOG_ERROR("webull", "no account_id in account list response");
      return false;
    }
  }

  connected_.store(true);
  APEX_LOG_INFO("webull", "connected: region={} sandbox={} account={}",
                config_.region, config_.sandbox, config_.account_id);
  return true;
}

void WebullBrokerGateway::disconnect() {
  polling_.store(false);
  if (poll_thread_.joinable()) poll_thread_.join();
  connected_.store(false);
}

OrderId WebullBrokerGateway::submit_order(const OrderRequest& request) {
  if (!connected_.load()) {
    APEX_LOG_ERROR("webull", "cannot submit order for {}: not connected",
                   request.symbol);
    return {};
  }

  const std::string client_order_id =
      request.client_order_id.empty()
          ? std::format("apex-{}-{}", ++sequence_,
                        std::chrono::system_clock::now().time_since_epoch().count())
          : request.client_order_id;

  json::Builder item;
  item.set("combo_type", "NORMAL")
      .set("client_order_id", client_order_id)
      .set("symbol", request.symbol)
      // apex's OrderRequest does not carry an asset class yet; equities are the
      // default. Options/futures/crypto would set instrument_type accordingly.
      .set("instrument_type", "EQUITY")
      .set("market", "US")
      .set("order_type", to_webull_order_type(request.type))
      .set("quantity", std::format("{}", request.quantity))
      .set("support_trading_session", "CORE")
      .set("side", to_webull_side(request.side))
      .set("time_in_force", to_webull_tif(request.time_in_force))
      .set("entrust_type", "QTY");
  if (request.limit_price) item.set("limit_price", std::format("{}", *request.limit_price));
  if (request.stop_price) item.set("stop_price", std::format("{}", *request.stop_price));

  json::Builder body;
  body.set("account_id", config_.account_id)
      .set_raw("new_orders", "[" + item.str() + "]");

  WebullResponse response = rest_->place_order(body.str());
  if (!response.ok()) {
    APEX_LOG_ERROR("webull", "place_order failed: status={} {}", response.status,
                   response.body);
    return {};
  }

  std::string error;
  json::Value root = json::Value::parse(response.body, error);
  const json::Value* node = first_object(root);

  Order order;
  order.id = node->find_string("order_id");
  order.request = request;
  order.request.client_order_id = client_order_id;
  order.status = status_from_webull(node->find_string("status"));
  if (order.status == OrderStatus::New) order.status = OrderStatus::Accepted;
  order.created_at = now();
  order.updated_at = order.created_at;

  OrderUpdateCallback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    orders_[client_order_id] = order;
    callback = order_cb_;
  }
  if (callback) callback(order);
  APEX_LOG_INFO("webull", "placed {} {} {} (client_order_id={})", order.id,
                to_webull_side(request.side), request.symbol, client_order_id);
  return order.id.empty() ? client_order_id : order.id;
}

bool WebullBrokerGateway::cancel_order(const OrderId& order_id) {
  if (!connected_.load()) return false;

  // WebullCancels by client_order_id; the REST client signs the compact body.
  WebullResponse response = rest_->cancel_order(order_id);
  if (!response.ok()) {
    APEX_LOG_WARN("webull", "cancel_order failed: status={} {}", response.status,
                  response.body);
    return false;
  }

  OrderUpdateCallback callback;
  Order updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(order_id);
    if (it != orders_.end()) {
      it->second.status = OrderStatus::Cancelled;
      it->second.updated_at = now();
      updated = it->second;
    }
    callback = order_cb_;
  }
  if (callback && !updated.id.empty()) callback(updated);
  return true;
}

std::optional<Order> WebullBrokerGateway::get_order(
    const OrderId& order_id) const {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(order_id);
    if (it != orders_.end()) return it->second;
  }
  const std::string body = query_order_detail(order_id);
  if (body.empty()) return std::nullopt;
  std::string error;
  json::Value root = json::Value::parse(body, error);
  return order_from_json(*first_object(root));
}

std::vector<Order> WebullBrokerGateway::open_orders() const {
  std::vector<Order> result;
  if (!connected_.load()) return result;

  WebullResponse response = rest_->open_orders(config_.account_id);
  if (!response.ok()) {
    APEX_LOG_WARN("webull", "open_orders failed: status={}", response.status);
    return result;
  }
  std::string error;
  json::Value root = json::Value::parse(response.body, error);
  const json::Value* node = unwrap(root);
  if (node->is_array()) {
    for (const auto& item : node->array()) result.push_back(order_from_json(item));
  } else if (const json::Value* list = node->find("data");
             list != nullptr && list->is_array()) {
    for (const auto& item : list->array()) result.push_back(order_from_json(item));
  }
  return result;
}

std::vector<Position> WebullBrokerGateway::positions() const {
  std::vector<Position> result;
  if (!connected_.load()) return result;

  WebullResponse response = rest_->account_positions(config_.account_id);
  if (!response.ok()) {
    APEX_LOG_WARN("webull", "positions failed: status={}", response.status);
    return result;
  }
  std::string error;
  json::Value root = json::Value::parse(response.body, error);
  const json::Value* node = unwrap(root);
  if (node->is_array()) {
    for (const auto& item : node->array())
      result.push_back(position_from_json(item));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  positions_ = result;
  return result;
}

std::optional<Position> WebullBrokerGateway::position(
    const Symbol& symbol) const {
  for (const auto& item : positions()) {
    if (item.symbol == symbol) return item;
  }
  return std::nullopt;
}

std::string WebullBrokerGateway::query_order_detail(
    const std::string& client_order_id) const {
  WebullResponse response =
      rest_->order_detail(config_.account_id, client_order_id);
  if (!response.ok()) return {};
  return response.body;
}

void WebullBrokerGateway::poll_order_updates() {
  if (!connected_.load()) return;

  std::vector<std::string> watched;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, order] : orders_) {
      if (order.status == OrderStatus::Accepted ||
          order.status == OrderStatus::PartiallyFilled ||
          order.status == OrderStatus::New) {
        watched.push_back(id);
      }
    }
  }

  for (const auto& id : watched) {
    const std::string body = query_order_detail(id);
    if (body.empty()) continue;
    std::string error;
    json::Value root = json::Value::parse(body, error);
    Order fresh = order_from_json(*first_object(root));
    if (fresh.status == OrderStatus::New) continue;

    OrderUpdateCallback order_cb;
    FillCallback fill_cb;
    Order previous;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = orders_.find(id);
      if (it == orders_.end()) continue;
      previous = it->second;
      if (previous.status == fresh.status &&
          previous.filled_quantity == fresh.filled_quantity) {
        continue;
      }
      const Quantity newly_filled = fresh.filled_quantity - previous.filled_quantity;
      fresh.request = previous.request;
      it->second = fresh;
      order_cb = order_cb_;
      fill_cb = fill_cb_;
      if (newly_filled > 0 && fill_cb) {
        Fill fill;
        fill.order_id = fresh.id;
        fill.symbol = fresh.request.symbol;
        fill.side = fresh.request.side;
        fill.quantity = newly_filled;
        fill.price = fresh.average_fill_price;
        fill.ts = now();
        if (order_cb) order_cb(fresh);
        fill_cb(fill);
        continue;
      }
    }
    if (order_cb) order_cb(fresh);
  }
}

}  // namespace apex
