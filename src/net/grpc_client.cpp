#include "grpc_client.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tls_socket.hpp"

// ---------------------------------------------------------------------------
// SCOPE
// ---------------------------------------------------------------------------
// This implements only what Webull's trade-events stream requires: TLS, the
// HTTP/2 connection preface and SETTINGS exchange, a HEADERS frame carrying a
// small HPACK block, and DATA frames carrying length-prefixed protobuf
// messages. It is NOT a general HTTP/2 or HPACK implementation:
//   * no dynamic HPACK table, no Huffman coding (only static-table indices and
//     literal-without-indexing / literal names);
//   * HEADERS response blocks are skipped, not decoded;
//   * no flow-control accounting (WINDOW_UPDATE is parsed and ignored);
//   * no CONTINUATION frames, no priority handling.
// It has not been verified against a live Webull endpoint.
namespace apex::net {
namespace {

// --- HTTP/2 frame header (9 bytes) -----------------------------------------
//   [3 bytes length][1 byte type][1 byte flags][4 bytes stream id]

void put_u24(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

void put_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

std::string make_frame(std::uint8_t type, std::uint8_t flags,
                       std::uint32_t stream_id, std::string_view payload) {
  std::string frame;
  put_u24(frame, static_cast<std::uint32_t>(payload.size()));
  frame.push_back(static_cast<char>(type));
  frame.push_back(static_cast<char>(flags));
  put_u32(frame, stream_id & 0x7FFFFFFFu);
  frame.append(payload.data(), payload.size());
  return frame;
}

// Reads exactly `needed` bytes into `out`, appending to whatever is already
// there. Returns false on a socket error; the caller inspects out.size() to
// distinguish "nothing available yet" (out empty) from a partial frame.
bool read_exact(TlsSocket* socket, std::string& out, std::size_t needed,
                std::string& error) {
  char chunk[4096];
  while (out.size() < needed) {
    const std::size_t want = std::min<std::size_t>(sizeof(chunk), needed - out.size());
    std::size_t nread = 0;
    if (!socket->read(chunk, want, nread, error)) return false;
    if (nread == 0) {
      error = "grpc: connection closed by peer";
      return false;
    }
    out.append(chunk, nread);
  }
  return true;
}

// --- Minimal HPACK encoder --------------------------------------------------
// Static table entries used below:
//   1 :authority   3 :method POST   4 :path /   7 :scheme https

void hpack_int(std::string& out, std::uint32_t value, std::uint8_t prefix_bits,
               std::uint8_t first_byte) {
  const std::uint32_t max_prefix = (1u << prefix_bits) - 1;
  if (value < max_prefix) {
    out.push_back(static_cast<char>(first_byte | value));
    return;
  }
  out.push_back(static_cast<char>(first_byte | max_prefix));
  value -= max_prefix;
  while (value >= 128) {
    out.push_back(static_cast<char>((value % 128) + 128));
    value /= 128;
  }
  out.push_back(static_cast<char>(value));
}

// Plain (non-Huffman) string literal: 7-bit length prefix with H bit clear.
void hpack_string(std::string& out, std::string_view value) {
  hpack_int(out, static_cast<std::uint32_t>(value.size()), 7, 0x00);
  out.append(value.data(), value.size());
}

// Indexed header field: 0x80 | index.
void hpack_indexed(std::string& out, std::uint32_t index) {
  hpack_int(out, index, 7, 0x80);
}

// Literal without indexing, name taken from the static table (4-bit index).
void hpack_literal_indexed_name(std::string& out, std::uint32_t name_index,
                                std::string_view value) {
  hpack_int(out, name_index, 4, 0x00);
  hpack_string(out, value);
}

// Literal without indexing with a literal name (0x00 prefix, name index 0).
void hpack_literal_name(std::string& out, std::string_view name,
                        std::string_view value) {
  hpack_int(out, 0, 4, 0x00);
  hpack_string(out, name);
  hpack_string(out, value);
}

}  // namespace

GrpcClient::GrpcClient() = default;

GrpcClient::~GrpcClient() { disconnect(); }

void GrpcClient::set_message_handler(MessageHandler handler) {
  handler_ = std::move(handler);
}

std::string GrpcClient::encode_frame(const GrpcMessage& message) {
  std::string out;
  out.push_back(static_cast<char>(message.compressed ? 0x01 : 0x00));
  put_u32(out, static_cast<std::uint32_t>(message.data.size()));
  out.append(message.data);
  return out;
}

bool GrpcClient::decode_frames(std::string_view data,
                               std::vector<GrpcMessage>& messages) {
  messages.clear();
  std::size_t offset = 0;
  while (offset + 5 <= data.size()) {
    GrpcMessage message;
    message.compressed = (static_cast<std::uint8_t>(data[offset]) & 0x01) != 0;
    const std::uint32_t length =
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 1])) << 24) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 2])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 3])) << 8) |
        static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 4]));
    offset += 5;
    if (offset + length > data.size()) return false;  // truncated frame
    message.data.assign(data.data() + offset, length);
    offset += length;
    messages.push_back(std::move(message));
  }
  return offset == data.size();
}

