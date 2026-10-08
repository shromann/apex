#include "http_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include "apex/adapters/webull/webull_crypto.hpp"
#include "tls_socket.hpp"

namespace apex::net {
namespace {

constexpr std::size_t kMaxResponseBytes = 8u * 1024u * 1024u;

std::string to_lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                           text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

bool has_header(const HttpRequest& request, std::string_view name) {
  for (const auto& header : request.headers) {
    if (to_lower(header.name) == to_lower(name)) return true;
  }
  return false;
}

// Decodes a Transfer-Encoding: chunked body.
bool decode_chunked(std::string_view body, std::string& out,
                    std::string& error) {
  std::size_t cursor = 0;
  while (true) {
    const std::size_t line_end = body.find("\r\n", cursor);
    if (line_end == std::string_view::npos) {
      error = "malformed chunked body: missing size line";
      return false;
    }
    std::string size_text(body.substr(cursor, line_end - cursor));
    const std::size_t semicolon = size_text.find(';');  // chunk extension
    if (semicolon != std::string::npos) size_text = size_text.substr(0, semicolon);
    size_text = std::string(trim(size_text));
    if (size_text.empty()) {
      error = "malformed chunked body: empty chunk size";
      return false;
    }

    char* end_ptr = nullptr;
    const unsigned long chunk_size = std::strtoul(size_text.c_str(), &end_ptr, 16);
    if (end_ptr == size_text.c_str()) {
      error = "malformed chunked body: invalid chunk size";
      return false;
    }
    cursor = line_end + 2;

    if (chunk_size == 0) {
      return true;  // last chunk; trailers (if any) are ignored
    }
    if (cursor + chunk_size > body.size()) {
      error = "malformed chunked body: chunk exceeds data";
      return false;
    }
    out.append(body.substr(cursor, chunk_size));
    cursor += chunk_size;

    if (body.substr(cursor, 2) == "\r\n") cursor += 2;
  }
}

}  // namespace

std::string HttpRequest::request_target() const {
  if (query.empty()) return path;
  std::string target = path;
  target += '?';
  bool first = true;
  for (const auto& [key, value] : query) {
    if (!first) target += '&';
    first = false;
    target += key;
    target += '=';
    target += webull::url_encode(value);
  }
  return target;
}

std::string HttpResponse::header(std::string_view name) const {
  const std::string wanted = to_lower(name);
  for (const auto& header : headers) {
    if (to_lower(header.name) == wanted) return header.value;
  }
  return {};
}

HttpResponse HttpClient::send(const HttpRequest& request, std::string& error) {
  error.clear();

  std::string message;
  message.reserve(512 + request.body.size());
  message += request.method;
  message += ' ';
  message += request.request_target();
  message += " HTTP/1.1\r\n";

  message += "Host: ";
  message += request.host;
  if (request.port != 443 && request.port != 80) {
    message += ':';
    message += std::to_string(request.port);
  }
  message += "\r\n";

  if (!has_header(request, "user-agent")) {
    message += "User-Agent: apex/0.1\r\n";
  }
  if (!has_header(request, "connection")) {
    message += "Connection: close\r\n";
  }

  const bool is_post = to_lower(request.method) == "post";
  if (is_post && !request.body.empty() &&
      !has_header(request, "content-type")) {
    message += "Content-Type: application/json\r\n";
  }
  if (!has_header(request, "content-length")) {
    message += "Content-Length: ";
    message += std::to_string(request.body.size());
    message += "\r\n";
  }

  for (const auto& header : request.headers) {
    message += header.name;
    message += ": ";
    message += header.value;
    message += "\r\n";
  }
  message += "\r\n";
  message += request.body;

  if (!request.tls) {
    error = "plaintext HTTP transport is not supported (HTTPS only)";
    return {};
  }

  TlsSocket socket;
  if (!socket.connect(request.host, request.port, request.timeout_ms, error)) {
    return {};
  }
  if (!socket.write(message, error)) {
    return {};
  }

  std::string raw;
  if (!socket.read_to_end(raw, kMaxResponseBytes, error)) {
    return {};
  }

  std::string parse_error;
  HttpResponse response = parse_response(raw, parse_error);
  if (!parse_error.empty()) {
    error = parse_error;
    return {};
  }
  return response;
}

HttpResponse HttpClient::parse_response(std::string_view raw,
                                        std::string& error) {
  error.clear();
  HttpResponse response;

  const std::size_t status_end = raw.find("\r\n");
  if (status_end == std::string_view::npos) {
    error = "malformed HTTP response: missing status line";
    return {};
  }

  const std::string_view status_line = raw.substr(0, status_end);
  const std::size_t first_space = status_line.find(' ');
  if (first_space == std::string_view::npos) {
    error = "malformed HTTP status line";
    return {};
  }
  std::string_view remainder = status_line.substr(first_space + 1);
  const std::size_t second_space = remainder.find(' ');
  std::string_view code_text = remainder;
  if (second_space != std::string_view::npos) {
    code_text = remainder.substr(0, second_space);
    response.reason = std::string(trim(remainder.substr(second_space + 1)));
  }
  response.status = std::atoi(std::string(code_text).c_str());

  std::size_t pos = status_end + 2;
  while (true) {
    const std::size_t line_end = raw.find("\r\n", pos);
    if (line_end == std::string_view::npos) {
      error = "malformed HTTP response: unterminated headers";
      return {};
    }
    if (line_end == pos) {
      pos += 2;  // blank line ends the header block
      break;
    }
    const std::string_view line = raw.substr(pos, line_end - pos);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      error = "malformed HTTP header line";
      return {};
    }
    HttpHeader header;
    header.name = std::string(trim(line.substr(0, colon)));
    header.value = std::string(trim(line.substr(colon + 1)));
    response.headers.push_back(std::move(header));
    pos = line_end + 2;
  }

  const std::string_view rest = raw.substr(pos);

  if (to_lower(response.header("transfer-encoding")).find("chunked") !=
      std::string::npos) {
    std::string decoded;
    if (!decode_chunked(rest, decoded, error)) return {};
    response.body = std::move(decoded);
    return response;
  }

  const std::string content_length = response.header("content-length");
  if (!content_length.empty()) {
    const std::size_t declared =
        static_cast<std::size_t>(std::strtoull(content_length.c_str(), nullptr, 10));
    response.body = std::string(rest.substr(0, std::min(declared, rest.size())));
  } else {
    response.body = std::string(rest);
  }
  return response;
}

}  // namespace apex::net
