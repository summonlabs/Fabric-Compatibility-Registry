#include "fcr/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace fcr {
namespace {

void AppendUtf8(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7f) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
}

void DumpString(std::string& out, std::string_view text) {
  out.push_back('"');
  for (char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out.append("\\\""); continue;
      case '\\': out.append("\\\\"); continue;
      case '\b': out.append("\\b"); continue;
      case '\f': out.append("\\f"); continue;
      case '\n': out.append("\\n"); continue;
      case '\r': out.append("\\r"); continue;
      case '\t': out.append("\\t"); continue;
      default: break;
    }
    if (u < 0x20) {
      static const char* kHex = "0123456789abcdef";
      out.append("\\u00");
      out.push_back(kHex[(u >> 4) & 0x0f]);
      out.push_back(kHex[u & 0x0f]);
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
}

void DumpDouble(std::string& out, double value) {
  if (value == 0.0) {
    out.append("0.0");
    return;
  }
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc{}) {
    out.append("0.0");
    return;
  }
  std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
  // Keep the value recognisable as a floating point literal.
  if (text.find('.') == std::string_view::npos && text.find('e') == std::string_view::npos &&
      text.find('E') == std::string_view::npos && text.find("inf") == std::string_view::npos &&
      text.find("nan") == std::string_view::npos) {
    out.append(text);
    out.append(".0");
  } else {
    out.append(text);
  }
}

