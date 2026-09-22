#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fixtures.hpp"

using namespace fcr;
using namespace fcr::test;
using fcr::test::Rng;

namespace {

std::shared_ptr<const RegistryGeneration> CompileStandard() {
  auto compiled = RegistryGeneration::Compile(MakeStandardDocument());
  CHECK_OK(compiled);
  return compiled.value();
}

ComponentSpec NicWithRoce(const std::string& instance, const std::string& version,
                          const char* rdma_versions) {
  ComponentSpec spec = MakeNic(instance, version);
  CHECK_OK(spec.capabilities.Set(RoceCapability(), CapabilityValue::Flag(true)));
  std::vector<SemVersion> versions;
  std::string text = rdma_versions;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::string piece =
        comma == std::string::npos ? text.substr(start) : text.substr(start, comma - start);
    if (!piece.empty()) versions.push_back(SemVersion::Parse(piece).value());
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  CHECK_OK(spec.protocols.Set(RdmaProtocol(), versions));
  return spec;
}

bool MentionsRule(const Decision& decision, const std::string& rule) {
  for (const RuleEvaluation& evaluation : decision.matched_rules) {
    if (evaluation.rule.ToString() == rule) return true;
  }
  return false;
}

const NegotiatedSubject* FindNegotiated(const std::vector<NegotiatedSubject>& subjects,
                                        const std::string& id) {
  for (const NegotiatedSubject& subject : subjects) {
    if (subject.subject == id) return &subject;
  }
  return nullptr;
}

}  // namespace

FCR_TEST(engine, highest_precedence_rule_decides) {
  const auto registry = CompileStandard();
  const ComponentSpec left = NicWithRoce("nic-a", "2.4.1", "2.0.0,2.1.0");
  const ComponentSpec right = NicWithRoce("nic-b", "2.1.0", "2.0.0,2.1.0");
  const Decision decision = registry->EvaluatePair(left, right);
  CHECK(decision.IsCompatible());
  CHECK_EQ(decision.reason, DecisionReason::DecidedByRule);
  CHECK(decision.deciding_rule.has_value());
  CHECK_EQ(decision.deciding_rule->ToString(), std::string("nic-requires-roce@r1"));
  CHECK(MentionsRule(decision, "nic-same-major@r1"));
  CHECK_EQ(decision.matched_rules.front().precedence_rank, std::size_t{0});
  CHECK(decision.matched_rules.front().decided);
  CHECK_EQ(decision.generation.number.value(), 1u);
  CHECK_FALSE(decision.narrative.empty());
}

FCR_TEST(engine, outranked_rules_are_recorded_as_overridden) {
  const auto registry = CompileStandard();
  const ComponentSpec left = MakeNic("nic-old", "1.5.0");
  const ComponentSpec right = MakeNic("nic-new", "3.1.0");
  const Decision decision = registry->EvaluatePair(left, right);
  CHECK(decision.IsIncompatible());
  CHECK_EQ(decision.deciding_rule->ToString(), std::string("nic-legacy-vs-modern@r1"));
  bool saw_override = false;
  for (const OverriddenNote& note : decision.overridden) {
    if (note.rule.ToString() == "nic-same-major@r1" &&
        note.outcome == DecisionOutcome::Compatible) {
      saw_override = true;
    }
  }
  CHECK(saw_override);
}

FCR_TEST(engine, unmet_requirement_is_incompatible_when_knowledge_is_closed) {
  const auto registry = CompileStandard();
  ComponentSpec left = MakeNic("nic-a", "2.4.1");
  ComponentSpec right = MakeNic("nic-b", "2.4.1");
  CloseKnowledge(&left);
  CloseKnowledge(&right);
  const Decision decision = registry->EvaluatePair(left, right);
  CHECK(decision.IsIncompatible());
  CHECK_EQ(decision.deciding_rule->ToString(), std::string("nic-requires-roce@r1"));
  CHECK_FALSE(decision.unmet_requirements.empty());
  CHECK_EQ(decision.unmet_requirements.front().status, ConstraintStatus::Violated);
  for (const RuleEvaluation& evaluation : decision.matched_rules) {
    if (evaluation.rule.ToString() != "nic-requires-roce@r1") continue;
    CHECK_EQ(evaluation.status, RuleEvaluationStatus::Violated);
    CHECK_FALSE(evaluation.constraints.empty());
  }
}

