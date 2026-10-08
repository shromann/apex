#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Minimal HTTP/1.1 client over TLS. Parses status line, headers and body
// (Content-Length or chunked transfer-encoding). Connection: close is sent, so
// no keep-alive state is kept and every request opens a fresh socket.
namespace apex::net {

struct HttpHeader {
  std::string name;
  std::string value;
};

struct HttpRequest {
  std::string method{"GET"};
  std::string host;  // e.g. "api.webull.com"
  std::string path;  // e.g. "/trading/accounts/list"
  // Query parameters, sent in this order and also used verbatim for signing.
  std::vector<std::pair<std::string, std::string>> query;
  std::vector<HttpHeader> headers;
  std::string body;
  std::uint16_t port{443};
  int timeout_ms{15000};
  bool tls{true};

  // path + "?" + raw query string (values percent-encoded), or just path.
  std::string request_target() const;
};

struct HttpResponse {
  int status{0};
  std::string reason;
  std::vector<HttpHeader> headers;
  std::string body;

  bool ok() const { return status >= 200 && status < 300; }
  std::string header(std::string_view name) const;
};

class HttpClient {
 public:
  HttpClient() = default;

  // Sends the request. Returns the response; on transport failure returns a
  // response with status 0 and sets `error`.
  HttpResponse send(const HttpRequest& request, std::string& error);

  // Test seam: parses a raw HTTP response (status line + headers + body).
  static HttpResponse parse_response(std::string_view raw, std::string& error);
};

}  // namespace apex::net
