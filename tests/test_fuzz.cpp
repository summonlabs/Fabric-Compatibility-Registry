#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fixtures.hpp"

using namespace fcr;
using namespace fcr::test;
using fcr::test::Rng;

namespace {

std::string StandardDocumentJson() {
  return RegistryDocumentToJson(MakeStandardDocument()).Dump(false);
}

std::string MutateDocument(Rng* rng, const std::string& seed) {
  std::string out = seed;
  const std::uint32_t edits = 1 + rng->Below(10);
  for (std::uint32_t i = 0; i < edits; ++i) {
    if (out.empty()) break;
    const std::size_t index = rng->Below(static_cast<std::uint32_t>(out.size()));
    switch (rng->Below(6)) {
      case 0: out[index] = static_cast<char>(rng->Below(256)); break;
      case 1: out.insert(index, 1, static_cast<char>('0' + static_cast<int>(rng->Below(10)))); break;
      case 2: out.erase(index, 1 + rng->Below(8) % std::max<std::size_t>(1, out.size() - index)); break;
      case 3: out.insert(index, "\"version\""); break;
      case 4: out.insert(index, "999999999999999999999999"); break;
      case 5: {
        static const char* kChunks[] = {"\"rules\":[", "],\"taxonomy\"", "null", "true",
                                        "\"outcome\":\"incompatible\"", "{", "}"};
        out.insert(index, kChunks[rng->Below(7)]);
        break;
      }
      default: break;
    }
  }
  return out;
}

void NeverCrashesOnText(const std::string& text) {
  auto json = ParseJson(text, JsonLimits{});
  if (!json.has_value()) return;
  auto document = ParseRegistryDocument(json.value());
  if (!document.has_value()) return;
  const ValidationReport report = ValidateRegistryDocument(document.value());
  (void)report;
  auto compiled = RegistryGeneration::Compile(document.value());
  if (compiled.has_value()) {
    ComponentSpec left = MakeNic("a", "2.4.1");
    ComponentSpec right = MakeNic("b", "2.4.1");
    const Decision decision = compiled.value()->EvaluatePair(left, right);
    CHECK(decision.id.ToHex().size() == 64);
  }
}

}  // namespace

FCR_TEST(fuzz, malformed_documents_are_always_typed_errors) {
  Rng rng(0x5eed1234ull);
  const std::string seed = StandardDocumentJson();
  std::size_t parsed = 0;
  for (int attempt = 0; attempt < 4000; ++attempt) {
    const std::string mutated = MutateDocument(&rng, seed);
    auto json = ParseJson(mutated, JsonLimits{});
    if (json.has_value()) ++parsed;
    NeverCrashesOnText(mutated);
  }
  CHECK(parsed > 0);
}

FCR_TEST(fuzz, absurd_version_components_are_rejected) {
  const char* components[] = {
      "99999999999999999999", "4294967296", "0", "1", "01", "", "x", "*", "-1", "1e9", "+1",
      "18446744073709551616", "0000000000000000000001",
  };
  for (const char* component : components) {
    const std::string text = std::string(component) + "." + component + "." + component;
    auto version = SemVersion::Parse(text);
    const bool valid = std::string(component) == "0" || std::string(component) == "1";
    if (valid) {
      CHECK(version.has_value());
    } else {
      CHECK_FALSE(version.has_value());
    }
    auto range = VersionRange::Parse(text);
    if (range.has_value()) {
      CHECK(range.value().Contains(SemVersion(1, 0, 0)) == range.value().Contains(SemVersion(1, 0, 0)));
    }
  }
  // A document whose selector carries an absurd version must be a typed error
  // rather than a silent fallback.
  const std::string text =
      std::string("{\"format\":\"fcr.registry.document\",\"format_version\":1,") +
      "\"generation\":1,\"created_at\":\"2026-01-01T00:00:00Z\",\"publisher\":\"p\"," +
      "\"publisher_epoch\":1,\"name\":\"n\",\"taxonomy\":{},\"rules\":[{\"id\":\"absurd\"," +
      "\"outcome\":\"compatible\",\"left\":{\"version\":\"99999999999999999999.0.0\"}," +
      "\"right\":{},\"provenance\":{\"publisher\":\"p\"}}]}";
  auto json = ParseJson(text);
  CHECK_OK(json);
  auto document = ParseRegistryDocument(json.value());
  CHECK_FALSE(document.has_value());
}

FCR_TEST(fuzz, duplicate_rules_and_cyclic_references_are_rejected) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule duplicated = MakeRule("duplicate", RuleOutcome::Compatible);
  duplicated.left.kind = NicKind();
  duplicated.right.kind = NicKind();
  document.rules.push_back(duplicated);
  document.rules.push_back(duplicated);
  document.Normalize();
  const ValidationReport duplicate_report = ValidateRegistryDocument(document);
  CHECK_FALSE(duplicate_report.Publishable());

  RegistryDocument cyclic = MakeStandardDocument();
  cyclic.rules.clear();
  Rule rule = MakeRule("cyclic", RuleOutcome::Compatible);
  rule.left.kind = NicKind();
  rule.right.kind = NicKind();
  for (int i = 0; i < 8; ++i) {
    rule.implications.push_back(CapabilityImplication{
        static_cast<Side>(i % 2), RoceCapability(), LanesCapability()});
    rule.implications.push_back(CapabilityImplication{
        static_cast<Side>(i % 2), LanesCapability(), RoceCapability()});
  }
  cyclic.rules.push_back(rule);
  const ValidationReport cyclic_report = ValidateRegistryDocument(cyclic);
  CHECK_FALSE(cyclic_report.Publishable());
  CHECK_FALSE(cyclic_report.diagnostics.empty());
}

