#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace apex {

// --- Fundamental value types ------------------------------------------------

using Symbol = std::string;
using Price = double;
using Quantity = std::int64_t;
using OrderId = std::string;
using ClientOrderId = std::string;

// Nanoseconds since the Unix epoch.
using Timestamp = std::chrono::nanoseconds;

inline Timestamp now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch());
}

inline double seconds(Timestamp ts) {
  return std::chrono::duration<double>(ts).count();
}

// --- Enumerations -----------------------------------------------------------

enum class Side { Buy, Sell };

enum class OrderType { Market, Limit, Stop, StopLimit };

enum class TimeInForce { Day, GoodTillCancel, ImmediateOrCancel, FillOrKill };

enum class OrderStatus {
  New,
  Accepted,
  PartiallyFilled,
  Filled,
  Cancelled,
  Rejected,
  Expired,
};

inline std::string_view to_string(Side v) {
  switch (v) {
    case Side::Buy:
      return "Buy";
    case Side::Sell:
      return "Sell";
  }
  return "Unknown";
}

inline std::string_view to_string(OrderType v) {
  switch (v) {
    case OrderType::Market:
      return "Market";
    case OrderType::Limit:
      return "Limit";
    case OrderType::Stop:
      return "Stop";
    case OrderType::StopLimit:
      return "StopLimit";
  }
  return "Unknown";
}

inline std::string_view to_string(TimeInForce v) {
  switch (v) {
    case TimeInForce::Day:
      return "Day";
    case TimeInForce::GoodTillCancel:
      return "GTC";
    case TimeInForce::ImmediateOrCancel:
      return "IOC";
    case TimeInForce::FillOrKill:
      return "FOK";
  }
  return "Unknown";
}

inline std::string_view to_string(OrderStatus v) {
  switch (v) {
    case OrderStatus::New:
      return "New";
    case OrderStatus::Accepted:
      return "Accepted";
    case OrderStatus::PartiallyFilled:
      return "PartiallyFilled";
    case OrderStatus::Filled:
      return "Filled";
    case OrderStatus::Cancelled:
      return "Cancelled";
    case OrderStatus::Rejected:
      return "Rejected";
    case OrderStatus::Expired:
      return "Expired";
  }
  return "Unknown";
}

inline Side opposite(Side side) {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

}  // namespace apex