FCR_TEST(engine, incomplete_knowledge_is_unknown_and_never_compatible) {
  const auto registry = CompileStandard();
  ComponentSpec left = MakeNic("nic-a", "2.4.1");
  ComponentSpec right = MakeNic("nic-b", "2.4.1");
  CHECK_OK(left.protocols.Set(RdmaProtocol(), {SemVersion(2, 0, 0)}));
  CHECK_OK(right.protocols.Set(RdmaProtocol(), {SemVersion(2, 0, 0)}));
  // Reopen the capability closure: the component may or may not have RoCE, but
  // its protocol support is fully known.
  left.knowledge.capabilities = KnowledgeClosure::Open;
  right.knowledge.capabilities = KnowledgeClosure::Open;
  const Decision decision = registry->EvaluatePair(left, right);
  CHECK(decision.IsUnknown());
  CHECK_FALSE(decision.IsCompatible());
  CHECK_EQ(decision.deciding_rule->ToString(), std::string("nic-requires-roce@r1"));
  CHECK_EQ(decision.matched_rules.front().status, RuleEvaluationStatus::Indeterminate);
  CHECK_FALSE(decision.unmet_requirements.empty());
  CHECK_EQ(decision.unmet_requirements.front().status, ConstraintStatus::Indeterminate);
}

FCR_TEST(engine, no_matching_rule_is_unknown_not_compatible) {
  const auto registry = CompileStandard();
  const ComponentSpec left = MakeSwitch("sw-a", "1.0.0");
  const ComponentSpec right = MakeSwitch("sw-b", "2.0.0");
  const Decision decision = registry->EvaluatePair(left, right);
  CHECK(decision.IsUnknown());
  CHECK_EQ(decision.reason, DecisionReason::NoMatchingRule);
  CHECK_EQ(decision.total_matched_rules, std::size_t{0});
  CHECK_FALSE(decision.deciding_rule.has_value());
  CHECK(decision.narrative.find("outcome: unknown") != std::string::npos);
  CHECK(decision.narrative.find("reason: no_matching_rule") != std::string::npos);
}

FCR_TEST(engine, unregistered_component_kind_is_unknown) {
  const auto registry = CompileStandard();
  ComponentSpec left = MakeNic("nic-a", "2.4.1");
  ComponentSpec right = MakeNic("nic-b", "2.4.1");
  right.kind = ComponentKindId::Parse("unregistered.kind").value();
  const Decision decision = registry->EvaluatePair(left, right);
  CHECK(decision.IsUnknown());
  CHECK_EQ(decision.reason, DecisionReason::UnregisteredComponentKind);
}

FCR_TEST(engine, asymmetric_rules_apply_in_one_direction_only) {
  const auto registry = CompileStandard();
  const ComponentSpec nic = MakeNic("nic-a", "2.4.1");
  const ComponentSpec legacy_switch = MakeSwitch("sw-a", "1.5.0");
  const Decision forward = registry->EvaluatePair(nic, legacy_switch);
  CHECK(forward.IsCompatible());
  CHECK_EQ(forward.deciding_rule->ToString(), std::string("nic-fronts-legacy-switch@r1"));

  const Decision reverse = registry->EvaluatePair(legacy_switch, nic);
  CHECK(reverse.IsUnknown());
  CHECK_FALSE(reverse.deciding_rule.has_value());
}

FCR_TEST(engine, symmetric_rules_match_both_orientations) {
  const auto registry = CompileStandard();
  const ComponentSpec gen1 = MakeNic("nic-gen1", "2.4.1");
  const ComponentSpec gen3 = MakeNic("nic-gen3", "2.4.1");
  ComponentSpec with_gen1 = gen1;
  with_gen1.hardware_class = SmartNicGen1();
  ComponentSpec with_gen3 = gen3;
  with_gen3.hardware_class = SmartNicGen3();
  CHECK_OK(with_gen1.capabilities.Set(RoceCapability(), CapabilityValue::Flag(true)));
  CHECK_OK(with_gen1.protocols.Set(RdmaProtocol(), {SemVersion(2, 4, 1)}));
  CHECK_OK(with_gen3.capabilities.Set(RoceCapability(), CapabilityValue::Flag(true)));
  CHECK_OK(with_gen3.protocols.Set(RdmaProtocol(), {SemVersion(2, 4, 1)}));

  const Decision forward = registry->EvaluatePair(with_gen1, with_gen3);
  CHECK(forward.IsIncompatible());
  CHECK_EQ(forward.deciding_rule->ToString(), std::string("smartnic-generation-mix@r1"));
  const Decision reverse = registry->EvaluatePair(with_gen3, with_gen1);
  CHECK(reverse.IsIncompatible());
  CHECK_EQ(reverse.deciding_rule->ToString(), std::string("smartnic-generation-mix@r1"));
  bool mirrored_seen = false;
  for (const RuleEvaluation& evaluation : reverse.matched_rules) {
    if (evaluation.rule.ToString() == "smartnic-generation-mix@r1" && evaluation.mirrored) {
      mirrored_seen = true;
    }
  }
  CHECK(mirrored_seen);
}

