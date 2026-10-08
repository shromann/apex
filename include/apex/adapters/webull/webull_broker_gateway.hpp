#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "apex/adapters/webull/webull_config.hpp"
#include "apex/adapters/webull/webull_rest_client.hpp"
#include "apex/broker/broker_gateway.hpp"

namespace apex {

// Order/account adapter for Webull's REST API, backed by WebullRestClient.
// connect() validates credentials and resolves the account. Trading methods
// translate apex order types to Webull's unified order schema; order-status
// changes arrive either from the gRPC trade-events stream or from REST polling
// (see poll_order_updates), both funnelling into the same callbacks.
class WebullBrokerGateway final : public IBrokerGateway {
 public:
  explicit WebullBrokerGateway(WebullConfig config);
  ~WebullBrokerGateway() override;

  std::string_view name() const override { return "webull-broker"; }

  void set_order_update_callback(OrderUpdateCallback callback) override;
  void set_fill_callback(FillCallback callback) override;

  bool connect() override;
  void disconnect() override;
  bool is_connected() const override { return connected_.load(); }

  OrderId submit_order(const OrderRequest& request) override;
  bool cancel_order(const OrderId& order_id) override;

  std::optional<Order> get_order(const OrderId& order_id) const override;
  std::vector<Order> open_orders() const override;

  std::vector<Position> positions() const override;
  std::optional<Position> position(const Symbol& symbol) const override;

  const WebullConfig& config() const { return config_; }
  WebullRestClient& rest() { return *rest_; }

  // REST-syncs the given client_order_ids and fires the order-update callback
  // for any that changed. Called by the engine's poll loop when gRPC is absent.
  void poll_order_updates();

 private:
  std::string query_order_detail(const std::string& client_order_id) const;

  WebullConfig config_;
  std::unique_ptr<WebullRestClient> rest_;
  mutable std::mutex mutex_;
  OrderUpdateCallback order_cb_;
  FillCallback fill_cb_;
  std::unordered_map<OrderId, Order> orders_;
  mutable std::vector<Position> positions_;
  std::thread poll_thread_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> polling_{false};
  std::atomic<std::uint64_t> sequence_{0};
};

}  // namespace apex