class Parser {
 public:
  Parser(std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {}

  Result<JsonValue> Run() {
    SkipWhitespace();
    auto value = ParseValue(0);
    if (!value.has_value()) return value.error();
    SkipWhitespace();
    if (position_ != text_.size()) {
      return MakeError(ErrorCode::InvalidJson, "trailing content after JSON value",
                       OffsetContext());
    }
    return value;
  }

 private:
  std::string OffsetContext() const {
    return "byte offset " + std::to_string(position_);
  }

  Error Fail(ErrorCode code, std::string message) const {
    return MakeError(code, std::move(message), OffsetContext());
  }

  void SkipWhitespace() {
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++position_;
      } else {
        break;
      }
    }
  }

  Result<std::uint32_t> ParseHex4() {
    if (position_ + 4 > text_.size()) return Fail(ErrorCode::InvalidJson, "truncated \\u escape");
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[position_ + static_cast<std::size_t>(i)];
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return Fail(ErrorCode::InvalidJson, "illegal hex digit in \\u escape");
      }
      value = value * 16u + digit;
    }
    position_ += 4;
    return value;
  }

  Result<std::string> ParseString() {
    if (position_ >= text_.size() || text_[position_] != '"') {
      return Fail(ErrorCode::InvalidJson, "expected string");
    }
    ++position_;
    std::string out;
    while (true) {
      if (position_ >= text_.size()) {
        return Fail(ErrorCode::InvalidJson, "unterminated string");
      }
      const char c = text_[position_];
      if (c == '"') {
        ++position_;
        return out;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return Fail(ErrorCode::InvalidJson, "unescaped control character in string");
      }
      if (c == '\\') {
        ++position_;
        if (position_ >= text_.size()) {
          return Fail(ErrorCode::InvalidJson, "truncated escape sequence");
        }
        const char esc = text_[position_++];
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
            auto high = ParseHex4();
            if (!high.has_value()) return high.error();
            std::uint32_t code_point = high.value();
            if (code_point >= 0xd800 && code_point <= 0xdbff) {
              if (position_ + 1 >= text_.size() || text_[position_] != '\\' ||
                  text_[position_ + 1] != 'u') {
                return Fail(ErrorCode::InvalidJson, "unpaired high surrogate in \\u escape");
              }
              position_ += 2;
              auto low = ParseHex4();
              if (!low.has_value()) return low.error();
              if (low.value() < 0xdc00 || low.value() > 0xdfff) {
                return Fail(ErrorCode::InvalidJson, "invalid low surrogate in \\u escape");
              }
              code_point =
                  0x10000u + ((code_point - 0xd800u) << 10) + (low.value() - 0xdc00u);
            } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
              return Fail(ErrorCode::InvalidJson, "unpaired low surrogate in \\u escape");
            }
            AppendUtf8(out, code_point);
            break;
          }
          default:
            return Fail(ErrorCode::InvalidJson, "illegal escape sequence");
        }
      } else {
        out.push_back(c);
        ++position_;
      }
      if (out.size() > limits_.max_string_bytes) {
        return Fail(ErrorCode::LimitExceeded, "string exceeds configured size limit");
      }
    }
  }

  Result<JsonValue> ParseNumber() {
    const std::size_t start = position_;
    bool negative = false;
    if (position_ < text_.size() && text_[position_] == '-') {
      negative = true;
      ++position_;
    }
    const std::size_t digits_start = position_;
    if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
      return Fail(ErrorCode::InvalidJson, "malformed number");
    }
    if (text_[position_] == '0') {
      ++position_;
      if (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        return Fail(ErrorCode::InvalidJson, "number has a leading zero");
      }
    } else {
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    }
    const bool integer_only = position_ >= text_.size() ||
                              (text_[position_] != '.' && text_[position_] != 'e' &&
                               text_[position_] != 'E');
    if (integer_only) {
      const std::string_view digits = text_.substr(digits_start, position_ - digits_start);
      if (!negative) {
        std::uint64_t value = 0;
        const auto conv = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (conv.ec == std::errc::result_out_of_range) {
          return Fail(ErrorCode::InvalidJson, "integer literal exceeds 64-bit unsigned range");
        }
        if (conv.ec != std::errc{}) {
          return Fail(ErrorCode::InvalidJson, "malformed integer literal");
        }
        return JsonValue::UInt(value);
      }
      std::int64_t value = 0;
      const auto conv = std::from_chars(digits.data(), digits.data() + digits.size(), value);
      if (conv.ec == std::errc::result_out_of_range) {
        return Fail(ErrorCode::InvalidJson, "integer literal exceeds 64-bit signed range");
      }
      if (conv.ec != std::errc{}) {
        return Fail(ErrorCode::InvalidJson, "malformed integer literal");
      }
      return JsonValue::Int(-value);
    }
    if (position_ < text_.size() && text_[position_] == '.') {
      ++position_;
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
        return Fail(ErrorCode::InvalidJson, "fraction has no digits");
      }
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
        ++position_;
      }
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
        return Fail(ErrorCode::InvalidJson, "exponent has no digits");
      }
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    }
    const std::string owned(text_.substr(start, position_ - start));
    double value = 0.0;
    const auto conv = std::from_chars(owned.data(), owned.data() + owned.size(), value);
    if (conv.ec != std::errc{} || !std::isfinite(value)) {
      return Fail(ErrorCode::InvalidJson, "floating point literal out of range");
    }
    return JsonValue::Double(value);
  }

  Result<JsonValue> ParseValue(int depth) {
    if (depth > static_cast<int>(limits_.max_depth)) {
      return Fail(ErrorCode::LimitExceeded, "JSON nesting exceeds configured depth limit");
    }
    if (position_ >= text_.size()) {
      return Fail(ErrorCode::InvalidJson, "unexpected end of input");
    }
    const char c = text_[position_];
    switch (c) {
      case '{': return ParseObject(depth);
      case '[': return ParseArray(depth);
      case '"': {
        auto text = ParseString();
        if (!text.has_value()) return text.error();
        return JsonValue::Str(std::move(text).value());
      }
      case 't':
        if (text_.substr(position_, 4) == "true") {
          position_ += 4;
          return JsonValue::Bool(true);
        }
        return Fail(ErrorCode::InvalidJson, "malformed literal");
      case 'f':
        if (text_.substr(position_, 5) == "false") {
          position_ += 5;
          return JsonValue::Bool(false);
        }
        return Fail(ErrorCode::InvalidJson, "malformed literal");
      case 'n':
        if (text_.substr(position_, 4) == "null") {
          position_ += 4;
          return JsonValue::Null();
        }
        return Fail(ErrorCode::InvalidJson, "malformed literal");
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber();
        return Fail(ErrorCode::InvalidJson, "unexpected character");
    }
  }

  Result<JsonValue> ParseArray(int depth) {
    ++position_;  // '['
    JsonValue array = JsonValue::Arr();
    SkipWhitespace();
    if (position_ < text_.size() && text_[position_] == ']') {
      ++position_;
      return array;
    }
    while (true) {
      SkipWhitespace();
      auto element = ParseValue(depth + 1);
      if (!element.has_value()) return element.error();
      array.Push(std::move(element).value());
      if (array.size() > limits_.max_elements) {
        return Fail(ErrorCode::LimitExceeded, "array exceeds configured element limit");
      }
      SkipWhitespace();
      if (position_ >= text_.size()) {
        return Fail(ErrorCode::InvalidJson, "unterminated array");
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == ']') {
        ++position_;
        return array;
      }
      return Fail(ErrorCode::InvalidJson, "expected ',' or ']' in array");
    }
  }

  Result<JsonValue> ParseObject(int depth) {
    ++position_;  // '{'
    JsonValue object = JsonValue::Obj();
    SkipWhitespace();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      return object;
    }
    while (true) {
      SkipWhitespace();
      auto key = ParseString();
      if (!key.has_value()) return key.error();
      SkipWhitespace();
      if (position_ >= text_.size() || text_[position_] != ':') {
        return Fail(ErrorCode::InvalidJson, "expected ':' after object key");
      }
      ++position_;
      SkipWhitespace();
      auto value = ParseValue(depth + 1);
      if (!value.has_value()) return value.error();
      if (object.Has(key.value())) {
        return Fail(ErrorCode::InvalidJson,
                     "duplicate object key '" + key.value() + "'");
      }
      object.Set(std::move(key).value(), std::move(value).value());
      if (object.size() > limits_.max_elements) {
        return Fail(ErrorCode::LimitExceeded, "object exceeds configured member limit");
      }
      SkipWhitespace();
      if (position_ >= text_.size()) {
        return Fail(ErrorCode::InvalidJson, "unterminated object");
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == '}') {
        ++position_;
        return object;
      }
      return Fail(ErrorCode::InvalidJson, "expected ',' or '}' in object");
    }
  }

  std::string_view text_;
  JsonLimits limits_;
  std::size_t position_ = 0;
};

}  // namespace

