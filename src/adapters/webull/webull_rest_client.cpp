#include "apex/adapters/webull/webull_rest_client.hpp"

#include <string>
#include <utility>
#include <vector>

#include "apex/adapters/webull/webull_crypto.hpp"
#include "apex/core/log.hpp"
#include "net/http_client.hpp"
#include "util/json.hpp"

namespace apex {
namespace {

// Signs and sends one request. `body` is hashed verbatim for the signature and
// sent unchanged. Query values are kept RAW for signing; the HTTP client
// percent-encodes them only when building the wire target.
WebullResponse send_signed(const WebullConfig& config,
                           const std::string& path,
                           const webull::ParamList& query, std::string body,
                           const std::string& method) {
  WebullResponse result;
  if (!config.has_app_credentials()) {
    result.error =
        "missing app_key/app_secret (set WEBULL_APP_KEY and WEBULL_APP_SECRET)";
    return result;
  }

  webull::SignatureInput input;
  input.path = path;
  input.query = query;
  input.body = body;
  input.app_key = config.app_key;
  input.app_secret = config.app_secret;
  input.host = config.api_host;
  input.timestamp = webull::iso8601_timestamp();
  input.nonce = webull::generate_nonce();

  const std::string signature = webull::sign(input);

  net::HttpRequest request;
  request.method = method;
  request.host = config.api_host;
  request.path = path;
  request.query = query;
  request.port = 443;
  request.tls = true;
  request.timeout_ms = config.http_timeout_ms;
  request.body = body;

  auto add_header = [&request](std::string name, std::string value) {
    request.headers.push_back(
        net::HttpHeader{std::move(name), std::move(value)});
  };
  add_header("x-app-key", config.app_key);
  add_header("x-timestamp", input.timestamp);
  add_header("x-signature", signature);
  add_header("x-signature-algorithm", input.algorithm);
  add_header("x-signature-version", input.version);
  add_header("x-signature-nonce", input.nonce);
  add_header("x-version", config.api_version);
  add_header("accept", "application/json");
  if (!config.access_token.empty()) {
    add_header("x-access-token", config.access_token);
  }
  if (!body.empty()) {
    add_header("Content-Type", "application/json");
  }

  net::HttpClient client;
  std::string error;
  const net::HttpResponse response = client.send(request, error);
  if (!error.empty()) {
    result.error = error;
    return result;
  }

  result.status = response.status;
  result.body = response.body;

  if (response.status == 401) {
    APEX_LOG_WARN("webull",
                  "HTTP 401 for {}: verify app_key/app_secret and, if 2FA is "
                  "enabled, the access token",
                  path);
  }
  return result;
}

std::vector<std::string> escape_all(const std::vector<std::string>& values) {
  std::vector<std::string> escaped;
  escaped.reserve(values.size());
  for (const auto& value : values) escaped.push_back(json::escape(value));
  return escaped;
}

}  // namespace

WebullRestClient::WebullRestClient(WebullConfig config)
    : config_(std::move(config)) {
  config_.resolve_hosts();
}

void WebullRestClient::set_access_token(std::string token) {
  config_.access_token = std::move(token);
}

void WebullRestClient::set_path_override(std::string key, std::string path) {
  for (auto& entry : path_overrides_) {
    if (entry.first == key) {
      entry.second = std::move(path);
      return;
    }
  }
  path_overrides_.emplace_back(std::move(key), std::move(path));
}

std::string WebullRestClient::resolve_path(const std::string& key_or_path) const {
  for (const auto& entry : path_overrides_) {
    if (entry.first == key_or_path) return entry.second;
  }
  return key_or_path;
}

WebullResponse WebullRestClient::get(const std::string& path,
                                     const webull::ParamList& query) {
  return send_signed(config_, resolve_path(path), query, std::string{}, "GET");
}

WebullResponse WebullRestClient::post(const std::string& path,
                                      const webull::ParamList& query,
                                      std::string body) {
  return send_signed(config_, resolve_path(path), query, std::move(body), "POST");
}

// --- Authentication ---------------------------------------------------------

WebullResponse WebullRestClient::create_token() {
  return post(webull_path::kTokenCreate, {}, "{}");
}

WebullResponse WebullRestClient::check_token() {
  return post(webull_path::kTokenCheck, {}, "{}");
}

WebullResponse WebullRestClient::refresh_token() {
  return post(webull_path::kTokenRefresh, {}, "{}");
}

// --- Accounts & assets ------------------------------------------------------

WebullResponse WebullRestClient::list_accounts() {
  return get(webull_path::kAccountList);
}

WebullResponse WebullRestClient::account_balance(const std::string& account_id) {
  return get(webull_path::kAccountBalance, {{"account_id", account_id}});
}

WebullResponse WebullRestClient::account_positions(
    const std::string& account_id) {
  return get(webull_path::kAccountPositions, {{"account_id", account_id}});
}

// --- Orders -----------------------------------------------------------------

WebullResponse WebullRestClient::place_order(const std::string& order_json) {
  return post(webull_path::kOrderPlace, {}, order_json);
}

WebullResponse WebullRestClient::replace_order(const std::string& order_json) {
  return post(webull_path::kOrderReplace, {}, order_json);
}

WebullResponse WebullRestClient::cancel_order(
    const std::string& client_order_id) {
  const std::string body = json::Builder{}
                               .set("client_order_id", client_order_id)
                               .str();
  return post(webull_path::kOrderCancel, {}, body);
}

WebullResponse WebullRestClient::order_detail(
    const std::string& account_id, const std::string& client_order_id) {
  return get(webull_path::kOrderDetail,
             {{"account_id", account_id},
              {"client_order_id", client_order_id}});
}

WebullResponse WebullRestClient::open_orders(const std::string& account_id,
                                             int page_size) {
  return get(webull_path::kOrderOpen,
             {{"account_id", account_id},
              {"page_size", std::to_string(page_size)}});
}

WebullResponse WebullRestClient::order_history(const std::string& account_id,
                                               std::string start_date,
                                               std::string end_date,
                                               int page_size) {
  webull::ParamList query;
  query.emplace_back("account_id", account_id);
  if (!start_date.empty()) query.emplace_back("start_date", std::move(start_date));
  if (!end_date.empty()) query.emplace_back("end_date", std::move(end_date));
  query.emplace_back("page_size", std::to_string(page_size));
  return get(webull_path::kOrderHistory, query);
}

// --- Market data ------------------------------------------------------------

WebullResponse WebullRestClient::stock_snapshots(
    const std::vector<std::string>& symbols, const std::string& category,
    const std::string& sub_category) {
  json::Builder body;
  body.set_array("symbols", escape_all(symbols));
  body.set("category", category);
  if (!sub_category.empty()) body.set("sub_category", sub_category);
  return post(webull_path::kStockSnapshots, {}, body.str());
}

WebullResponse WebullRestClient::stock_bars(const std::string& symbol,
                                            const std::string& category,
                                            const std::string& timespan,
                                            int count,
                                            const std::string& sub_category) {
  webull::ParamList query;
  query.emplace_back("symbol", symbol);
  query.emplace_back("category", category);
  query.emplace_back("timespan", timespan);
  query.emplace_back("count", std::to_string(count));
  if (!sub_category.empty()) query.emplace_back("sub_category", sub_category);
  return get(webull_path::kStockBars, query);
}

WebullResponse WebullRestClient::stock_depths(const std::string& symbol,
                                              const std::string& category,
                                              int depth) {
  return get(webull_path::kStockDepths,
             {{"symbol", symbol},
              {"category", category},
              {"depth", std::to_string(depth)}});
}

// --- Streaming control (HTTP side of MQTT) ----------------------------------

WebullResponse WebullRestClient::stream_subscribe(
    const std::vector<std::string>& topics, const std::string& session_id) {
  json::Builder body;
  body.set("session_id", session_id);
  body.set_array("topics", escape_all(topics));
  return post(webull_path::kStreamSubscribe, {}, body.str());
}

WebullResponse WebullRestClient::stream_unsubscribe(
    const std::vector<std::string>& topics, const std::string& session_id) {
  json::Builder body;
  body.set("session_id", session_id);
  body.set_array("topics", escape_all(topics));
  return post(webull_path::kStreamUnsubscribe, {}, body.str());
}

}  // namespace apex
