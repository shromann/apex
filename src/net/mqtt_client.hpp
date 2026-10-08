#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Minimal MQTT 3.1.1 client over TLS, sufficient for Webull's market-data
// stream: CONNECT, SUBSCRIBE, PUBLISH (in/out), PINGREQ, DISCONNECT.
// QoS 0 only, which is what the streaming feed uses.
namespace apex::net {

class MqttClient {
 public:
  using MessageHandler =
      std::function<void(const std::string& topic, const std::string& payload)>;

  MqttClient();
  ~MqttClient();

  MqttClient(const MqttClient&) = delete;
  MqttClient& operator=(const MqttClient&) = delete;

  void set_message_handler(MessageHandler handler);

  bool connect(const std::string& host, std::uint16_t port,
               const std::string& client_id, const std::string& username,
               const std::string& password, int keep_alive_seconds,
               int timeout_ms, std::string& error);

  bool subscribe(const std::vector<std::string>& topics, std::string& error);
  bool publish(const std::string& topic, const std::string& payload,
               bool retain, std::string& error);

  // Reads and dispatches one incoming control packet. Blocks up to timeout_ms
  // waiting for the socket. Dispatches PUBLISH payloads to the message handler.
  bool poll(int timeout_ms, std::string& error);

  void disconnect();
  bool is_connected() const { return connected_; }

  // --- Test seam -------------------------------------------------------------
  // Parses a single MQTT control packet. Exposed so packet encoding/decoding
  // can be verified without a broker.
  struct Packet {
    std::uint8_t type{0};
    std::uint8_t flags{0};
    std::string payload;
    std::uint16_t packet_id{0};
    std::string topic;
  };
  static bool encode_length(std::size_t length, std::string& out);
  static bool decode_length(std::string_view buffer, std::size_t& length,
                            std::size_t& consumed);
  static bool parse_packet(std::string_view buffer, Packet& packet,
                           std::size_t& consumed);

 private:
  bool send_packet(std::uint8_t type, std::uint8_t flags,
                   const std::string& payload, std::string& error);
  bool read_packet(Packet& packet, int timeout_ms, std::string& error);
  bool wait_readable(int timeout_ms, std::string& error);

  void* socket_{nullptr};  // net::TlsSocket*
  MessageHandler handler_;
  bool connected_{false};
  std::uint16_t next_packet_id_{1};
  std::string buffer_;
};

}  // namespace apex::net
