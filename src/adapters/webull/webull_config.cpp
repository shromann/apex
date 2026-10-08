#include "apex/adapters/webull/webull_config.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>

namespace apex {
namespace {

std::string read_environment(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

bool parse_bool(const std::string& value) {
  if (value.empty()) return false;
  std::string lower;
  lower.reserve(value.size());
  for (char c : value) {
    lower.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

bool parse_port(const std::string& value, std::uint16_t& out) {
  if (value.empty()) return false;
  try {
    const int port = std::stoi(value);
    if (port <= 0 || port > 65535) return false;
    out = static_cast<std::uint16_t>(port);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

void WebullConfig::resolve_hosts() {
  struct RegionHosts {
    const char* region;
    const char* api;
    const char* quotes;
    const char* events;
    const char* mqtt;
  };

  // Region table from Webull's SDK endpoint metadata.
  static const RegionHosts kProduction[] = {
      {"us", "api.webull.com", "usquotes-api.webullfintech.com",
       "events-api.webull.com", "data-api.webull.com"},
      {"hk", "api.webull.hk", "quotes-api.webull.hk", "events-api.webull.hk",
       "data-api.webull.com"},
      {"jp", "api.webull.co.jp", "", "events-api.webull.co.jp",
       "data-api.webull.com"},
  };

  const RegionHosts* hosts = &kProduction[0];  // default: us
  for (const auto& candidate : kProduction) {
    if (region == candidate.region) {
      hosts = &candidate;
      break;
    }
  }

  // Only fill endpoints that were not set explicitly (env override or caller).
  if (api_host.empty()) {
    api_host = sandbox ? "api.sandbox.webull.com" : hosts->api;
  }
  if (quotes_host.empty()) {
    quotes_host = hosts->quotes;
  }
  if (events_host.empty()) {
    events_host =
        sandbox ? "events-api.sandbox.webull.com" : hosts->events;
  }
  if (mqtt_host.empty()) {
    mqtt_host = sandbox ? "data-api.sandbox.webull.com" : hosts->mqtt;
  }
  if (mqtt_port == 0) {
    mqtt_port = 1883;
  }
}

WebullConfig WebullConfig::from_environment() {
  WebullConfig config;

  auto assign_if_set = [](std::string& field, const char* name) {
    std::string value = read_environment(name);
    if (!value.empty()) field = std::move(value);
  };

  assign_if_set(config.app_key, webull_env::kAppKey);
  assign_if_set(config.app_secret, webull_env::kAppSecret);
  assign_if_set(config.access_token, webull_env::kAccessToken);
  assign_if_set(config.account_id, webull_env::kAccountId);
  assign_if_set(config.device_id, webull_env::kDeviceId);
  assign_if_set(config.region, webull_env::kRegion);
  assign_if_set(config.api_host, webull_env::kApiHost);
  assign_if_set(config.quotes_host, webull_env::kQuotesHost);
  assign_if_set(config.events_host, webull_env::kEventsHost);
  assign_if_set(config.mqtt_host, webull_env::kMqttHost);
  assign_if_set(config.mqtt_session_id, webull_env::kMqttSessionId);

  const std::string sandbox = read_environment(webull_env::kSandbox);
  if (!sandbox.empty()) config.sandbox = parse_bool(sandbox);

  std::uint16_t port = 0;
  if (parse_port(read_environment(webull_env::kMqttPort), port)) {
    config.mqtt_port = port;
  }

  config.resolve_hosts();
  return config;
}

}  // namespace apex
