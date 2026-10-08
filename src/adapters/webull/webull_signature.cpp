#include "apex/adapters/webull/webull_signature.hpp"

#include <cstdint>
#include <ctime>
#include <map>
#include <random>

#include "apex/adapters/webull/webull_crypto.hpp"

namespace apex::webull {

ParamList signing_headers(const SignatureInput& in) {
  return ParamList{
      {"x-app-key", in.app_key},
      {"x-timestamp", in.timestamp},
      {"x-signature-version", in.version},
      {"x-signature-algorithm", in.algorithm},
      {"x-signature-nonce", in.nonce},
      {"host", in.host},
  };
}

std::string build_signature_string(const SignatureInput& in) {
  // Merge signing headers with the query parameters. std::map keeps the keys in
  // ascending order, which is exactly the required ordering.
  std::map<std::string, std::string> merged;
  for (const auto& [key, value] : signing_headers(in)) merged[key] = value;

  for (const auto& [key, value] : in.query) {
    auto it = merged.find(key);
    if (it != merged.end()) {
      // Duplicate-key rule: join the extra values with '&'.
      it->second += "&";
      it->second += value;
    } else {
      merged[key] = value;
    }
  }

  std::string str3;
  if (!in.path.empty()) {
    std::string joined;
    for (const auto& [key, value] : merged) {
      if (!joined.empty()) joined += "&";
      joined += key;
      joined += "=";
      joined += value;
    }
    str3 = in.path;
    str3 += "&";
    str3 += joined;
  } else {
    // No path (e.g. gRPC subscriptions per the reference): the sorted pairs are
    // joined with '=' instead of '&'.
    for (const auto& [key, value] : merged) {
      if (!str3.empty()) str3 += "=";
      str3 += key;
      str3 += "=";
      str3 += value;
    }
  }

  if (!in.body.empty()) {
    str3 += "&";
    str3 += md5_hex_upper(in.body);
  }

  return url_encode(str3);
}

std::string sign(const SignatureInput& in) {
  return hmac_sha1_base64(in.app_secret + "&", build_signature_string(in));
}

std::string iso8601_timestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

std::string generate_nonce() {
  std::random_device device;
  std::mt19937_64 generator((static_cast<std::uint64_t>(device()) << 32) ^
                            static_cast<std::uint64_t>(device()));
  std::uniform_int_distribution<std::uint64_t> distribution;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (int word = 0; word < 2; ++word) {
    const std::uint64_t value = distribution(generator);
    for (int nibble = 15; nibble >= 0; --nibble) {
      out.push_back(kHex[(value >> (nibble * 4)) & 0x0F]);
    }
  }
  return out;
}

}  // namespace apex::webull
