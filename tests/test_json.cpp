#include <string>
#include <vector>

#include "fixtures.hpp"

using namespace fcr;
using namespace fcr::test;
using fcr::test::Rng;

namespace {

JsonValue RandomJson(Rng* rng, int depth) {
  const std::uint32_t pick = rng->Below(depth > 4 ? 6u : 9u);
  switch (pick) {
    case 0: return JsonValue::Null();
    case 1: return JsonValue::Bool(rng->Chance(50));
    case 2: return JsonValue::Int(static_cast<std::int64_t>(rng->Next() % 100000));
    case 3: return JsonValue::UInt(rng->Next() % 100000);
    case 4: return JsonValue::Double(static_cast<double>(rng->Below(1000)) / 8.0);
    case 5: return JsonValue::Str("s" + std::to_string(rng->Below(1000)));
    case 6: {
      JsonValue array = JsonValue::Arr();
      const std::uint32_t count = rng->Below(6);
      for (std::uint32_t i = 0; i < count; ++i) array.Push(RandomJson(rng, depth + 1));
      return array;
    }
    case 7: {
      JsonValue object = JsonValue::Obj();
      const std::uint32_t count = rng->Below(6);
      for (std::uint32_t i = 0; i < count; ++i) {
        object.Set("k" + std::to_string(rng->Below(20)), RandomJson(rng, depth + 1));
      }
      return object;
    }
    default: return JsonValue::Str("esc\"\\\n\t\u00e9");
  }
}

std::string Mutate(Rng* rng, const std::string& text) {
  std::string out = text;
  const std::uint32_t edits = 1 + rng->Below(6);
  for (std::uint32_t i = 0; i < edits; ++i) {
    if (out.empty()) break;
    const std::uint32_t mode = rng->Below(4);
    const std::size_t index = rng->Below(static_cast<std::uint32_t>(out.size()));
    if (mode == 0) {
      out[index] = static_cast<char>(rng->Below(256));
    } else if (mode == 1) {
      out.insert(index, 1, static_cast<char>('0' + static_cast<int>(rng->Below(10))));
    } else if (mode == 2) {
      out.erase(index, 1);
    } else {
      static const char* kWords[] = {"}", "]", "\"", "null", "true", ",", ":", "\\"};
      out.insert(index, kWords[rng->Below(8)]);
    }
  }
  return out;
}

}  // namespace

FCR_TEST(json, parses_scalars_and_containers) {
  auto value = ParseJson("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":-2.5,\"d\":18446744073709551615}");
  CHECK_OK(value);
  CHECK(value.value().IsObject());
  CHECK_EQ(value.value().Find("a")->AsUInt().value(), 1u);
  CHECK_EQ(value.value().Find("b")->AsArray()->size(), std::size_t{3});
  CHECK(value.value().Find("b")->AsArray()->at(0).AsBool(false));
  CHECK(value.value().Find("b")->AsArray()->at(1).IsNull());
  CHECK_EQ(*value.value().Find("b")->AsArray()->at(2).AsString(), std::string("x"));
  CHECK_EQ(value.value().Find("c")->AsDouble().value(), -2.5);
  CHECK_EQ(value.value().Find("d")->AsUInt().value(), 18446744073709551615ull);
}

FCR_TEST(json, rejects_malformed_input) {
  const char* cases[] = {
      "",        "{",          "}",         "[1,",       "{\"a\"}",   "{\"a\":}",  "{'a':1}",
      "{\"a\":1,}", "{\"a\":1} trailing", "01",       "+1",        ".5",        "1.",
      "1e",      "\"unterminated", "{\"a\":1,\"a\":2}", "tru",  "nul",      "[1 2]",
      "{\"a\" 1}", "18446744073709551616", "-9223372036854775809", "1e999",
      "\"\\x\"", "\"\\u12\"", "\"\\ud800\"", "\"\\udc00\"",
  };
  for (const char* text : cases) {
    auto value = ParseJson(text);
    if (value.has_value()) {
      FCR_FAIL("JSON input should have been rejected: " << text);
    }
  }
}

FCR_TEST(json, accepts_escapes_and_surrogate_pairs) {
  auto value = ParseJson("\"\\u0041\\u00e9\\ud83d\\ude00\\n\\t\\\\\\\"\"");
  CHECK_OK(value);
  const std::string expected = std::string("A") + "\xc3\xa9" + "\xf0\x9f\x98\x80" + "\n\t\\\"";
  CHECK_EQ(*value.value().AsString(), expected);
}

FCR_TEST(json, output_is_canonical_and_key_sorted) {
  auto value = ParseJson("{\"z\":1,\"a\":2,\"m\":{\"y\":1,\"b\":2}}");
  CHECK_OK(value);
  CHECK_EQ(value.value().Dump(false), std::string("{\"a\":2,\"m\":{\"b\":2,\"y\":1},\"z\":1}"));
  // Insertion order does not affect the canonical bytes.
  JsonValue built = JsonValue::Obj();
  built.Set("z", JsonValue::UInt(1));
  built.Set("a", JsonValue::UInt(2));
  JsonValue nested = JsonValue::Obj();
  nested.Set("y", JsonValue::UInt(1));
  nested.Set("b", JsonValue::UInt(2));
  built.Set("m", std::move(nested));
  CHECK_EQ(built.Dump(false), value.value().Dump(false));
}

FCR_TEST(json, enforces_depth_and_size_limits) {
  std::string deep;
  for (int i = 0; i < 200; ++i) deep.push_back('[');
  for (int i = 0; i < 200; ++i) deep.push_back(']');
  auto value = ParseJson(deep);
  CHECK_FALSE(value.has_value());
  CHECK_EQ(value.error().code, ErrorCode::LimitExceeded);

  JsonLimits tiny;
  tiny.max_bytes = 4;
  auto too_big = ParseJson("{\"a\":1}", tiny);
  CHECK_FALSE(too_big.has_value());
  CHECK_EQ(too_big.error().code, ErrorCode::LimitExceeded);

  JsonLimits shallow;
  shallow.max_depth = 2;
  auto nested = ParseJson("[[[1]]]", shallow);
  CHECK_FALSE(nested.has_value());
}

FCR_TEST(json, property_round_trip_is_stable) {
  Rng rng(0x9e3779b9ull);
  for (int i = 0; i < 600; ++i) {
    const JsonValue original = RandomJson(&rng, 0);
    const std::string text = original.Dump(false);
    auto reparsed = ParseJson(text);
    if (!reparsed.has_value()) {
      FCR_FAIL("canonical output failed to parse: " << text << " (" << reparsed.error().ToString()
                                                    << ")");
    }
    CHECK_EQ(reparsed.value().Dump(false), text);
  }
}

FCR_TEST(json, property_mutated_documents_never_crash) {
  Rng rng(0x1234567ull);
  const char* seeds[] = {
      "{\"a\":[1,2,3],\"b\":{\"c\":true}}",
      "[1,2,{\"x\":\"y\"}]",
      "{\"format\":\"fcr.registry.document\",\"format_version\":1,\"rules\":[]}",
      "\"\\u00e9\\ud83d\\ude00\"",
      "12345678901234567890",
  };
  for (const char* seed : seeds) {
    for (int i = 0; i < 800; ++i) {
      const std::string mutated = Mutate(&rng, seed);
      auto value = ParseJson(mutated);
      if (value.has_value()) {
        // Whatever parses must re-serialize into something that parses again.
        auto again = ParseJson(value.value().Dump(false));
        CHECK(again.has_value());
      }
    }
  }
}