FCR_TEST(engine, negotiation_selects_the_highest_common_version) {
  const auto registry = CompileStandard();
  const ComponentSpec left = NicWithRoce("nic-a", "2.4.1", "2.0.0,2.1.0,2.2.0");
  const ComponentSpec right = NicWithRoce("nic-b", "2.4.1", "2.1.0,2.2.0");
  const Decision decision = registry->EvaluatePair(left, right);
  CHECK(decision.IsCompatible());
  const NegotiatedSubject* subject =
      FindNegotiated(decision.negotiation.protocols, "fabric.rdma");
  CHECK(subject != nullptr);
  CHECK_EQ(subject->status, NegotiationStatus::Agreed);
  CHECK(subject->selected.has_value());
  CHECK_EQ(subject->selected->ToString(), std::string("2.2.0"));
  CHECK_EQ(subject->window.size(), std::size_t{2});
  CHECK_EQ(decision.negotiation.policy, NegotiationPolicy::HighestCommon);
}

FCR_TEST(engine, negotiation_policies_are_honoured) {
  RegistryDocument document = MakeStandardDocument();
  for (Rule& rule : document.rules) {
    if (rule.id.value() == "nic-requires-roce") rule.negotiation = NegotiationPolicy::LowestCommon;
  }
  auto registry = RegistryGeneration::Compile(document);
  CHECK_OK(registry);
  const ComponentSpec left = NicWithRoce("nic-a", "2.4.1", "2.0.0,2.1.0,2.2.0");
  const ComponentSpec right = NicWithRoce("nic-b", "2.4.1", "2.1.0,2.2.0");
  const Decision decision = registry.value()->EvaluatePair(left, right);
  CHECK(decision.IsCompatible());
  const NegotiatedSubject* subject =
      FindNegotiated(decision.negotiation.protocols, "fabric.rdma");
  CHECK(subject != nullptr);
  CHECK_EQ(subject->selected->ToString(), std::string("2.1.0"));
}

FCR_TEST(engine, negotiation_reports_no_overlap_without_failing_the_decision) {
  const auto registry = CompileStandard();
  const ComponentSpec left = NicWithRoce("nic-a", "2.4.1", "2.0.0");
  const ComponentSpec right = NicWithRoce("nic-b", "2.4.1", "2.0.0");
  const Decision decision = registry->EvaluatePair(left, right);
  CHECK(decision.IsCompatible());
  const NegotiatedSubject* subject =
      FindNegotiated(decision.negotiation.protocols, "fabric.rdma");
  CHECK(subject != nullptr);
  CHECK_EQ(subject->status, NegotiationStatus::Agreed);
  CHECK_EQ(subject->selected->ToString(), std::string("2.0.0"));

  ComponentSpec conflicting_left = NicWithRoce("nic-c", "2.4.1", "2.0.0");
  ComponentSpec conflicting_right = NicWithRoce("nic-d", "2.4.1", "2.1.0");
  const Decision conflicting = registry->EvaluatePair(conflicting_left, conflicting_right);
  // The rule requires fabric.rdma in ^2.0.0 on both sides, which both satisfy,
  // but the two components share no common version.
  const NegotiatedSubject* conflicting_subject =
      FindNegotiated(conflicting.negotiation.protocols, "fabric.rdma");
  CHECK(conflicting_subject != nullptr);
  CHECK_EQ(conflicting_subject->status, NegotiationStatus::NoOverlap);
  CHECK_FALSE(conflicting_subject->selected.has_value());
}

