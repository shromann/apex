#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "apex/adapters/webull/webull_config.hpp"
#include "apex/adapters/webull/webull_crypto.hpp"
#include "apex/adapters/webull/webull_signature.hpp"
#include "util/json.hpp"

#include "test_common.hpp"

namespace {

// The exact request from Webull's Signature documentation.
constexpr const char* kPath = "/trade/place_order";
constexpr const char* kBody =
    "{\"k1\":123,\"k2\":\"this is the api request body\",\"k3\":true,"
    "\"k4\":{\"foo\":[1,2]}}";
constexpr const char* kAppKey = "776da210ab4a452795d74e726ebd74b6";
constexpr const char* kAppSecret = "0f50a2e853334a9aae1a783bee120c1f";
constexpr const char* kTimestamp = "2022-01-04T03:55:31Z";
constexpr const char* kNonce = "48ef5afed43d4d91ae514aaeafbc29ba";
constexpr const char* kExpectedMd5Upper = "E296C96787E1A309691CEF3692F5EEDD";
constexpr const char* kExpectedSignature = "kvlS6opdZDhEBo5jq40nHYXaLvM=";
constexpr const char* kExpectedEncoded =
    "%2Ftrade%2Fplace_order%26a1%3Dwebull%26a2%3D123%26a3%3Dxxx%26host%3Dapi."
    "webull.com%26q1%3Dyyy%26x-app-key%3D776da210ab4a452795d74e726ebd74b6%26x-"
    "signature-algorithm%3DHMAC-SHA1%26x-signature-nonce%3D48ef5afed43d4d91ae"
    "514aaeafbc29ba%26x-signature-version%3D1.0%26x-timestamp%3D2022-01-04T03"
    "%3A55%3A31Z%26E296C96787E1A309691CEF3692F5EEDD";

apex::webull::SignatureInput worked_example() {
  apex::webull::SignatureInput in;
  in.path = kPath;
  in.query = {{"a1", "webull"}, {"a2", "123"}, {"a3", "xxx"}, {"q1", "yyy"}};
  in.body = kBody;
  in.app_key = kAppKey;
  in.app_secret = kAppSecret;
  in.host = "api.webull.com";
  in.timestamp = kTimestamp;
  in.nonce = kNonce;
  return in;
}

void test_signature() {
  using namespace apex::webull;

  // Step 4 of the algorithm: uppercase hex MD5 of the body.
  CHECK(md5_hex_upper(kBody) == kExpectedMd5Upper);

  // Step 6: the exact string that is HMAC'd, URL-encoded with an empty safe set.
  CHECK(build_signature_string(worked_example()) == kExpectedEncoded);

  // Step 3: the final signature from the documentation.
  CHECK(sign(worked_example()) == kExpectedSignature);
}

void test_crypto_vectors() {
  using namespace apex::webull;

  // Standard SHA-1 test vector.
  CHECK(sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
  CHECK(md5_hex_upper("abc") == "900150983CD24FB0D6963F7D28E17F72");

  // Base64 (standard, padded).
  CHECK(base64_encode("abc") == "YWJj");
  CHECK(base64_encode("") == "");

  // RFC 3986 with empty safe set: '/' ':' '=' '?' space are all encoded.
  CHECK(url_encode("a b/c?d=e&f") == "a%20b%2Fc%3Fd%3De%26f");
  CHECK(url_encode("-._~AZaz09") == "-._~AZaz09");

  // Nonce shape: 32 lowercase hex characters.
  const std::string nonce = generate_nonce();
  CHECK(nonce.size() == 32);
  bool hex = true;
  for (char c : nonce) hex = hex && (std::isdigit(static_cast<unsigned char>(c)) ||
                                     (c >= 'a' && c <= 'f'));
  CHECK(hex);
}

void test_json() {
  using namespace apex::json;

  // Builder must emit the exact compact body the signature hashes.
  const std::string body = Builder()
                               .set("k1", 123.0)
                               .set("k2", "this is the api request body")
                               .set("k3", true)
                               .set_raw("k4", "{\"foo\":[1,2]}")
                               .str();
  CHECK(body == kBody);

  std::string error;
  Value root = Value::parse("{\"data\":[{\"order_id\":\"7\",\"filled\":2.5}]}", error);
  CHECK(error.empty());
  CHECK(root.is_object());
  const Value* data = root.find("data");
  CHECK(data != nullptr);
  CHECK(data->is_array());
  CHECK(data->array().size() == 1);
  CHECK(data->array().front().find_string("order_id") == "7");
  CHECK(data->array().front().find_number("filled") == 2.5);
  CHECK(data->array().front().find_string("missing", "fallback") == "fallback");

  Value bad = Value::parse("{oops", error);
  CHECK(!error.empty());
  CHECK(bad.is_null());
}

void test_config_from_environment() {
  ::setenv(apex::webull_env::kAppKey, "key-123", 1);
  ::setenv(apex::webull_env::kAppSecret, "secret-456", 1);
  ::setenv(apex::webull_env::kRegion, "us", 1);
  ::unsetenv(apex::webull_env::kSandbox);

  apex::WebullConfig config = apex::WebullConfig::from_environment();
  CHECK(config.app_key == "key-123");
  CHECK(config.app_secret == "secret-456");
  CHECK(config.has_app_credentials());
  // Hosts are resolved when not explicitly provided.
  CHECK(config.api_host == "api.webull.com");
  CHECK(config.mqtt_host == "data-api.webull.com");
  CHECK(config.mqtt_port == 1883);

  // Sandbox flips the REST and MQTT hosts.
  ::setenv(apex::webull_env::kSandbox, "true", 1);
  apex::WebullConfig sandbox = apex::WebullConfig::from_environment();
  CHECK(sandbox.sandbox);
  CHECK(sandbox.api_host == "api.sandbox.webull.com");
  CHECK(sandbox.mqtt_host == "data-api.sandbox.webull.com");

  ::unsetenv(apex::webull_env::kSandbox);
  ::unsetenv(apex::webull_env::kAppKey);
  ::unsetenv(apex::webull_env::kAppSecret);
  ::unsetenv(apex::webull_env::kRegion);
}

}  // namespace

int main() {
  test_signature();
  test_crypto_vectors();
  test_json();
  test_config_from_environment();
  return apextest::summary("webull");
}
