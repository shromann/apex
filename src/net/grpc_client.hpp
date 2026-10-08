#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Minimal gRPC client over HTTP/2, used for Webull's trade-events stream
// (order-status pushes). This implements just enough of HTTP/2 for a
// server-streaming RPC: connection preface, SETTINGS exchange, a HEADERS frame
// with HPACK-encoded pseudo-headers, DATA frames carrying length-prefixed
// protobuf messages, and PING/WINDOW_UPDATE handling.
//
// Scope note: this is a purpose-built subset, not a general HTTP/2 stack. It
// covers the pieces Webull's event stream needs and is documented as such.
namespace apex::net {

// A protobuf message is length-prefixed inside a gRPC DATA frame:
//   [1 byte compressed flag][4 byte big-endian length][message bytes]
struct GrpcMessage {
  bool compressed{false};
  std::string data;
};

class GrpcClient {
 public:
  using MessageHandler = std::function<void(const GrpcMessage&)>;

  GrpcClient();
  ~GrpcClient();

  GrpcClient(const GrpcClient&) = delete;
  GrpcClient& operator=(const GrpcClient&) = delete;

  void set_message_handler(MessageHandler handler);

  // Establishes TLS + HTTP/2 and opens a server-streaming call.
  bool connect(const std::string& host, std::uint16_t port,
               const std::string& path, const std::string& authority,
               const std::vector<std::pair<std::string, std::string>>& metadata,
               int timeout_ms, std::string& error);

  // Sends an initial (empty) request message to start the stream.
  bool start_stream(std::string& error);

  // Reads and dispatches one DATA frame's worth of messages.
  bool poll(int timeout_ms, std::string& error);

  void disconnect();

  // --- Test seam -------------------------------------------------------------
  static std::string encode_frame(const GrpcMessage& message);
  static bool decode_frames(std::string_view data,
                            std::vector<GrpcMessage>& messages);

 private:
  void* socket_{nullptr};  // net::TlsSocket*
  MessageHandler handler_;
  bool connected_{false};
  std::uint32_t stream_id_{1};
  std::string authority_;
};

}  // namespace apex::net