JsonValue JsonValue::Bool(bool v) {
  JsonValue out;
  out.value_ = v;
  return out;
}
JsonValue JsonValue::Int(std::int64_t v) {
  JsonValue out;
  out.value_ = v;
  return out;
}
JsonValue JsonValue::UInt(std::uint64_t v) {
  JsonValue out;
  out.value_ = v;
  return out;
}
JsonValue JsonValue::Double(double v) {
  JsonValue out;
  out.value_ = v;
  return out;
}
JsonValue JsonValue::Str(std::string v) {
  JsonValue out;
  out.value_ = std::move(v);
  return out;
}
JsonValue JsonValue::Str(std::string_view v) {
  return Str(std::string(v));
}
JsonValue JsonValue::Arr() {
  JsonValue out;
  out.value_ = Array{};
  return out;
}
JsonValue JsonValue::Arr(Array items) {
  JsonValue out;
  out.value_ = std::move(items);
  return out;
}
JsonValue JsonValue::Obj() {
  JsonValue out;
  out.value_ = JsonMemberList{};
  return out;
}

JsonValue::Type JsonValue::type() const noexcept {
  switch (value_.index()) {
    case 0: return Type::Null;
    case 1: return Type::Bool;
    case 2: return Type::Int;
    case 3: return Type::UInt;
    case 4: return Type::Double;
    case 5: return Type::String;
    case 6: return Type::Array;
    default: return Type::Object;
  }
}

bool JsonValue::IsNumber() const noexcept {
  const Type t = type();
  return t == Type::Int || t == Type::UInt || t == Type::Double;
}

bool JsonValue::AsBool(bool fallback) const noexcept {
  if (const bool* v = std::get_if<bool>(&value_)) return *v;
  return fallback;
}

std::optional<std::int64_t> JsonValue::AsInt() const noexcept {
  if (const std::int64_t* v = std::get_if<std::int64_t>(&value_)) return *v;
  if (const std::uint64_t* v = std::get_if<std::uint64_t>(&value_)) {
    if (*v <= static_cast<std::uint64_t>(INT64_MAX)) return static_cast<std::int64_t>(*v);
  }
  if (const double* v = std::get_if<double>(&value_)) {
    if (std::isfinite(*v) && *v >= -9223372036854775808.0 && *v < 9223372036854775808.0) {
      return static_cast<std::int64_t>(*v);
    }
  }
  return std::nullopt;
}

std::optional<std::uint64_t> JsonValue::AsUInt() const noexcept {
  if (const std::uint64_t* v = std::get_if<std::uint64_t>(&value_)) return *v;
  if (const std::int64_t* v = std::get_if<std::int64_t>(&value_)) {
    if (*v >= 0) return static_cast<std::uint64_t>(*v);
  }
  return std::nullopt;
}

std::optional<double> JsonValue::AsDouble() const noexcept {
  if (const double* v = std::get_if<double>(&value_)) return *v;
  if (const std::int64_t* v = std::get_if<std::int64_t>(&value_)) return static_cast<double>(*v);
  if (const std::uint64_t* v = std::get_if<std::uint64_t>(&value_)) return static_cast<double>(*v);
  return std::nullopt;
}

