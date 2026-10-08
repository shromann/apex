#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Minimal JSON support for the Webull adapter.
//
// Builder: produces compact JSON with keys in insertion order (no spaces), so
// the exact bytes can be MD5-hashed for request signing and sent as the body.
// Value:   a small recursive-descent parser for mapping API responses.
namespace apex::json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;

  static Value parse(std::string_view text, std::string& error);

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  double number_or(double fallback) const;
  std::string string_or(std::string fallback = {}) const;
  bool bool_or(bool fallback) const;

  // Object member lookup; nullptr if absent or not an object.
  const Value* find(std::string_view key) const;

  const std::vector<Value>& array() const { return array_; }
  const std::vector<std::pair<std::string, Value>>& members() const {
    return object_;
  }

  // Convenience accessors used by the response mappers.
  std::string find_string(std::string_view key,
                          std::string fallback = {}) const;
  double find_number(std::string_view key, double fallback = 0.0) const;

 private:
  Type type_{Type::Null};
  bool bool_{false};
  double number_{0.0};
  std::string string_;
  std::vector<Value> array_;
  std::vector<std::pair<std::string, Value>> object_;
};

// Ordered, compact JSON object builder: {"a":1,"b":"x"} with no whitespace.
class Builder {
 public:
  Builder& set(std::string key, std::string value);
  Builder& set(std::string key, const char* value);
  Builder& set(std::string key, double value);
  Builder& set(std::string key, bool value);
  Builder& set_raw(std::string key, std::string raw_json);
  Builder& set_array(std::string key, std::vector<std::string> raw_items);

  std::string str() const;

 private:
  std::vector<std::pair<std::string, std::string>> items_;
};

// Escapes a string as a JSON string literal (including surrounding quotes).
std::string escape(std::string_view value);

}  // namespace apex::json