FCR_TEST(engine, negotiation_distinguishes_unsupported_from_undetermined) {
  const auto registry = CompileStandard();
  ComponentSpec left = NicWithRoce("nic-a", "2.4.1", "2.0.0");
  ComponentSpec right = MakeNic("nic-b", "2.4.1");
  right.knowledge.protocols = KnowledgeClosure::Closed;
  const Decision closed_decision = registry->EvaluatePair(left, right);
  const NegotiatedSubject* closed_subject =
      FindNegotiated(closed_decision.negotiation.protocols, "fabric.rdma");
  CHECK(closed_subject != nullptr);
  CHECK_EQ(closed_subject->status, NegotiationStatus::NotSupported);

  ComponentSpec open_right = MakeNic("nic-c", "2.4.1");
  CHECK_OK(open_right.capabilities.Set(RoceCapability(), CapabilityValue::Flag(true)));
  open_right.knowledge.protocols = KnowledgeClosure::Open;
  const Decision open_decision = registry->EvaluatePair(left, open_right);
  const NegotiatedSubject* open_subject =
      FindNegotiated(open_decision.negotiation.protocols, "fabric.rdma");
  CHECK(open_subject != nullptr);
  CHECK_EQ(open_subject->status, NegotiationStatus::Indeterminate);
  CHECK_EQ(open_decision.outcome, DecisionOutcome::Unknown);
}

FCR_TEST(engine, exact_required_negotiation_refuses_a_window) {
  RegistryDocument document = MakeStandardDocument();
  for (Rule& rule : document.rules) {
    if (rule.id.value() == "nic-requires-roce") {
      rule.negotiation = NegotiationPolicy::ExactRequired;
    }
  }
  auto registry = RegistryGeneration::Compile(document);
  CHECK_OK(registry);
  const ComponentSpec left = NicWithRoce("nic-a", "2.4.1", "2.0.0,2.1.0");
  const ComponentSpec right = NicWithRoce("nic-b", "2.4.1", "2.0.0,2.1.0");
  const Decision decision = registry.value()->EvaluatePair(left, right);
  const NegotiatedSubject* subject =
      FindNegotiated(decision.negotiation.protocols, "fabric.rdma");
  CHECK(subject != nullptr);
  CHECK_EQ(subject->status, NegotiationStatus::NoOverlap);
}

FCR_TEST(engine, capability_value_comparisons_are_typed) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule rule = MakeRule("lanes-at-least-8", RuleOutcome::Compatible);
  rule.left.kind = NicKind();
  rule.right.kind = NicKind();
  rule.constraints.push_back(RequiresCapabilityValue{Side::Right, LanesCapability(),
                                                     ComparisonOp::Ge,
                                                     ConstraintLiteral::Integer(8)});
  document.rules.push_back(rule);
  auto registry = RegistryGeneration::Compile(document);
  CHECK_OK(registry);

  ComponentSpec wide = MakeNic("nic-wide", "2.4.1");
  CHECK_OK(wide.capabilities.Set(LanesCapability(), CapabilityValue::Integer(16)));
  ComponentSpec narrow = MakeNic("nic-narrow", "2.4.1");
  CHECK_OK(narrow.capabilities.Set(LanesCapability(), CapabilityValue::Integer(4)));
  // The constraint is attached to the right-hand side.
  CHECK(registry.value()->EvaluatePair(narrow, wide).IsCompatible());
  const Decision violated = registry.value()->EvaluatePair(narrow, narrow);
  CHECK(violated.IsIncompatible());
  CHECK_FALSE(violated.unmet_requirements.empty());
  const Decision asymmetric = registry.value()->EvaluatePair(wide, narrow);
  CHECK(asymmetric.IsIncompatible());
}

FCR_TEST(engine, set_query_aggregates_pairs_and_set_rules) {
  const auto registry = CompileStandard();
  std::vector<ComponentSpec> members;
  for (const char* name : {"nic-a", "nic-b", "nic-c"}) {
    members.push_back(NicWithRoce(name, "2.4.1", "2.0.0,2.1.0"));
  }
  for (ComponentSpec& member : members) {
    member.features.Add(SecureBootFeature());
  }
  auto decision = registry->EvaluateSet(members);
  CHECK_OK(decision);
  CHECK_EQ(decision.value().outcome, DecisionOutcome::Compatible);
  CHECK_EQ(decision.value().pairs.size(), std::size_t{3});
  CHECK_EQ(decision.value().set_level.outcome, DecisionOutcome::Compatible);
  CHECK_EQ(decision.value().set_level.deciding_rule->ToString(),
           std::string("transport-set-secure-boot@r1"));
}

