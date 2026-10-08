#pragma once

#include <string>
#include <utility>
#include <vector>

namespace apex::webull {

using ParamList = std::vector<std::pair<std::string, std::string>>;

// Inputs to the signature algorithm. `query` and `body` must be exactly what is
// sent on the wire: the body string is hashed verbatim with MD5, so it must be
// byte-identical to the HTTP request body.
struct SignatureInput {
  std::string path;                       // e.g. "/trading/accounts/list"; "" for gRPC
  ParamList query;                        // query parameters (order preserved)
  std::string body;                       // exact request body; empty => omitted
  std::string app_key;
  std::string app_secret;
  std::string host;                       // e.g. "api.webull.com"
  std::string timestamp;                  // ISO 8601 UTC: YYYY-MM-DDThh:mm:ssZ
  std::string nonce;
  std::string algorithm{"HMAC-SHA1"};
  std::string version{"1.0"};
};

// The six signing headers (lowercase keys) that participate in the signature.
// x-signature and x-version are deliberately excluded.
ParamList signing_headers(const SignatureInput& in);

// Builds str1 (sorted, '&'-joined key=value), appends the body hash and the
// path, then URL-encodes the result. Returns the exact string that is HMAC'd.
std::string build_signature_string(const SignatureInput& in);

// Full signature: base64(HMAC-SHA1(app_secret + "&", build_signature_string(in))).
std::string sign(const SignatureInput& in);

// ISO 8601 UTC timestamp for "now" in Webull's required format.
std::string iso8601_timestamp();

// A fresh random nonce (32 lowercase hex characters).
std::string generate_nonce();

}  // namespace apex::webull
