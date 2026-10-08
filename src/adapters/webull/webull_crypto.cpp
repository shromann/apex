#include "apex/adapters/webull/webull_crypto.hpp"

#include <cstddef>

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace apex::webull {
namespace {

std::string to_hex(const unsigned char* data, std::size_t length) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(length * 2);
  for (std::size_t i = 0; i < length; ++i) {
    out[2 * i] = kHex[(data[i] >> 4) & 0x0F];
    out[2 * i + 1] = kHex[data[i] & 0x0F];
  }
  return out;
}

std::string digest_hex(const EVP_MD* md, std::string_view data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) return {};
  std::string result;
  if (EVP_DigestInit_ex(ctx, md, nullptr) == 1 &&
      EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
      EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1) {
    result = to_hex(digest, digest_len);
  }
  EVP_MD_CTX_free(ctx);
  return result;
}

}  // namespace

std::string md5_hex(std::string_view data) { return digest_hex(EVP_md5(), data); }

std::string md5_hex_upper(std::string_view data) {
  std::string hex = md5_hex(data);
  for (char& c : hex) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  return hex;
}

std::string sha1_hex(std::string_view data) { return digest_hex(EVP_sha1(), data); }

std::string base64_encode(std::string_view data) {
  if (data.empty()) return {};
  const int in_len = static_cast<int>(data.size());
  const int out_len = 4 * ((in_len + 2) / 3);
  std::string out(static_cast<std::size_t>(out_len), '\0');
  const int written = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(out.data()),
      reinterpret_cast<const unsigned char*>(data.data()), in_len);
  if (written <= 0) return {};
  out.resize(static_cast<std::size_t>(written));
  return out;
}

std::string hmac_sha1_base64(std::string_view key, std::string_view data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  unsigned char* result = HMAC(
      EVP_sha1(), key.data(), static_cast<int>(key.size()),
      reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest,
      &digest_len);
  if (result == nullptr) return {};
  return base64_encode(
      std::string_view(reinterpret_cast<const char*>(digest), digest_len));
}

std::string url_encode(std::string_view data) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(data.size() * 3);
  for (char ch : data) {
    const auto c = static_cast<unsigned char>(ch);
    const bool unreserved =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0x0F]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

}  // namespace apex::webull