FCR_TEST(engine, set_rule_violation_makes_the_set_incompatible) {
  const auto registry = CompileStandard();
  std::vector<ComponentSpec> members;
  for (const char* name : {"nic-a", "nic-b"}) {
    members.push_back(NicWithRoce(name, "2.4.1", "2.0.0,2.1.0"));
  }
  members[0].features.Add(SecureBootFeature());
  auto decision = registry->EvaluateSet(members);
  CHECK_OK(decision);
  CHECK_EQ(decision.value().outcome, DecisionOutcome::Incompatible);
  CHECK_EQ(decision.value().set_level.outcome, DecisionOutcome::Incompatible);
}

FCR_TEST(engine, set_rule_uncertainty_keeps_the_aggregate_unknown) {
  const auto registry = CompileStandard();
  std::vector<ComponentSpec> members;
  for (const char* name : {"nic-a", "nic-b"}) {
    ComponentSpec member = MakeNic(name, "2.4.1");
    CHECK_OK(member.protocols.Set(RdmaProtocol(), {SemVersion(2, 0, 0)}));
    member.knowledge.capabilities = KnowledgeClosure::Open;
    members.push_back(member);
  }
  auto decision = registry->EvaluateSet(members);
  CHECK_OK(decision);
  CHECK_EQ(decision.value().set_level.outcome, DecisionOutcome::Unknown);
  CHECK_EQ(decision.value().pairs[0].outcome, DecisionOutcome::Unknown);
  CHECK_EQ(decision.value().outcome, DecisionOutcome::Unknown);
}

FCR_TEST(engine, an_incompatible_pair_dominates_a_compatible_set_rule) {
  const auto registry = CompileStandard();
  std::vector<ComponentSpec> members;
  ComponentSpec old_nic = MakeNic("nic-old", "1.5.0");
  old_nic.features.Add(SecureBootFeature());
  ComponentSpec new_nic = NicWithRoce("nic-new", "3.1.0", "2.0.0");
  new_nic.features.Add(SecureBootFeature());
  members.push_back(old_nic);
  members.push_back(new_nic);
  auto decision = registry->EvaluateSet(members);
  CHECK_OK(decision);
  CHECK_EQ(decision.value().outcome, DecisionOutcome::Incompatible);
}

FCR_TEST(engine, set_query_bounds_are_enforced) {
  const auto registry = CompileStandard();
  std::vector<ComponentSpec> single{MakeNic("nic-a", "2.4.1")};
  CHECK_ERR(registry->EvaluateSet(single), ErrorCode::InvalidArgument);
  std::vector<ComponentSpec> huge;
  for (std::size_t i = 0; i <= kMaxSetMembers; ++i) {
    huge.push_back(MakeNic("nic-" + std::to_string(i), "2.4.1"));
  }
  CHECK_ERR(registry->EvaluateSet(huge), ErrorCode::LimitExceeded);
}

FCR_TEST(engine, decision_identifier_is_stable_for_equal_queries) {
  const auto registry = CompileStandard();
  const ComponentSpec left = NicWithRoce("nic-a", "2.4.1", "2.0.0");
  const ComponentSpec right = NicWithRoce("nic-b", "2.4.1", "2.0.0");
  const Decision first = registry->EvaluatePair(left, right);
  const Decision second = registry->EvaluatePair(left, right);
  CHECK(first.id == second.id);
  CHECK_EQ(RenderDecision(first), RenderDecision(second));
  CHECK_EQ(DecisionToJson(first).Dump(false), DecisionToJson(second).Dump(false));

  // Instance names and labels are not part of the decision.
  ComponentSpec renamed = left;
  renamed.instance = ComponentInstanceId::Parse("other-instance").value();
  renamed.label = "different label";
  const Decision third = registry->EvaluatePair(renamed, right);
  CHECK(third.id == first.id);
}

