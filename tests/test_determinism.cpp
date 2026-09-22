#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fixtures.hpp"

using namespace fcr;
using namespace fcr::test;
using fcr::test::Rng;

namespace {

std::string ShuffledCanonical(const RegistryDocument& document, Rng* rng) {
  RegistryDocument shuffled = document;
  for (std::size_t i = shuffled.rules.size(); i > 1; --i) {
    std::swap(shuffled.rules[i - 1], shuffled.rules[rng->Below(static_cast<std::uint32_t>(i))]);
  }
  for (Rule& rule : shuffled.rules) {
    for (std::size_t i = rule.constraints.size(); i > 1; --i) {
      std::swap(rule.constraints[i - 1],
                rule.constraints[rng->Below(static_cast<std::uint32_t>(i))]);
    }
    for (std::size_t i = rule.implications.size(); i > 1; --i) {
      std::swap(rule.implications[i - 1],
                rule.implications[rng->Below(static_cast<std::uint32_t>(i))]);
    }
  }
  return shuffled.CanonicalJson();
}

ComponentSpec RandomNic(Rng* rng, const std::string& instance) {
  ComponentSpec spec = MakeNic(instance, std::to_string(rng->Below(4)) + "." +
                                             std::to_string(rng->Below(4)) + ".0");
  if (rng->Chance(60)) {
    CHECK_OK(spec.capabilities.Set(RoceCapability(), CapabilityValue::Flag(true)));
  }
  if (rng->Chance(50)) {
    CHECK_OK(spec.capabilities.Set(LanesCapability(),
                                   CapabilityValue::Integer(static_cast<std::int64_t>(
                                       1 + rng->Below(32)))));
  }
  if (rng->Chance(40)) {
    CHECK_OK(spec.protocols.Set(RdmaProtocol(),
                                {SemVersion(2, 0, 0), SemVersion(2, 1, 0)}));
  }
  if (rng->Chance(30)) {
    spec.hardware_class = rng->Chance(50) ? SmartNicGen1() : SmartNicGen3();
  }
  if (rng->Chance(50)) spec.knowledge.capabilities = KnowledgeClosure::Open;
  if (rng->Chance(50)) spec.knowledge.protocols = KnowledgeClosure::Open;
  return spec;
}

}  // namespace

FCR_TEST(determinism, canonical_form_is_independent_of_construction_order) {
  const RegistryDocument document = MakeStandardDocument();
  const std::string baseline = document.CanonicalJson();
  const std::string baseline_digest = document.Digest().ToHex();
  Rng rng(0xdeadbeefull);
  for (int attempt = 0; attempt < 64; ++attempt) {
    CHECK_EQ(ShuffledCanonical(document, &rng), baseline);
    auto reparsed = ParseRegistryDocumentText(ShuffledCanonical(document, &rng));
    CHECK_OK(reparsed);
    CHECK_EQ(reparsed.value().Digest().ToHex(), baseline_digest);
  }
}

FCR_TEST(determinism, identical_input_yields_identical_decisions) {
  Rng rng(0x5150ull);
  const auto registry = RegistryGeneration::Compile(MakeStandardDocument());
  CHECK_OK(registry);
  const auto recompiled = RegistryGeneration::Compile(MakeStandardDocument());
  CHECK_OK(recompiled);
  CHECK(registry.value()->id() == recompiled.value()->id());

  for (int attempt = 0; attempt < 300; ++attempt) {
    const ComponentSpec left = RandomNic(&rng, "nic-" + std::to_string(attempt) + "-a");
    const ComponentSpec right = RandomNic(&rng, "nic-" + std::to_string(attempt) + "-b");
    const Decision first = registry.value()->EvaluatePair(left, right);
    const Decision second = registry.value()->EvaluatePair(left, right);
    const Decision third = recompiled.value()->EvaluatePair(left, right);
    CHECK(first.id == second.id);
    CHECK(first.id == third.id);
    CHECK_EQ(first.outcome, third.outcome);
    CHECK_EQ(RenderDecision(first), RenderDecision(third));
    CHECK_EQ(DecisionToJson(first).Dump(false), DecisionToJson(third).Dump(false));
  }
}

