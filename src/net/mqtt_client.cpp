#include "mqtt_client.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tls_socket.hpp"

namespace apex::net {
namespace {

// MQTT strings are length-prefixed with a 16-bit big-endian length.
void append_string(std::string& out, std::string_view value) {
  out.push_back(static_cast<char>((value.size() >> 8) & 0xFF));
  out.push_back(static_cast<char>(value.size() & 0xFF));
  out.append(value.data(), value.size());
}

constexpr std::uint8_t kTypeConnect = 0x01;
constexpr std::uint8_t kTypeConnack = 0x02;
constexpr std::uint8_t kTypePublish = 0x03;
constexpr std::uint8_t kTypePingreq = 0x06;
constexpr std::uint8_t kTypeSuback = 0x09;
constexpr std::uint8_t kTypePingresp = 0x0D;
constexpr std::uint8_t kTypeDisconnect = 0x0E;

// Timeout used while waiting for a handshake response (CONNACK / SUBACK).
constexpr int kHandshakeTimeoutMs = 5000;

}  // namespace

MqttClient::MqttClient() = default;

MqttClient::~MqttClient() { disconnect(); }

void MqttClient::set_message_handler(MessageHandler handler) {
  handler_ = std::move(handler);
}

bool MqttClient::encode_length(std::size_t length, std::string& out) {
  do {
    std::uint8_t byte = static_cast<std::uint8_t>(length % 128);
    length /= 128;
    if (length > 0) byte |= 0x80;
    out.push_back(static_cast<char>(byte));
  } while (length > 0);
  return true;
}

bool MqttClient::decode_length(std::string_view buffer, std::size_t& length,
                               std::size_t& consumed) {
  length = 0;
  consumed = 0;
  std::size_t multiplier = 1;
  for (std::size_t i = 0; i < buffer.size() && i < 4; ++i) {
    const std::uint8_t byte = static_cast<std::uint8_t>(buffer[i]);
    length += (byte & 0x7F) * multiplier;
    ++consumed;
    if ((byte & 0x80) == 0) return true;
    multiplier *= 128;
  }
  return false;
}

bool MqttClient::parse_packet(std::string_view buffer, Packet& packet,
                              std::size_t& consumed) {
  consumed = 0;
  if (buffer.size() < 2) return false;

  const std::uint8_t first = static_cast<std::uint8_t>(buffer[0]);
  packet.type = static_cast<std::uint8_t>(first >> 4);
  packet.flags = static_cast<std::uint8_t>(first & 0x0F);
  packet.packet_id = 0;
  packet.topic.clear();
  packet.payload.clear();

  std::size_t remaining = 0;
  std::size_t length_bytes = 0;
  if (!decode_length(buffer.substr(1), remaining, length_bytes)) return false;

  const std::size_t header_size = 1 + length_bytes;
  if (buffer.size() < header_size + remaining) return false;
  const std::string_view body = buffer.substr(header_size, remaining);

  switch (packet.type) {
    case kTypePublish: {
      if (body.size() < 2) return false;
      const std::size_t topic_length =
          (static_cast<std::size_t>(static_cast<std::uint8_t>(body[0])) << 8) |
          static_cast<std::size_t>(static_cast<std::uint8_t>(body[1]));
      if (body.size() < 2 + topic_length) return false;
      packet.topic.assign(body.data() + 2, topic_length);
      std::size_t offset = 2 + topic_length;
      const int qos = (first >> 1) & 0x03;
      if (qos > 0) {
        if (body.size() < offset + 2) return false;
        packet.packet_id =
            static_cast<std::uint16_t>(
                (static_cast<std::uint8_t>(body[offset]) << 8) |
                static_cast<std::uint8_t>(body[offset + 1]));
        offset += 2;
      }
      packet.payload.assign(body.data() + offset, body.size() - offset);
      break;
    }
    case kTypeConnack:   // 0x02
    case kTypeSuback:    // 0x09
    case 0x04:           // PUBACK
    case 0x05:           // PUBREC
    case 0x0B:           // UNSUBACK
    case 0x0C:           // PUBCOMP
    case kTypePingresp:  // 0x0D
      if (body.size() >= 2) {
        packet.packet_id =
            static_cast<std::uint16_t>(
                (static_cast<std::uint8_t>(body[0]) << 8) |
                static_cast<std::uint8_t>(body[1]));
      }
      packet.payload.assign(body);
      break;
    default:
      packet.payload.assign(body);
      break;
  }

  consumed = header_size + remaining;
  return true;
}

bool MqttClient::send_packet(std::uint8_t type, std::uint8_t flags,
                             const std::string& payload, std::string& error) {
  auto* socket = static_cast<TlsSocket*>(socket_);
  if (socket == nullptr || !socket->is_open()) {
    error = "mqtt: socket is not open";
    return false;
  }

  std::string packet;
  packet.push_back(static_cast<char>((type << 4) | (flags & 0x0F)));
  encode_length(payload.size(), packet);
  packet.append(payload);
  return socket->write(packet, error);
}

bool MqttClient::wait_readable(int timeout_ms, std::string& error) {
  // TlsSocket does not expose its descriptor, so we cannot select() on it from
  // here. Instead we attempt one read; the socket applies the timeout configured
  // at connect() time. A failed read therefore means "no data available (yet)"
  // rather than a fatal error, and the caller simply retries. Only a clean EOF
  // (nread == 0) is treated as a closed connection.
  (void)timeout_ms;
  auto* socket = static_cast<TlsSocket*>(socket_);
  if (socket == nullptr || !socket->is_open()) {
    error = "mqtt: not connected";
    return false;
  }

  char chunk[4096];
  std::size_t nread = 0;
  std::string read_error;
  if (!socket->read(chunk, sizeof(chunk), nread, read_error)) {
    return true;
  }
  if (nread == 0) {
    error = "mqtt: connection closed by broker";
    return false;
  }
  buffer_.append(chunk, nread);
  return true;
}

bool MqttClient::read_packet(Packet& packet, int timeout_ms,
                             std::string& error) {
  packet = Packet{};
  for (int attempt = 0; attempt < 16; ++attempt) {
    std::size_t consumed = 0;
    if (parse_packet(buffer_, packet, consumed)) {
      buffer_.erase(0, consumed);
      return true;
    }
    const std::size_t before = buffer_.size();
    if (!wait_readable(timeout_ms, error)) return false;
    if (buffer_.size() == before) return true;  // nothing available; type == 0
  }
  return true;
}

bool MqttClient::connect(const std::string& host, std::uint16_t port,
                         const std::string& client_id,
                         const std::string& username,
                         const std::string& password, int keep_alive_seconds,
                         int timeout_ms, std::string& error) {
  if (connected_) return true;

  auto* socket = new TlsSocket();
  std::string connect_error;
  if (!socket->connect(host, port, timeout_ms, connect_error)) {
    delete socket;
    error = "mqtt: TLS connection to " + host + " failed: " + connect_error;
    return false;
  }
  socket_ = socket;
  buffer_.clear();

  std::string variable;
  append_string(variable, "MQTT");
  variable.push_back(0x04);  // protocol level 4 (MQTT 3.1.1)
  std::uint8_t flags = 0x02;  // clean session
  if (!username.empty()) flags |= 0x80;
  if (!password.empty()) flags |= 0x40;
  variable.push_back(static_cast<char>(flags));
  variable.push_back(
      static_cast<char>((static_cast<unsigned>(keep_alive_seconds) >> 8) & 0xFF));
  variable.push_back(static_cast<char>(static_cast<unsigned>(keep_alive_seconds) & 0xFF));
  append_string(variable, client_id);
  if (!username.empty()) append_string(variable, username);
  if (!password.empty()) append_string(variable, password);

  if (!send_packet(kTypeConnect, 0x00, variable, error)) {
    disconnect();
    return false;
  }

  Packet packet;
  for (int attempt = 0; attempt < 64; ++attempt) {
    if (!read_packet(packet, kHandshakeTimeoutMs, error)) {
      disconnect();
      return false;
    }
    if (packet.type != 0) break;
  }
  if (packet.type != kTypeConnack) {
    error = "mqtt: expected CONNACK, got packet type " +
            std::to_string(packet.type);
    disconnect();
    return false;
  }
  if (packet.payload.size() < 2 ||
      static_cast<std::uint8_t>(packet.payload[1]) != 0) {
    const int code = packet.payload.size() > 1
                         ? static_cast<int>(
                               static_cast<std::uint8_t>(packet.payload[1]))
                         : -1;
    error = "mqtt: connection refused, CONNACK code " + std::to_string(code);
    disconnect();
    return false;
  }

  connected_ = true;
  return true;
}

bool MqttClient::subscribe(const std::vector<std::string>& topics,
                           std::string& error) {
  if (!connected_) {
    error = "mqtt: not connected";
    return false;
  }
  const std::uint16_t packet_id = next_packet_id_++;

  std::string payload;
  payload.push_back(static_cast<char>((packet_id >> 8) & 0xFF));
  payload.push_back(static_cast<char>(packet_id & 0xFF));
  for (const auto& topic : topics) {
    append_string(payload, topic);
    payload.push_back(0x00);  // requested QoS 0
  }

  if (!send_packet(0x08, 0x02, payload, error)) return false;

  Packet packet;
  for (int attempt = 0; attempt < 64; ++attempt) {
    if (!read_packet(packet, kHandshakeTimeoutMs, error)) return false;
    if (packet.type != 0) break;
  }
  if (packet.type != kTypeSuback) {
    error = "mqtt: expected SUBACK, got packet type " +
            std::to_string(packet.type);
    return false;
  }
  return true;
}

bool MqttClient::publish(const std::string& topic, const std::string& payload,
                         bool retain, std::string& error) {
  if (!connected_) {
    error = "mqtt: not connected";
    return false;
  }
  std::string variable;
  append_string(variable, topic);
  variable.append(payload);
  return send_packet(kTypePublish, retain ? 0x01 : 0x00, variable, error);
}

bool MqttClient::poll(int timeout_ms, std::string& error) {
  if (!connected_) {
    error = "mqtt: not connected";
    return false;
  }

  Packet packet;
  if (!read_packet(packet, timeout_ms, error)) return false;
  if (packet.type == 0) return true;  // nothing arrived this round

  switch (packet.type) {
    case kTypePublish:
      if (handler_) handler_(packet.topic, packet.payload);
      break;
    case kTypePingreq: {
      std::string ignored;
      send_packet(kTypePingresp, 0x00, std::string{}, ignored);
      break;
    }
    case kTypePingresp:
      break;
    case kTypeDisconnect:
      connected_ = false;
      break;
    default:
      break;
  }
  return true;
}

void MqttClient::disconnect() {
  if (socket_ != nullptr) {
    if (connected_) {
      std::string ignored;
      send_packet(kTypeDisconnect, 0x00, std::string{}, ignored);
    }
    auto* socket = static_cast<TlsSocket*>(socket_);
    socket->close();
    delete socket;
    socket_ = nullptr;
  }
  connected_ = false;
  buffer_.clear();
}

}  // namespace apex::net
