#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "apex/adapters/webull/webull_config.hpp"
#include "apex/adapters/webull/webull_signature.hpp"

namespace apex {

// Webull REST API paths, from the Open Web API reference. Paths marked
// "derived" follow the documented /trading/orders/* convention but could not be
// confirmed on their reference page; every path is overridable via
// WebullRestClient::set_path_override() so a change needs no recompile.
namespace webull_path {
// Authentication.
inline constexpr const char* kTokenCreate = "/auth/tokens/create";
inline constexpr const char* kTokenCheck = "/auth/tokens/check";
inline constexpr const char* kTokenRefresh = "/auth/tokens/refresh";

// Accounts & assets.
inline constexpr const char* kAccountList = "/trading/accounts/list";
inline constexpr const char* kAccountBalance = "/trading/assets/balances/get";
inline constexpr const char* kAccountPositions = "/trading/assets/positions/list";

// Orders (query endpoints confirmed; mutations derived from the same family).
inline constexpr const char* kOrderPlace = "/trading/orders/place";      // derived
inline constexpr const char* kOrderReplace = "/trading/orders/replace";  // derived
inline constexpr const char* kOrderCancel = "/trading/orders/cancel";    // derived
inline constexpr const char* kOrderDetail = "/trading/orders/get";
inline constexpr const char* kOrderOpen = "/trading/orders/open-orders/list";
inline constexpr const char* kOrderHistory = "/trading/orders/historical-orders/list";

// Market data.
inline constexpr const char* kStockSnapshots = "/market-data/stocks/snapshots/list";
inline constexpr const char* kStockBars = "/market-data/stocks/bars/get";
inline constexpr const char* kStockDepths = "/market-data/stocks/depths/list";

// Streaming control over HTTP (derived; the MQTT data path is separate).
inline constexpr const char* kStreamSubscribe = "/market-data/streaming/subscribe";      // derived
inline constexpr const char* kStreamUnsubscribe = "/market-data/streaming/unsubscribe";  // derived
}  // namespace webull_path

// A raw REST response. `body` is the parsed JSON text; callers use apex::json
// to map it.
struct WebullResponse {
  int status{0};
  std::string body;
  std::string error;  // transport/parse failure message, empty on success

  bool ok() const { return status >= 200 && status < 300; }
  bool transport_ok() const { return error.empty(); }
};

// Signed REST client: builds the signature headers, sends the request over
// HTTPS, and returns the raw response. One instance is shared by the broker
// gateway and the market-data feed.
class WebullRestClient {
 public:
  explicit WebullRestClient(WebullConfig config);

  // Applies the access token required for 2FA-enabled applications.
  void set_access_token(std::string token);
  const std::string& access_token() const { return config_.access_token; }
  const WebullConfig& config() const { return config_; }

  // Overrides an endpoint path at runtime (see webull_path).
  void set_path_override(std::string key, std::string path);

  // --- Generic verbs ---------------------------------------------------------
  WebullResponse get(const std::string& path, const webull::ParamList& query = {});
  WebullResponse post(const std::string& path,
                      const webull::ParamList& query,
                      std::string body);

  // --- Authentication --------------------------------------------------------
  // Creates a token (2FA apps). The token starts Pending Verification and must
  // be approved in the Webull app.
  WebullResponse create_token();
  WebullResponse check_token();
  WebullResponse refresh_token();

  // --- Accounts & assets -----------------------------------------------------
  WebullResponse list_accounts();
  WebullResponse account_balance(const std::string& account_id);
  WebullResponse account_positions(const std::string& account_id);

  // --- Orders ----------------------------------------------------------------
  WebullResponse place_order(const std::string& order_json);
  WebullResponse replace_order(const std::string& order_json);
  WebullResponse cancel_order(const std::string& client_order_id);
  WebullResponse order_detail(const std::string& account_id,
                              const std::string& client_order_id);
  WebullResponse open_orders(const std::string& account_id,
                             int page_size = 50);
  WebullResponse order_history(const std::string& account_id,
                               std::string start_date = {},
                               std::string end_date = {}, int page_size = 50);

  // --- Market data -----------------------------------------------------------
  WebullResponse stock_snapshots(const std::vector<std::string>& symbols,
                                 const std::string& category,
                                 const std::string& sub_category = {});
  WebullResponse stock_bars(const std::string& symbol,
                            const std::string& category,
                            const std::string& timespan, int count,
                            const std::string& sub_category = {});
  WebullResponse stock_depths(const std::string& symbol,
                              const std::string& category, int depth = 1);

  // --- Streaming control (HTTP side of MQTT) --------------------------------
  WebullResponse stream_subscribe(const std::vector<std::string>& topics,
                                  const std::string& session_id);
  WebullResponse stream_unsubscribe(const std::vector<std::string>& topics,
                                    const std::string& session_id);

  // --- Trade events (gRPC) ---------------------------------------------------
  // Host targeted by the gRPC trade-events stream (see net::GrpcClient).
  std::string events_host() const { return config_.events_host; }

 private:
  std::string resolve_path(const std::string& key_or_path) const;

  WebullConfig config_;
  std::vector<std::pair<std::string, std::string>> path_overrides_;
};

}  // namespace apex