FCR_TEST(determinism, unknown_is_never_silently_compatible) {
  Rng rng(0x1234abcdull);
  RegistryDocument document = MakeStandardDocument();
  // Remove every rule: no knowledge at all.
  document.rules.clear();
  auto registry = RegistryGeneration::Compile(document);
  CHECK_OK(registry);
  for (int attempt = 0; attempt < 100; ++attempt) {
    const ComponentSpec left = RandomNic(&rng, "nic-a");
    const ComponentSpec right = RandomNic(&rng, "nic-b");
    const Decision decision = registry.value()->EvaluatePair(left, right);
    CHECK(decision.IsUnknown());
  }
  // With open knowledge, a partially satisfied rule must never report
  // COMPATIBLE.
  RegistryDocument open_document = MakeStandardDocument();
  auto open_registry = RegistryGeneration::Compile(open_document);
  CHECK_OK(open_registry);
  std::size_t unknown_count = 0;
  for (int attempt = 0; attempt < 300; ++attempt) {
    ComponentSpec left = RandomNic(&rng, "nic-a");
    ComponentSpec right = RandomNic(&rng, "nic-b");
    left.knowledge.capabilities = KnowledgeClosure::Open;
    right.knowledge.capabilities = KnowledgeClosure::Open;
    const Decision decision = open_registry.value()->EvaluatePair(left, right);
    if (decision.IsUnknown()) ++unknown_count;
    if (decision.IsCompatible()) {
      CHECK(decision.deciding_rule.has_value());
      // A COMPATIBLE verdict may only come from a rule whose constraints were
      // all definitely satisfied.
      bool satisfied = false;
      for (const RuleEvaluation& evaluation : decision.matched_rules) {
        if (evaluation.decided) satisfied = evaluation.status == RuleEvaluationStatus::Satisfied;
      }
      CHECK(satisfied);
    }
  }
  CHECK(unknown_count > 0);
}

FCR_TEST(determinism, evidence_order_follows_precedence) {
  const auto registry = RegistryGeneration::Compile(MakeStandardDocument());
  CHECK_OK(registry);
  const ComponentSpec left = MakeNic("nic-a", "2.4.1");
  const ComponentSpec right = MakeNic("nic-b", "2.4.1");
  const Decision decision = registry.value()->EvaluatePair(left, right);
  for (std::size_t i = 1; i < decision.matched_rules.size(); ++i) {
    const RuleEvaluation& previous = decision.matched_rules[i - 1];
    const RuleEvaluation& current = decision.matched_rules[i];
    CHECK_EQ(previous.precedence_rank + 1, current.precedence_rank);
    CHECK(PrecedenceHigher(previous.precedence, current.precedence) ||
          previous.precedence.tier == current.precedence.tier);
  }
}

FCR_TEST(determinism, diff_is_deterministic_and_symmetric_in_its_reporting) {
  auto before = RegistryGeneration::Compile(MakeStandardDocument());
  CHECK_OK(before);
  RegistryDocument modified_document = MakeStandardDocument();
  for (Rule& rule : modified_document.rules) {
    if (rule.id.value() == "nic-requires-roce") {
      rule.revision = RuleRevision(2);
      rule.rationale = "tightened negotiation window";
    }
  }
  Rule added = MakeRule("new-rule", RuleOutcome::Compatible);
  added.left.kind = GpuKind();
  added.right.kind = GpuKind();
  modified_document.rules.push_back(added);
  modified_document.Normalize();
  auto after = RegistryGeneration::Compile(modified_document);
  CHECK_OK(after);

  const GenerationDiff first = DiffGenerations(*before.value(), *after.value());
  const GenerationDiff second = DiffGenerations(*before.value(), *after.value());
  CHECK_EQ(RenderGenerationDiff(first), RenderGenerationDiff(second));
  CHECK_EQ(GenerationDiffToJson(first).Dump(false), GenerationDiffToJson(second).Dump(false));
  CHECK_EQ(first.added, std::size_t{1});
  CHECK_EQ(first.modified, std::size_t{1});
  CHECK_EQ(first.removed, std::size_t{0});
  bool saw_field = false;
  for (const RuleDiffEntry& entry : first.rules) {
    if (entry.kind == RuleDiffKind::Modified) {
      CHECK(std::find(entry.changed_fields.begin(), entry.changed_fields.end(), "revision") !=
            entry.changed_fields.end());
      saw_field = true;
    }
  }
  CHECK(saw_field);
}
