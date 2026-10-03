#pragma once
#include <cstdint>
#include <string>
#include "../app/json_value.hpp"
#include "setup_common.hpp"

// Builds and writes the small JSON records Setup keeps beside the installation.
namespace taxi_camera::setup::json_out {
using standalone::json::Value;
inline Value text(const std::wstring& value) {
  Value result;
  result.kind = Value::Kind::String;
  result.text = narrow(value);
  return result;
}
inline Value text_or_null(const std::wstring& value) {
  return value.empty() ? Value{} : text(value);
}
inline Value boolean(bool value) {
  Value result;
  result.kind = Value::Kind::Bool;
  result.boolean = value;
  return result;
}
inline Value number(std::int64_t value) {
  Value result;
  result.kind = Value::Kind::Number;
  result.number = static_cast<double>(value);
  result.text = std::to_string(value);
  return result;
}
inline Value array() {
  Value result;
  result.kind = Value::Kind::Array;
  return result;
}
inline Value object() {
  Value result;
  result.kind = Value::Kind::Object;
  return result;
}
inline void set(Value& object, const char* key, Value value) {
  for (auto& member : object.members)
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  object.members.emplace_back(key, std::move(value));
}
inline void escape(std::string& out, const std::string& text) {
  out += '"';
  for (unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          constexpr char hex[] = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}
inline void write(std::string& out, const Value& value, unsigned depth) {
  const std::string indent(2 * (depth + 1), ' '), closing(2 * depth, ' ');
  switch (value.kind) {
    case Value::Kind::Null:
      out += "null";
      break;
    case Value::Kind::Bool:
      out += value.boolean ? "true" : "false";
      break;
    case Value::Kind::Number:
      out += value.text.empty() ? std::to_string(value.number) : value.text;
      break;
    case Value::Kind::String:
      escape(out, value.text);
      break;
    case Value::Kind::Array:
      if (value.items.empty()) {
        out += "[]";
        break;
      }
      out += "[\r\n";
      for (std::size_t i = 0; i < value.items.size(); ++i) {
        out += indent;
        write(out, value.items[i], depth + 1);
        out += i + 1 < value.items.size() ? ",\r\n" : "\r\n";
      }
      out += closing + "]";
      break;
    case Value::Kind::Object:
      if (value.members.empty()) {
        out += "{}";
        break;
      }
      out += "{\r\n";
      for (std::size_t i = 0; i < value.members.size(); ++i) {
        out += indent;
        escape(out, value.members[i].first);
        out += ": ";
        write(out, value.members[i].second, depth + 1);
        out += i + 1 < value.members.size() ? ",\r\n" : "\r\n";
      }
      out += closing + "}";
      break;
  }
}
inline std::string serialize(const Value& value) {
  std::string out;
  write(out, value, 0);
  return out + "\r\n";
}
}  // namespace taxi_camera::setup::json_out