FCR_TEST(engine, compile_refuses_a_conflicting_rule_set) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule first = MakeRule("a", RuleOutcome::Compatible);
  first.left.kind = NicKind();
  first.right.kind = NicKind();
  Rule second = MakeRule("b", RuleOutcome::Incompatible);
  second.left.kind = NicKind();
  second.right.kind = NicKind();
  document.rules.push_back(first);
  document.rules.push_back(second);
  document.Normalize();
  CHECK_ERR(RegistryGeneration::Compile(document), ErrorCode::ValidationFailed);
}

FCR_TEST(engine, retired_rules_are_not_evaluated) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule retired = MakeRule("retired", RuleOutcome::Incompatible);
  retired.left.kind = NicKind();
  retired.right.kind = NicKind();
  retired.lifecycle.state = LifecycleState::Retired;
  document.rules.push_back(retired);
  auto registry = RegistryGeneration::Compile(document);
  CHECK_OK(registry);
  const Decision decision = registry.value()->EvaluatePair(MakeNic("a", "2.4.1"), MakeNic("b", "2.4.1"));
  CHECK_EQ(decision.outcome, DecisionOutcome::Unknown);
  CHECK_EQ(decision.reason, DecisionReason::NoActiveRules);
}

FCR_TEST(engine, generation_digest_is_part_of_the_decision) {
  RegistryDocument document = MakeStandardDocument();
  auto first = RegistryGeneration::Compile(document);
  CHECK_OK(first);
  RegistryDocument changed = document;
  changed.meta.created_at = Timestamp::FromNanos(document.meta.created_at.nanos() + 1);
  auto second = RegistryGeneration::Compile(changed);
  CHECK_OK(second);
  CHECK(first.value()->id().digest != second.value()->id().digest);
  const ComponentSpec left = NicWithRoce("nic-a", "2.4.1", "2.0.0");
  const ComponentSpec right = NicWithRoce("nic-b", "2.4.1", "2.0.0");
  const Decision a = first.value()->EvaluatePair(left, right);
  const Decision b = second.value()->EvaluatePair(left, right);
  CHECK(a.outcome == b.outcome);
  CHECK(a.id != b.id);
}

FCR_TEST(engine, same_tier_same_outcome_resolves_by_rule_identity) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule alpha = MakeRule("alpha-rule", RuleOutcome::Compatible);
  alpha.left.kind = NicKind();
  alpha.right.kind = NicKind();
  Rule beta = MakeRule("beta-rule", RuleOutcome::Compatible);
  beta.left.kind = NicKind();
  beta.right.kind = NicKind();
  document.rules.push_back(beta);
  document.rules.push_back(alpha);
  document.Normalize();
  auto registry = RegistryGeneration::Compile(document);
  CHECK_OK(registry);
  const Decision decision =
      registry.value()->EvaluatePair(MakeNic("a", "1.0.0"), MakeNic("b", "1.0.0"));
  CHECK(decision.IsCompatible());
  CHECK_EQ(decision.deciding_rule->id.value(), std::string("alpha-rule"));
}

FCR_TEST(engine, operator_origin_breaks_ties_below_priority) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule standard = MakeRule("standard", RuleOutcome::Compatible);
  standard.left.kind = NicKind();
  standard.left.version = VersionRange::Parse("^1.0.0").value();
  standard.right.kind = NicKind();
  standard.origin = RuleOrigin::Standard;
  Rule local = MakeRule("local", RuleOutcome::Incompatible);
  local.left.kind = NicKind();
  local.left.version = VersionRange::Parse("^1.2.0").value();
  local.right.kind = NicKind();
  local.origin = RuleOrigin::LocalOverride;
  document.rules.push_back(standard);
  document.rules.push_back(local);
  document.Normalize();
  // Same priority and the same derived specificity: only the origin separates
  // the two rules, and the local override must win.
  CHECK_EQ(ComputeSpecificity(document.rules[0]), ComputeSpecificity(document.rules[1]));
  auto registry = RegistryGeneration::Compile(document);
  CHECK_OK(registry);
  const Decision decision =
      registry.value()->EvaluatePair(MakeNic("a", "1.4.0"), MakeNic("b", "1.9.0"));
  CHECK(decision.IsIncompatible());
  CHECK_EQ(decision.deciding_rule->id.value(), std::string("local"));
}

