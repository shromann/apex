#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Blocking TLS client socket built on POSIX sockets + OpenSSL. OpenSSL types
// are hidden behind void* so this header does not leak <openssl/...>.
namespace apex::net {

class TlsSocket {
 public:
  TlsSocket();
  ~TlsSocket();

  TlsSocket(const TlsSocket&) = delete;
  TlsSocket& operator=(const TlsSocket&) = delete;

  // Resolves host, opens a TCP connection, and performs the TLS handshake with
  // SNI + certificate verification against the system trust store.
  bool connect(const std::string& host, std::uint16_t port, int timeout_ms,
               std::string& error);

  bool write(std::string_view data, std::string& error);

  // Reads up to `capacity` bytes. Sets nread and returns true on success
  // (nread == 0 means the peer cleanly closed). Returns false on error.
  bool read(char* buffer, std::size_t capacity, std::size_t& nread,
            std::string& error);

  // Reads until the peer closes or `max_bytes` is reached.
  bool read_to_end(std::string& out, std::size_t max_bytes, std::string& error);

  void close();
  bool is_open() const { return fd_ >= 0; }

 private:
  void* ssl_{nullptr};  // SSL*
  void* ctx_{nullptr};  // SSL_CTX*
  int fd_{-1};
};

}  // namespace apex::net