const std::string* JsonValue::AsString() const noexcept { return std::get_if<std::string>(&value_); }

const JsonValue::Array* JsonValue::AsArray() const noexcept {
  return std::get_if<Array>(&value_);
}

std::size_t JsonValue::size() const noexcept {
  if (const Array* a = std::get_if<Array>(&value_)) return a->size();
  if (const JsonMemberList* o = std::get_if<JsonMemberList>(&value_)) return o->keys.size();
  return 0;
}

void JsonValue::Push(JsonValue value) {
  Array* a = std::get_if<Array>(&value_);
  if (a == nullptr) {
    value_ = Array{};
    a = std::get_if<Array>(&value_);
  }
  a->push_back(std::move(value));
}

const JsonValue* JsonValue::At(std::size_t index) const noexcept {
  const Array* a = std::get_if<Array>(&value_);
  if (a == nullptr || index >= a->size()) return nullptr;
  return &(*a)[index];
}

void JsonValue::Set(std::string key, JsonValue value) {
  JsonMemberList* o = std::get_if<JsonMemberList>(&value_);
  if (o == nullptr) {
    value_ = JsonMemberList{};
    o = std::get_if<JsonMemberList>(&value_);
  }
  const auto it = std::lower_bound(o->keys.begin(), o->keys.end(), key);
  const std::size_t index = static_cast<std::size_t>(it - o->keys.begin());
  if (it != o->keys.end() && *it == key) {
    o->values[index] = std::move(value);
    return;
  }
  o->keys.insert(it, std::move(key));
  o->values.insert(o->values.begin() + static_cast<std::ptrdiff_t>(index), std::move(value));
}

bool JsonValue::Has(std::string_view key) const noexcept { return Find(key) != nullptr; }

const JsonValue* JsonValue::Find(std::string_view key) const noexcept {
  const JsonMemberList* o = std::get_if<JsonMemberList>(&value_);
  if (o == nullptr) return nullptr;
  const auto it = std::lower_bound(o->keys.begin(), o->keys.end(), key);
  if (it == o->keys.end() || *it != key) return nullptr;
  return &o->values[static_cast<std::size_t>(it - o->keys.begin())];
}

const std::vector<std::string>& JsonValue::keys() const noexcept {
  static const std::vector<std::string> kEmpty;
  const JsonMemberList* o = std::get_if<JsonMemberList>(&value_);
  return o == nullptr ? kEmpty : o->keys;
}

const std::vector<JsonValue>& JsonValue::values() const noexcept {
  static const std::vector<JsonValue> kEmpty;
  const JsonMemberList* o = std::get_if<JsonMemberList>(&value_);
  return o == nullptr ? kEmpty : o->values;
}

void JsonValue::DumpTo(std::string& out, bool pretty, int indent) const {
  const auto newline_indent = [&](int level) {
    if (!pretty) return;
    out.push_back('\n');
    out.append(static_cast<std::size_t>(level) * 2u, ' ');
  };
  switch (type()) {
    case Type::Null: out.append("null"); break;
    case Type::Bool: out.append(AsBool(false) ? "true" : "false"); break;
    case Type::Int: out.append(std::to_string(std::get<std::int64_t>(value_))); break;
    case Type::UInt: out.append(std::to_string(std::get<std::uint64_t>(value_))); break;
    case Type::Double: DumpDouble(out, std::get<double>(value_)); break;
    case Type::String: DumpString(out, std::get<std::string>(value_)); break;
    case Type::Array: {
      const Array& a = std::get<Array>(value_);
      if (a.empty()) {
        out.append("[]");
        return;
      }
      out.push_back('[');
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (i != 0) out.push_back(',');
        newline_indent(indent + 1);
        a[i].DumpTo(out, pretty, indent + 1);
      }
      newline_indent(indent);
      out.push_back(']');
      return;
    }
    case Type::Object: {
      const JsonMemberList& o = std::get<JsonMemberList>(value_);
      if (o.keys.empty()) {
        out.append("{}");
        return;
      }
      out.push_back('{');
      for (std::size_t i = 0; i < o.keys.size(); ++i) {
        if (i != 0) out.push_back(',');
        newline_indent(indent + 1);
        DumpString(out, o.keys[i]);
        out.push_back(':');
        if (pretty) out.push_back(' ');
        o.values[i].DumpTo(out, pretty, indent + 1);
      }
      newline_indent(indent);
      out.push_back('}');
      return;
    }
  }
}

