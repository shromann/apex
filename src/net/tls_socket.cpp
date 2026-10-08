#include "tls_socket.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <mutex>
#include <string>

namespace apex::net {
namespace {

std::once_flag g_openssl_once;

// Collects the whole OpenSSL error queue into one readable message.
std::string openssl_error(const std::string& context) {
  std::string message = context + ": ";
  bool first = true;
  for (unsigned long code = ERR_get_error(); code != 0; code = ERR_get_error()) {
    char buffer[256];
    ERR_error_string_n(code, buffer, sizeof(buffer));
    if (!first) message += "; ";
    message += buffer;
    first = false;
  }
  if (first) message += "unknown OpenSSL error";
  return message;
}

std::string numeric_host(const std::string& host) {
  // Best-effort reverse lookup for a friendlier error message only.
  return host;
}

}  // namespace

TlsSocket::TlsSocket() {
  std::call_once(g_openssl_once, [] { OPENSSL_init_ssl(0, nullptr); });
}

TlsSocket::~TlsSocket() { close(); }

bool TlsSocket::connect(const std::string& host, std::uint16_t port,
                        int timeout_ms, std::string& error) {
  close();

  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (ctx == nullptr) {
    error = openssl_error("SSL_CTX_new");
    return false;
  }
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
    error = openssl_error("SSL_CTX_set_default_verify_paths");
    SSL_CTX_free(ctx);
    return false;
  }
  ctx_ = ctx;

  const std::string port_str = std::to_string(port);

  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo* resolved = nullptr;
  const int gai = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &resolved);
  if (gai != 0) {
    error = "getaddrinfo(" + numeric_host(host) + "): " + ::gai_strerror(gai);
    close();
    return false;
  }

  int fd = -1;
  for (struct addrinfo* ai = resolved; ai != nullptr; ai = ai->ai_next) {
    const int candidate = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (candidate < 0) continue;

    if (timeout_ms > 0) {
      struct timeval tv{};
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
      ::setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      ::setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (::connect(candidate, ai->ai_addr, ai->ai_addrlen) == 0) {
      fd = candidate;
      break;
    }
    ::close(candidate);
  }
  ::freeaddrinfo(resolved);

  if (fd < 0) {
    error = "tcp connect failed for " + host + ":" + port_str;
    close();
    return false;
  }
  fd_ = fd;

  SSL* ssl = SSL_new(ctx);
  if (ssl == nullptr) {
    error = openssl_error("SSL_new");
    close();
    return false;
  }

  // SNI so the server presents the right certificate, plus hostname
  // verification of that certificate. Verification is never disabled.
  ::SSL_set_tlsext_host_name(ssl, host.c_str());
  if (SSL_set1_host(ssl, host.c_str()) != 1) {
    error = openssl_error("SSL_set1_host");
    SSL_free(ssl);
    close();
    return false;
  }
  SSL_set_fd(ssl, fd_);

  if (SSL_connect(ssl) != 1) {
    error = openssl_error("TLS handshake with " + host);
    SSL_free(ssl);
    close();
    return false;
  }

  ssl_ = ssl;
  return true;
}

bool TlsSocket::write(std::string_view data, std::string& error) {
  if (ssl_ == nullptr) {
    error = "socket is not connected";
    return false;
  }
  SSL* ssl = static_cast<SSL*>(ssl_);

  std::size_t total = 0;
  while (total < data.size()) {
    std::size_t chunk = data.size() - total;
    if (chunk > static_cast<std::size_t>(INT_MAX)) {
      chunk = static_cast<std::size_t>(INT_MAX);
    }
    const int written =
        SSL_write(ssl, data.data() + total, static_cast<int>(chunk));
    if (written <= 0) {
      error = openssl_error("SSL_write");
      return false;
    }
    total += static_cast<std::size_t>(written);
  }
  return true;
}

bool TlsSocket::read(char* buffer, std::size_t capacity, std::size_t& nread,
                     std::string& error) {
  nread = 0;
  if (ssl_ == nullptr) {
    error = "socket is not connected";
    return false;
  }
  SSL* ssl = static_cast<SSL*>(ssl_);

  const std::size_t want = std::min<std::size_t>(capacity, INT_MAX);
  const int got = SSL_read(ssl, buffer, static_cast<int>(want));
  if (got > 0) {
    nread = static_cast<std::size_t>(got);
    return true;
  }

  const int reason = SSL_get_error(ssl, got);
  if (reason == SSL_ERROR_ZERO_RETURN) {
    return true;  // clean TLS shutdown
  }
  if (reason == SSL_ERROR_SYSCALL && got == 0) {
    return true;  // peer closed without close_notify; treat as end of stream
  }
  error = openssl_error("SSL_read");
  return false;
}

bool TlsSocket::read_to_end(std::string& out, std::size_t max_bytes,
                            std::string& error) {
  char buffer[16384];
  while (out.size() < max_bytes) {
    const std::size_t want =
        std::min<std::size_t>(sizeof(buffer), max_bytes - out.size());
    std::size_t got = 0;
    if (!read(buffer, want, got, error)) return false;
    if (got == 0) return true;
    out.append(buffer, got);
  }
  return true;
}

void TlsSocket::close() {
  if (ssl_ != nullptr) {
    SSL* ssl = static_cast<SSL*>(ssl_);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    ssl_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (ctx_ != nullptr) {
    SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
    ctx_ = nullptr;
  }
}

}  // namespace apex::net
