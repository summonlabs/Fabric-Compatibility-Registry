// Fabric Compatibility Registry - Summon Software Labs
// Strict, bounded, deterministic JSON (RFC 8259 subset with canonical output).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "fcr/error.hpp"

namespace fcr {

// Object members are kept sorted by key, so iteration is deterministic and the
// serialized form is canonical without a separate sorting pass. Duplicate keys
// are rejected at parse time.
struct JsonMemberList {
  std::vector<std::string> keys;
  std::vector<class JsonValue> values;
};

class JsonValue {
 public:
  using Array = std::vector<JsonValue>;

  enum class Type { Null, Bool, Int, UInt, Double, String, Array, Object };

  JsonValue() = default;
  static JsonValue Null() { return JsonValue(); }
  static JsonValue Bool(bool v);
  static JsonValue Int(std::int64_t v);
  static JsonValue UInt(std::uint64_t v);
  static JsonValue Double(double v);
  static JsonValue Str(std::string v);
  static JsonValue Str(std::string_view v);
  static JsonValue Str(const char* v) { return Str(std::string(v)); }
  static JsonValue Arr();
  static JsonValue Arr(Array items);
  static JsonValue Obj();

  Type type() const noexcept;
  bool IsNull() const noexcept { return type() == Type::Null; }
  bool IsBool() const noexcept { return type() == Type::Bool; }
  bool IsInt() const noexcept { return type() == Type::Int; }
  bool IsUInt() const noexcept { return type() == Type::UInt; }
  bool IsDouble() const noexcept { return type() == Type::Double; }
  bool IsNumber() const noexcept;
  bool IsString() const noexcept { return type() == Type::String; }
  bool IsArray() const noexcept { return type() == Type::Array; }
  bool IsObject() const noexcept { return type() == Type::Object; }

  bool AsBool(bool fallback) const noexcept;
  std::optional<std::int64_t> AsInt() const noexcept;
  std::optional<std::uint64_t> AsUInt() const noexcept;
  std::optional<double> AsDouble() const noexcept;
  const std::string* AsString() const noexcept;
  const Array* AsArray() const noexcept;
  std::size_t size() const noexcept;

  // Array access.
  void Push(JsonValue value);
  const JsonValue* At(std::size_t index) const noexcept;

  // Object access.
  void Set(std::string key, JsonValue value);
  bool Has(std::string_view key) const noexcept;
  const JsonValue* Find(std::string_view key) const noexcept;
  const std::vector<std::string>& keys() const noexcept;
  const std::vector<JsonValue>& values() const noexcept;

  // Canonical (compact, key-sorted) or pretty-printed serialization.
  std::string Dump(bool pretty = false) const;

 private:
  void DumpTo(std::string& out, bool pretty, int indent) const;

  std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, Array,
               JsonMemberList>
      value_;
};

struct JsonLimits {
  std::size_t max_bytes = 8u * 1024u * 1024u;
  std::size_t max_depth = 64;
  std::size_t max_string_bytes = 1u * 1024u * 1024u;
  std::size_t max_elements = 1u << 20;
};

// Strict parse. Rejects duplicate keys, trailing content, control characters in
// strings, out-of-range integers, non-finite numbers, and inputs beyond the
// configured limits.
Result<JsonValue> ParseJson(std::string_view text, const JsonLimits& limits = JsonLimits{});

// Typed field accessors that produce domain errors instead of throwing.
Result<const JsonValue*> RequireField(const JsonValue& object, std::string_view key,
                                      std::string_view context);
Result<std::string> RequireString(const JsonValue& object, std::string_view key,
                                  std::string_view context);
Result<std::uint64_t> RequireUInt(const JsonValue& object, std::string_view key,
                                  std::string_view context);
Result<std::int64_t> RequireInt(const JsonValue& object, std::string_view key,
                                std::string_view context);
Result<bool> RequireBool(const JsonValue& object, std::string_view key, std::string_view context);
Result<const JsonValue::Array*> RequireArray(const JsonValue& object, std::string_view key,
                                             std::string_view context);
Result<const JsonValue*> OptionalField(const JsonValue& object, std::string_view key);
Result<std::string> OptionalString(const JsonValue& object, std::string_view key,
                                   std::string_view fallback);
std::string_view JsonTypeName(JsonValue::Type type);

}  // namespace fcr