std::string JsonValue::Dump(bool pretty) const {
  std::string out;
  DumpTo(out, pretty, 0);
  return out;
}

Result<JsonValue> ParseJson(std::string_view text, const JsonLimits& limits) {
  if (text.size() > limits.max_bytes) {
    return MakeError(ErrorCode::LimitExceeded, "JSON document exceeds configured byte limit");
  }
  Parser parser(text, limits);
  return parser.Run();
}

std::string_view JsonTypeName(JsonValue::Type type) {
  switch (type) {
    case JsonValue::Type::Null: return "null";
    case JsonValue::Type::Bool: return "boolean";
    case JsonValue::Type::Int: return "integer";
    case JsonValue::Type::UInt: return "unsigned integer";
    case JsonValue::Type::Double: return "number";
    case JsonValue::Type::String: return "string";
    case JsonValue::Type::Array: return "array";
    case JsonValue::Type::Object: return "object";
  }
  return "unknown";
}

Result<const JsonValue*> RequireField(const JsonValue& object, std::string_view key,
                                      std::string_view context) {
  if (!object.IsObject()) {
    return MakeError(ErrorCode::InvalidJson,
                     std::string(context) + " must be a JSON object");
  }
  const JsonValue* value = object.Find(key);
  if (value == nullptr) {
    return MakeError(ErrorCode::InvalidJson,
                     std::string(context) + " is missing required field '" + std::string(key) + "'");
  }
  return value;
}

Result<std::string> RequireString(const JsonValue& object, std::string_view key,
                                  std::string_view context) {
  auto field = RequireField(object, key, context);
  if (!field.has_value()) return field.error();
  const std::string* text = field.value()->AsString();
  if (text == nullptr) {
    return MakeError(ErrorCode::InvalidJson,
                     std::string(context) + " field '" + std::string(key) + "' must be a string");
  }
  return *text;
}

Result<std::uint64_t> RequireUInt(const JsonValue& object, std::string_view key,
                                  std::string_view context) {
  auto field = RequireField(object, key, context);
  if (!field.has_value()) return field.error();
  auto value = field.value()->AsUInt();
  if (!value.has_value()) {
    return MakeError(ErrorCode::InvalidJson,
                     std::string(context) + " field '" + std::string(key) +
                         "' must be a non-negative integer");
  }
  return value.value();
}

Result<std::int64_t> RequireInt(const JsonValue& object, std::string_view key,
                                std::string_view context) {
  auto field = RequireField(object, key, context);
  if (!field.has_value()) return field.error();
  auto value = field.value()->AsInt();
  if (!value.has_value()) {
    return MakeError(ErrorCode::InvalidJson,
                     std::string(context) + " field '" + std::string(key) + "' must be an integer");
  }
  return value.value();
}

Result<bool> RequireBool(const JsonValue& object, std::string_view key, std::string_view context) {
  auto field = RequireField(object, key, context);
  if (!field.has_value()) return field.error();
  if (!field.value()->IsBool()) {
    return MakeError(ErrorCode::InvalidJson,
                     std::string(context) + " field '" + std::string(key) + "' must be a boolean");
  }
  return field.value()->AsBool(false);
}

Result<const JsonValue::Array*> RequireArray(const JsonValue& object, std::string_view key,
                                             std::string_view context) {
  auto field = RequireField(object, key, context);
  if (!field.has_value()) return field.error();
  const JsonValue::Array* array = field.value()->AsArray();
  if (array == nullptr) {
    return MakeError(ErrorCode::InvalidJson,
                     std::string(context) + " field '" + std::string(key) + "' must be an array");
  }
  return array;
}

Result<const JsonValue*> OptionalField(const JsonValue& object, std::string_view key) {
  if (!object.IsObject()) {
    return MakeError(ErrorCode::InvalidJson, "expected a JSON object");
  }
  const JsonValue* value = object.Find(key);
  if (value == nullptr || value->IsNull()) {
    return MakeError(ErrorCode::NotFound, "optional field not present");
  }
  return value;
}

Result<std::string> OptionalString(const JsonValue& object, std::string_view key,
                                   std::string_view fallback) {
  const JsonValue* value = object.Find(key);
  if (value == nullptr || value->IsNull()) return std::string(fallback);
  const std::string* text = value->AsString();
  if (text == nullptr) {
    return MakeError(ErrorCode::InvalidJson,
                     "field '" + std::string(key) + "' must be a string");
  }
  return *text;
}

}  // namespace fcr
