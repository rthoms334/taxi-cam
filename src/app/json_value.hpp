#pragma once
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace taxi_camera::standalone::json {
// Small strict JSON reader for bounded documents (release metadata and
// installation records). Rejects duplicate keys, invalid UTF-8 and excess depth.
struct Value {
  enum class Kind { Null, Bool, Number, String, Array, Object };
  Kind kind{Kind::Null};
  bool boolean{};
  double number{};
  std::string text;  // String contents, or the exact number token.
  std::vector<Value> items;
  std::vector<std::pair<std::string, Value>> members;

  bool is_null() const { return kind == Kind::Null; }
  bool is_bool() const { return kind == Kind::Bool; }
  bool is_number() const { return kind == Kind::Number; }
  bool is_string() const { return kind == Kind::String; }
  bool is_array() const { return kind == Kind::Array; }
  bool is_object() const { return kind == Kind::Object; }
  // Returns nullptr when this is not an object or the member is absent.
  const Value* find(std::string_view key) const {
    if (kind != Kind::Object)
      return nullptr;
    for (const auto& [name, value] : members)
      if (name == key)
        return &value;
    return nullptr;
  }
  // An integral number within [minimum, maximum], as JSON writers emit sizes and counts.
  bool integer(double minimum, double maximum, std::int64_t& out) const {
    if (kind != Kind::Number || !std::isfinite(number) || number != std::trunc(number) || number < minimum || number > maximum)
      return false;
    out = static_cast<std::int64_t>(number);
    return true;
  }
};

namespace detail {
struct Parser {
  std::string_view text;
  std::size_t at{};
  unsigned depth{};
  static constexpr unsigned MaxDepth = 32;

  void space() {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
      ++at;
  }
  bool literal(std::string_view word) {
    if (text.substr(at, word.size()) != word)
      return false;
    at += word.size();
    return true;
  }
  bool hex4(std::uint32_t& value) {
    value = 0;
    for (int i = 0; i < 4; ++i) {
      if (at >= text.size())
        return false;
      const char c = text[at++];
      value <<= 4;
      if (c >= '0' && c <= '9')
        value |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        value |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        value |= static_cast<std::uint32_t>(c - 'A' + 10);
      else
        return false;
    }
    return true;
  }
  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xc0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xe0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    }
  }
  bool string(std::string& out) {
    if (at >= text.size() || text[at] != '"')
      return false;
    ++at;
    out.clear();
    while (at < text.size()) {
      const auto c = static_cast<unsigned char>(text[at++]);
      if (c == '"')
        return out.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, out.data(), static_cast<int>(out.size()), nullptr, 0) > 0;
      if (c < 0x20)
        return false;
      if (c != '\\') {
        out += static_cast<char>(c);
        continue;
      }
      if (at >= text.size())
        return false;
      const char e = text[at++];
      std::uint32_t cp{};
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out += e;
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u':
          if (!hex4(cp))
            return false;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            std::uint32_t low{};
            if (!literal("\\u") || !hex4(low) || low < 0xdc00 || low > 0xdfff)
              return false;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
          } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return false;
          }
          append_utf8(out, cp);
          break;
        default:
          return false;
      }
    }
    return false;
  }
  bool digits() {
    const auto start = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9')
      ++at;
    return at > start;
  }
  bool number(Value& out) {
    const auto start = at;
    if (at < text.size() && text[at] == '-')
      ++at;
    if (at < text.size() && text[at] == '0')
      ++at;
    else if (!digits())
      return false;
    if (at < text.size() && text[at] == '.') {
      ++at;
      if (!digits())
        return false;
    }
    if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
      ++at;
      if (at < text.size() && (text[at] == '+' || text[at] == '-'))
        ++at;
      if (!digits())
        return false;
    }
    out.kind = Value::Kind::Number;
    out.text.assign(text.substr(start, at - start));
    out.number = std::strtod(out.text.c_str(), nullptr);
    return std::isfinite(out.number);
  }
  bool value(Value& out) {
    space();
    if (at >= text.size())
      return false;
    const char c = text[at];
    if (c == '{' || c == '[') {
      if (++depth > MaxDepth)
        return false;
      ++at;
      const bool object = c == '{';
      out.kind = object ? Value::Kind::Object : Value::Kind::Array;
      space();
      if (at < text.size() && text[at] == (object ? '}' : ']')) {
        ++at;
        --depth;
        return true;
      }
      for (;;) {
        if (object) {
          std::string key;
          space();
          if (!string(key))
            return false;
          for (const auto& member : out.members)
            if (member.first == key)
              return false;
          space();
          if (at >= text.size() || text[at++] != ':')
            return false;
          Value item;
          if (!value(item))
            return false;
          out.members.emplace_back(std::move(key), std::move(item));
        } else {
          Value item;
          if (!value(item))
            return false;
          out.items.push_back(std::move(item));
        }
        space();
        if (at >= text.size())
          return false;
        const char next = text[at++];
        if (next == ',')
          continue;
        if (next != (object ? '}' : ']'))
          return false;
        --depth;
        return true;
      }
    }
    if (c == '"') {
      out.kind = Value::Kind::String;
      return string(out.text);
    }
    if (literal("true")) {
      out.kind = Value::Kind::Bool;
      out.boolean = true;
      return true;
    }
    if (literal("false")) {
      out.kind = Value::Kind::Bool;
      return true;
    }
    if (literal("null")) {
      out.kind = Value::Kind::Null;
      return true;
    }
    return number(out);
  }
};
}  // namespace detail

// Parses one complete document; a UTF-8 byte-order mark is accepted.
inline bool parse(std::string_view text, Value& out) {
  if (text.substr(0, 3) == "\xEF\xBB\xBF")
    text.remove_prefix(3);
  detail::Parser parser{text};
  Value parsed;
  if (!parser.value(parsed))
    return false;
  parser.space();
  if (parser.at != text.size())
    return false;
  out = std::move(parsed);
  return true;
}
}  // namespace taxi_camera::standalone::json