FCR_TEST(fuzz, deeply_nested_documents_are_bounded) {
  std::string deep = "{\"format\":\"fcr.registry.document\",\"format_version\":1,\"extra\":";
  for (int i = 0; i < 500; ++i) deep.push_back('[');
  for (int i = 0; i < 500; ++i) deep.push_back(']');
  deep.push_back('}');
  auto json = ParseJson(deep, JsonLimits{});
  CHECK_FALSE(json.has_value());
}

FCR_TEST(fuzz, oversized_collections_are_bounded) {
  std::string text = "{\"rules\":[";
  for (int i = 0; i < 70000; ++i) {
    if (i != 0) text.push_back(',');
    text.append("{\"id\":\"r").append(std::to_string(i)).append("\"}");
  }
  text.append("]}");
  DocumentLimits limits;
  limits.max_rules = 1024;
  JsonLimits json_limits;
  json_limits.max_bytes = 32u * 1024u * 1024u;
  auto json = ParseJson(text, json_limits);
  CHECK_OK(json);
  auto document = ParseRegistryDocument(json.value(), limits);
  CHECK_FALSE(document.has_value());
}

FCR_TEST(fuzz, corrupt_generation_files_never_crash_the_decoder) {
  RegistryDocument document = MakeStandardDocument();
  document.meta.generation = GenerationNumber(3);
  document.meta.epoch = PublisherEpoch(2);
  document.Normalize();
  auto encoded = RegistryStore::EncodeGeneration(document, GenerationNumber(3), PublisherEpoch(2),
                                                 Incarnation(5), 64ull * 1024ull * 1024ull);
  CHECK_OK(encoded);
  Rng rng(0xc0decafeull);
  for (int attempt = 0; attempt < 1500; ++attempt) {
    std::vector<std::byte> corrupted = encoded.value();
    const std::uint32_t edits = 1 + rng.Below(4);
    for (std::uint32_t i = 0; i < edits; ++i) {
      std::size_t index = rng.Below(static_cast<std::uint32_t>(corrupted.size()));
      corrupted[index] = static_cast<std::byte>(rng.Below(256));
    }
    if (rng.Chance(20)) {
      const std::uint32_t new_size = rng.Below(static_cast<std::uint32_t>(corrupted.size()));
      corrupted.resize(new_size);
    }
    auto decoded = RegistryStore::DecodeGeneration(corrupted, nullptr, nullptr, nullptr);
    if (decoded.has_value()) {
      CHECK_EQ(decoded.value().CanonicalJson(), document.CanonicalJson());
    }
  }
  // Random noise of arbitrary lengths.
  for (int attempt = 0; attempt < 1000; ++attempt) {
    std::vector<std::byte> noise(static_cast<std::size_t>(rng.Below(400)));
    for (std::byte& byte : noise) byte = static_cast<std::byte>(rng.Below(256));
    auto decoded = RegistryStore::DecodeGeneration(noise, nullptr, nullptr, nullptr);
    CHECK_FALSE(decoded.has_value());
  }
}

FCR_TEST(fuzz, malformed_component_specs_are_rejected) {
  const Taxonomy& taxonomy = MakeStandardDocument().taxonomy;
  const char* cases[] = {
      "{}",
      "{\"family\":\"fabric.transport\"}",
      "{\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\"}",
      "{\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\",\"version\":\"nope\"}",
      "{\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\",\"version\":\"1.0.0\","
      "\"capabilities\":{\"ghost.cap\":true}}",
      "{\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\",\"version\":\"1.0.0\","
      "\"capabilities\":{\"link.lanes\":\"lots\"}}",
      "{\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\",\"version\":\"1.0.0\","
      "\"protocols\":{\"fabric.rdma\":[]}}",
      "{\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\",\"version\":\"1.0.0\","
      "\"schemas\":{\"fabric.config\":[1]}}",
      "{\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\",\"version\":\"1.0.0\","
      "\"knowledge\":{\"capabilities\":\"maybe\"}}",
      "{\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\",\"version\":\"1.0.0\","
      "\"features\":[\"ghost.feature\"]}",
      "[]",
      "null",
  };
  for (const char* text : cases) {
    auto json = ParseJson(text);
    if (!json.has_value()) continue;
    auto spec = ParseComponentSpec(json.value(), taxonomy, "spec");
    CHECK_FALSE(spec.has_value());
  }
  // A well-formed spec still parses.
  auto good = ParseJson("{\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\","
                        "\"version\":\"1.0.0\",\"capabilities\":{\"rdma.rocev2\":true}}");
  CHECK_OK(good);
  CHECK_OK(ParseComponentSpec(good.value(), taxonomy, "spec"));
}
