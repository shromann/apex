#include "util/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace apex::json {
namespace {

std::string format_number(double value) {
  if (!std::isfinite(value)) return "0";
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    return std::to_string(static_cast<long long>(value));
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.10g", value);
  return buffer;
}

}  // namespace

Value Value::parse(std::string_view text, std::string& error) {
  std::size_t pos = 0;
  error.clear();

  auto fail = [&](const std::string& message) {
    if (error.empty()) {
      error = message + " at offset " + std::to_string(pos);
    }
  };

  auto skip_ws = [&]() {
    while (pos < text.size()) {
      const char c = text[pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos;
      } else {
        break;
      }
    }
  };

  auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };

  auto append_utf8 = [](std::string& out, unsigned int codepoint) {
    if (codepoint <= 0x7F) {
      out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
  };

  auto parse_string = [&]() -> std::string {
    std::string out;
    ++pos;  // consume opening quote
    while (pos < text.size()) {
      const char c = text[pos++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos >= text.size()) {
        fail("unterminated escape");
        return out;
      }
      const char esc = text[pos++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (pos + 4 > text.size()) {
            fail("invalid \\u escape");
            return out;
          }
          unsigned int codepoint = 0;
          for (int i = 0; i < 4; ++i) {
            const int digit = hex_value(text[pos++]);
            if (digit < 0) {
              fail("invalid hex in \\u escape");
              return out;
            }
            codepoint = (codepoint << 4) | static_cast<unsigned int>(digit);
          }
          if (codepoint >= 0xD800 && codepoint <= 0xDBFF &&
              pos + 6 <= text.size() && text[pos] == '\\' &&
              text[pos + 1] == 'u') {
            unsigned int low = 0;
            bool valid = true;
            for (int i = 0; i < 4; ++i) {
              const int digit = hex_value(text[pos + 2 + i]);
              if (digit < 0) {
                valid = false;
                break;
              }
              low = (low << 4) | static_cast<unsigned int>(digit);
            }
            if (valid && low >= 0xDC00 && low <= 0xDFFF) {
              pos += 6;
              codepoint =
                  0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
            }
          }
          append_utf8(out, codepoint);
          break;
        }
        default:
          fail("invalid escape sequence");
          return out;
      }
    }
    fail("unterminated string");
    return out;
  };

  auto parse_number = [&]() -> double {
    const std::size_t start = pos;
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    if (pos < text.size() && text[pos] == '.') {
      ++pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
      ++pos;
      if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    }
    const std::string slice(text.substr(start, pos - start));
    return std::strtod(slice.c_str(), nullptr);
  };

  auto match_literal = [&](std::string_view literal) -> bool {
    if (text.compare(pos, literal.size(), literal) != 0) return false;
    pos += literal.size();
    return true;
  };

  std::function<Value()> parse_value;
  parse_value = [&]() -> Value {
    skip_ws();
    if (pos >= text.size()) {
      fail("unexpected end of input");
      return Value{};
    }
    const char c = text[pos];

    if (c == '{') {
      ++pos;
      Value object;
      object.type_ = Type::Object;
      skip_ws();
      if (pos < text.size() && text[pos] == '}') {
        ++pos;
        return object;
      }
      while (true) {
        skip_ws();
        if (pos >= text.size() || text[pos] != '"') {
          fail("expected object key");
          return Value{};
        }
        std::string key = parse_string();
        if (!error.empty()) return Value{};
        skip_ws();
        if (pos >= text.size() || text[pos] != ':') {
          fail("expected ':'");
          return Value{};
        }
        ++pos;
        Value value = parse_value();
        if (!error.empty()) return Value{};
        object.object_.emplace_back(std::move(key), std::move(value));
        skip_ws();
        if (pos < text.size() && text[pos] == ',') {
          ++pos;
          continue;
        }
        if (pos < text.size() && text[pos] == '}') {
          ++pos;
          break;
        }
        fail("expected ',' or '}'");
        return Value{};
      }
      return object;
    }

    if (c == '[') {
      ++pos;
      Value array;
      array.type_ = Type::Array;
      skip_ws();
      if (pos < text.size() && text[pos] == ']') {
        ++pos;
        return array;
      }
      while (true) {
        Value value = parse_value();
        if (!error.empty()) return Value{};
        array.array_.push_back(std::move(value));
        skip_ws();
        if (pos < text.size() && text[pos] == ',') {
          ++pos;
          continue;
        }
        if (pos < text.size() && text[pos] == ']') {
          ++pos;
          break;
        }
        fail("expected ',' or ']'");
        return Value{};
      }
      return array;
    }

    if (c == '"') {
      Value string;
      string.type_ = Type::String;
      string.string_ = parse_string();
      return string;
    }

    if (c == 't' && match_literal("true")) {
      Value boolean;
      boolean.type_ = Type::Bool;
      boolean.bool_ = true;
      return boolean;
    }
    if (c == 'f' && match_literal("false")) {
      Value boolean;
      boolean.type_ = Type::Bool;
      boolean.bool_ = false;
      return boolean;
    }
    if (c == 'n' && match_literal("null")) {
      return Value{};
    }

    if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
      Value number;
      number.type_ = Type::Number;
      number.number_ = parse_number();
      return number;
    }

    fail("unexpected token");
    return Value{};
  };

  Value result = parse_value();
  if (!error.empty()) return Value{};
  skip_ws();
  if (pos != text.size()) {
    fail("trailing characters after JSON value");
    return Value{};
  }
  return result;
}

double Value::number_or(double fallback) const {
  return type_ == Type::Number ? number_ : fallback;
}

std::string Value::string_or(std::string fallback) const {
  return type_ == Type::String ? string_ : fallback;
}

bool Value::bool_or(bool fallback) const {
  return type_ == Type::Bool ? bool_ : fallback;
}

const Value* Value::find(std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto& [name, value] : object_) {
    if (name == key) return &value;
  }
  return nullptr;
}

std::string Value::find_string(std::string_view key, std::string fallback) const {
  const Value* value = find(key);
  return value != nullptr ? value->string_or(std::move(fallback))
                          : std::move(fallback);
}

double Value::find_number(std::string_view key, double fallback) const {
  const Value* value = find(key);
  return value != nullptr ? value->number_or(fallback) : fallback;
}

Builder& Builder::set(std::string key, std::string value) {
  items_.emplace_back(std::move(key), escape(value));
  return *this;
}

Builder& Builder::set(std::string key, const char* value) {
  items_.emplace_back(std::move(key), escape(value));
  return *this;
}

Builder& Builder::set(std::string key, double value) {
  items_.emplace_back(std::move(key), format_number(value));
  return *this;
}

Builder& Builder::set(std::string key, bool value) {
  items_.emplace_back(std::move(key), value ? "true" : "false");
  return *this;
}

Builder& Builder::set_raw(std::string key, std::string raw_json) {
  items_.emplace_back(std::move(key), std::move(raw_json));
  return *this;
}

Builder& Builder::set_array(std::string key, std::vector<std::string> raw_items) {
  std::string array = "[";
  for (std::size_t i = 0; i < raw_items.size(); ++i) {
    if (i != 0) array.push_back(',');
    array += raw_items[i];
  }
  array.push_back(']');
  items_.emplace_back(std::move(key), std::move(array));
  return *this;
}

std::string Builder::str() const {
  std::string out = "{";
  bool first = true;
  for (const auto& [key, value] : items_) {
    if (!first) out.push_back(',');
    first = false;
    out += escape(key);
    out.push_back(':');
    out += value;
  }
  out.push_back('}');
  return out;
}

std::string escape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace apex::json
