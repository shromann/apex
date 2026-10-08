#pragma once

#include <string>
#include <string_view>

// Low-level primitives required by Webull's request-signing algorithm:
//   signature = base64( HMAC-SHA1( app_secret + "&", urlencode(str3) ) )
//   str3      = path + "&" + str1 + ["&" + upper(MD5(body))]
// Backed by OpenSSL (EVP/MD5/HMAC). Exposed separately from the signer so the
// primitives can be unit-tested against standard test vectors.
namespace apex::webull {

// Lowercase hex MD5 digest.
std::string md5_hex(std::string_view data);

// Uppercase hex MD5 digest -- what the signing spec uses for the body hash.
std::string md5_hex_upper(std::string_view data);

std::string sha1_hex(std::string_view data);

// Standard base64 (RFC 4648, with padding).
std::string base64_encode(std::string_view data);

// base64( HMAC-SHA1(key, data) ).
std::string hmac_sha1_base64(std::string_view key, std::string_view data);

// RFC 3986 percent-encoding with an empty "safe" set: every byte except
// ALPHA / DIGIT / '-' / '.' / '_' / '~' is encoded as %XX (uppercase hex).
// This matches Python's urllib.parse.quote(s, safe='') used by Webull's SDK.
std::string url_encode(std::string_view data);

}  // namespace apex::webull
