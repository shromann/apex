#pragma once

#include <cstdint>
#include <string>

namespace apex {

// Environment variable names. The CLI writes these (see main.cpp) so credentials
// are configured once and picked up by the adapter via from_environment().
namespace webull_env {
inline constexpr const char* kAppKey = "WEBULL_APP_KEY";
inline constexpr const char* kAppSecret = "WEBULL_APP_SECRET";
inline constexpr const char* kAccessToken = "WEBULL_ACCESS_TOKEN";
inline constexpr const char* kAccountId = "WEBULL_ACCOUNT_ID";
inline constexpr const char* kRegion = "WEBULL_REGION";
inline constexpr const char* kSandbox = "WEBULL_SANDBOX";
inline constexpr const char* kDeviceId = "WEBULL_DEVICE_ID";
inline constexpr const char* kApiHost = "WEBULL_API_HOST";
inline constexpr const char* kQuotesHost = "WEBULL_QUOTES_HOST";
inline constexpr const char* kEventsHost = "WEBULL_EVENTS_HOST";
inline constexpr const char* kMqttHost = "WEBULL_MQTT_HOST";
inline constexpr const char* kMqttPort = "WEBULL_MQTT_PORT";
inline constexpr const char* kMqttSessionId = "WEBULL_MQTT_SESSION_ID";
}  // namespace webull_env

// Connection settings for Webull's OpenAPI. Webull exposes a REST API for
// account/order operations, an MQTT stream for market data, and gRPC for trade
// events. There is no official C++ SDK, so this adapter talks those protocols
// directly (see src/net).
struct WebullConfig {
  std::string region{"us"};  // "us", "hk", "jp"
  bool sandbox{false};

  // Credentials (from WEBULL_* environment variables).
  std::string app_key;
  std::string app_secret;
  std::string access_token;  // required only when 2FA is enabled
  std::string account_id;
  std::string device_id;

  // Endpoints. Left empty, resolve_hosts() fills them from region + sandbox.
  std::string api_host;     // REST: orders, accounts, assets
  std::string quotes_host;  // market data REST (may differ from api_host)
  std::string events_host;  // trade events (gRPC)
  std::string mqtt_host;    // market data streaming
  std::uint16_t mqtt_port{1883};

  std::string mqtt_session_id{"apex_session_1"};
  int http_timeout_ms{15000};
  std::string api_version{"v3"};  // x-version request header

  // Fills any empty endpoint from the region/sandbox table.
  void resolve_hosts();

  bool has_app_credentials() const {
    return !app_key.empty() && !app_secret.empty();
  }
  // A signed REST call needs app credentials; an access token is additionally
  // required when the application has 2FA enabled.
  bool is_configured() const { return has_app_credentials(); }

  // Reads credentials/endpoints from the WEBULL_* environment variables and
  // resolves hosts. Returns a config whose is_configured() reflects the env.
  static WebullConfig from_environment();
};

}  // namespace apex