FCR_TEST(engine, specificity_breaks_ties_below_priority) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule broad = MakeRule("broad", RuleOutcome::Incompatible);
  broad.left.kind = NicKind();
  broad.right.kind = NicKind();
  Rule narrow = MakeRule("narrow", RuleOutcome::Compatible);
  narrow.left.kind = NicKind();
  narrow.left.version = VersionRange::Parse("^1.0.0").value();
  narrow.right.kind = NicKind();
  narrow.right.version = VersionRange::Parse("^1.0.0").value();
  document.rules.push_back(broad);
  document.rules.push_back(narrow);
  document.Normalize();
  auto registry = RegistryGeneration::Compile(document);
  CHECK_OK(registry);
  const Decision decision =
      registry.value()->EvaluatePair(MakeNic("a", "1.4.0"), MakeNic("b", "1.9.0"));
  CHECK(decision.IsCompatible());
  CHECK_EQ(decision.deciding_rule->id.value(), std::string("narrow"));
  CHECK(ComputeSpecificity(document.rules[1]) > ComputeSpecificity(document.rules[0]));
}

FCR_TEST(engine, skip_policies_fall_through_to_lower_rules) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule conditional = MakeRule("conditional", RuleOutcome::Incompatible);
  conditional.priority = RulePriority(10);
  conditional.on_unmet = OnUnmetRequirement::Skip;
  conditional.on_indeterminate = OnIndeterminate::Skip;
  conditional.left.kind = NicKind();
  conditional.right.kind = NicKind();
  conditional.constraints.push_back(RequiresCapability{Side::Left, RoceCapability()});
  Rule fallback = MakeRule("fallback", RuleOutcome::Compatible);
  fallback.priority = RulePriority(0);
  fallback.left.kind = NicKind();
  fallback.right.kind = NicKind();
  document.rules.push_back(conditional);
  document.rules.push_back(fallback);
  document.Normalize();
  auto registry = RegistryGeneration::Compile(document);
  CHECK_OK(registry);
  const Decision decision =
      registry.value()->EvaluatePair(MakeNic("a", "1.0.0"), MakeNic("b", "1.0.0"));
  CHECK(decision.IsCompatible());
  CHECK_EQ(decision.deciding_rule->id.value(), std::string("fallback"));
  CHECK_EQ(decision.matched_rules.front().status, RuleEvaluationStatus::Skipped);
}

FCR_TEST(engine, all_skipped_is_unknown) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule conditional = MakeRule("only-conditional", RuleOutcome::Incompatible);
  conditional.on_unmet = OnUnmetRequirement::Skip;
  conditional.on_indeterminate = OnIndeterminate::Skip;
  conditional.left.kind = NicKind();
  conditional.right.kind = NicKind();
  conditional.constraints.push_back(RequiresCapability{Side::Left, RoceCapability()});
  document.rules.push_back(conditional);
  document.Normalize();
  auto registry = RegistryGeneration::Compile(document);
  CHECK_OK(registry);
  const Decision decision =
      registry.value()->EvaluatePair(MakeNic("a", "1.0.0"), MakeNic("b", "1.0.0"));
  CHECK(decision.IsUnknown());
  CHECK_EQ(decision.reason, DecisionReason::AllMatchedRulesSkipped);
}

FCR_TEST(engine, explanation_renders_every_required_section) {
  const auto registry = CompileStandard();
  ComponentSpec left = MakeNic("nic-a", "2.4.1");
  ComponentSpec right = NicWithRoce("nic-b", "2.4.1", "2.0.0");
  const Decision decision = registry->EvaluatePair(left, right);
  const std::string text = decision.narrative;
  CHECK(text.find("decision ") != std::string::npos);
  CHECK(text.find("outcome:") != std::string::npos);
  CHECK(text.find("generation:") != std::string::npos);
  CHECK(text.find("matched_rules:") != std::string::npos);
  CHECK(text.find("deciding_rule:") != std::string::npos);
  CHECK(text.find("unmet_requirements:") != std::string::npos);
  CHECK(text.find("negotiation:") != std::string::npos);
  CHECK(text.find("narrative:") == std::string::npos);
  const JsonValue json = DecisionToJson(decision);
  CHECK(json.Find("matched_rules") != nullptr);
  CHECK(json.Find("negotiation") != nullptr);
  CHECK(json.Find("generation") != nullptr);
  CHECK(json.Find("decision_id") != nullptr);
}