bool GrpcClient::connect(
    const std::string& host, std::uint16_t port, const std::string& path,
    const std::string& authority,
    const std::vector<std::pair<std::string, std::string>>& metadata,
    int timeout_ms, std::string& error) {
  auto* socket = new TlsSocket();
  std::string socket_error;
  if (!socket->connect(host, port, timeout_ms, socket_error)) {
    delete socket;
    error = "grpc: TLS connection to " + host + " failed: " + socket_error;
    return false;
  }
  socket_ = socket;
  authority_ = authority;

  // HTTP/2 client connection preface, immediately followed by an empty SETTINGS
  // frame. (ALPN negotiation for "h2" is not required by this subset; if the
  // server does not advertise it we still speak HTTP/2 directly.)
  static const std::string kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  if (!socket->write(kPreface, error)) {
    disconnect();
    return false;
  }
  if (!socket->write(make_frame(0x04, 0x00, 0, {}), error)) {
    disconnect();
    return false;
  }

  // Read the server's SETTINGS frame.
  std::string header;
  std::string read_error;
  if (!read_exact(socket, header, 9, read_error)) {
    error = read_error;
    disconnect();
    return false;
  }
  const std::uint8_t peer_type = static_cast<std::uint8_t>(header[3]);
  if (peer_type != 0x04) {
    error = "grpc: expected SETTINGS frame, got type " +
            std::to_string(peer_type);
    disconnect();
    return false;
  }
  const std::uint32_t peer_length =
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[0])) << 16) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[1])) << 8) |
      static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[2]));
  if (peer_length > 0) {
    std::string ignored;
    if (!read_exact(socket, ignored, peer_length, read_error)) {
      error = read_error;
      disconnect();
      return false;
    }
  }

  // Acknowledge the server's SETTINGS.
  if (!socket->write(make_frame(0x04, 0x01, 0, {}), error)) {
    disconnect();
    return false;
  }

  // HEADERS with END_HEADERS. HPACK block: :method, :scheme, :path,
  // :authority, then the gRPC content-type / te headers and caller metadata.
  std::string hpack;
  hpack_indexed(hpack, 3);  // :method POST
  hpack_indexed(hpack, 7);  // :scheme https
  if (path == "/") {
    hpack_indexed(hpack, 4);  // :path /
  } else {
    hpack_literal_indexed_name(hpack, 4, path);  // :path <path>
  }
  hpack_literal_indexed_name(hpack, 1, authority);  // :authority <authority>
  hpack_literal_name(hpack, "content-type", "application/grpc");
  hpack_literal_name(hpack, "te", "trailers");
  for (const auto& [name, value] : metadata) {
    hpack_literal_name(hpack, name, value);
  }

  if (!socket->write(make_frame(0x01, 0x04, stream_id_, hpack), error)) {
    disconnect();
    return false;
  }

  connected_ = true;
  return true;
}

bool GrpcClient::start_stream(std::string& error) {
  auto* socket = static_cast<TlsSocket*>(socket_);
  if (socket == nullptr || !connected_) {
    error = "grpc: not connected";
    return false;
  }
  // One gRPC message: an empty protobuf body is the 5-byte frame
  // 00 00 00 00 00. The stream is left half-open (no END_STREAM) because this
  // is a long-lived server-streaming subscription.
  GrpcMessage empty;
  empty.compressed = false;
  empty.data.clear();
  const std::string body = encode_frame(empty);
  return socket->write(make_frame(0x00, 0x00, stream_id_, body), error);
}

bool GrpcClient::poll(int timeout_ms, std::string& error) {
  (void)timeout_ms;  // the socket applies its own read timeout
  auto* socket = static_cast<TlsSocket*>(socket_);
  if (socket == nullptr || !socket->is_open()) {
    error = "grpc: not connected";
    return false;
  }

  std::string header;
  std::string read_error;
  if (!read_exact(socket, header, 9, read_error)) {
    if (header.empty()) return true;  // idle: nothing to read this round
    error = read_error;
    return false;
  }

  const std::uint32_t length =
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[0])) << 16) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[1])) << 8) |
      static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[2]));
  const std::uint8_t type = static_cast<std::uint8_t>(header[3]);
  const std::uint8_t flags = static_cast<std::uint8_t>(header[4]);
  const std::uint32_t stream =
      ((static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[5])) << 24) |
       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[6])) << 16) |
       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[7])) << 8) |
       static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[8]))) &
      0x7FFFFFFFu;

  std::string payload;
  if (length > 0) {
    if (!read_exact(socket, payload, length, read_error)) {
      error = read_error;
      return false;
    }
  }

  switch (type) {
    case 0x00: {  // DATA
      if (stream == stream_id_) {
        std::vector<GrpcMessage> messages;
        if (decode_frames(payload, messages)) {
          for (const auto& message : messages) {
            if (handler_) handler_(message);
          }
        }
        if ((flags & 0x01) != 0) connected_ = false;  // END_STREAM
      }
      break;
    }
    case 0x01:  // HEADERS: HPACK decoding is out of scope for this subset
      break;
    case 0x03:  // RST_STREAM: the call ended
      connected_ = false;
      break;
    case 0x06:  // PING
      if ((flags & 0x01) == 0) {  // reply unless it is already an ACK
        std::string ignored;
        socket->write(make_frame(0x06, 0x01, 0, {}), ignored);
      }
      break;
    case 0x07:  // GOAWAY
      connected_ = false;
      break;
    case 0x08:  // WINDOW_UPDATE: parsed and ignored (no flow-control accounting)
    default:
      break;
  }
  return true;
}

void GrpcClient::disconnect() {
  if (socket_ != nullptr) {
    auto* socket = static_cast<TlsSocket*>(socket_);
    socket->close();
    delete socket;
    socket_ = nullptr;
  }
  connected_ = false;
}

}  // namespace apex::net
