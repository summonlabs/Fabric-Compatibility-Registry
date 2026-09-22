#include <string>
#include <vector>

#include "fixtures.hpp"

using namespace fcr;
using namespace fcr::test;
using fcr::test::Rng;

namespace {

ValidationReport Validate(const RegistryDocument& document) {
  return ValidateRegistryDocument(document);
}

bool HasCode(const ValidationReport& report, DiagnosticCode code) {
  for (const Diagnostic& diagnostic : report.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::size_t CountCode(const ValidationReport& report, DiagnosticCode code) {
  std::size_t count = 0;
  for (const Diagnostic& diagnostic : report.diagnostics) {
    if (diagnostic.code == code) ++count;
  }
  return count;
}

}  // namespace

FCR_TEST(validation, standard_document_publishes) {
  const RegistryDocument document = MakeStandardDocument();
  const ValidationReport report = Validate(document);
  CHECK_PUBLISHABLE(report);
  CHECK_EQ(report.error_count, std::size_t{0});
  CHECK_FALSE(report.diagnostics.empty());
}

FCR_TEST(validation, duplicate_rule_identity_is_rejected) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.push_back(document.rules.front());
  document.rules.back().revision = RuleRevision(2);
  const ValidationReport report = Validate(document);
  CHECK_NOT_PUBLISHABLE(report);
  CHECK(HasCode(report, DiagnosticCode::DuplicateRuleIdentity));
}

FCR_TEST(validation, same_tier_overlapping_disagreement_is_a_conflict) {
  RegistryDocument document = MakeStandardDocument();
  Rule first = MakeRule("conflict-first", RuleOutcome::Compatible);
  first.priority = RulePriority(5);
  first.left.kind = NicKind();
  first.right.kind = NicKind();
  Rule second = first;
  second.id = RuleId::Parse("conflict-second").value();
  second.outcome = RuleOutcome::Incompatible;
  RegistryDocument conflict = MakeStandardDocument();
  conflict.rules.clear();
  conflict.rules.push_back(first);
  conflict.rules.push_back(second);
  conflict.Normalize();
  const ValidationReport report = Validate(conflict);
  CHECK_NOT_PUBLISHABLE(report);
  CHECK(HasCode(report, DiagnosticCode::ConflictingRules));

  // The same disagreement in different tiers is resolved by precedence. The
  // higher rule is narrower, so the lower rule stays reachable and the pair is
  // a legitimate override rather than a conflict.
  RegistryDocument resolved = MakeStandardDocument();
  resolved.rules.clear();
  resolved.rules.push_back(first);
  Rule higher = second;
  higher.priority = RulePriority(6);
  higher.left.version = VersionRange::Parse("^2.0.0").value();
  resolved.rules.push_back(higher);
  resolved.Normalize();
  const ValidationReport resolved_report = Validate(resolved);
  CHECK_PUBLISHABLE(resolved_report);
  CHECK_FALSE(HasCode(resolved_report, DiagnosticCode::ConflictingRules));
}

FCR_TEST(validation, disjoint_same_tier_rules_do_not_conflict) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule first = MakeRule("range-a", RuleOutcome::Compatible);
  first.symmetry = RuleSymmetry::LeftToRight;
  first.left.kind = NicKind();
  first.left.version = VersionRange::Parse("^1.0.0").value();
  first.right.kind = NicKind();
  Rule second = MakeRule("range-b", RuleOutcome::Incompatible);
  second.symmetry = RuleSymmetry::LeftToRight;
  second.left.kind = NicKind();
  second.left.version = VersionRange::Parse("^2.0.0").value();
  second.right.kind = NicKind();
  document.rules.push_back(first);
  document.rules.push_back(second);
  document.Normalize();
  const ValidationReport report = Validate(document);
  CHECK_PUBLISHABLE(report);
  CHECK_FALSE(HasCode(report, DiagnosticCode::ConflictingRules));
}

FCR_TEST(validation, impossible_ranges_and_windows_are_rejected) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule rule = MakeRule("impossible-selector", RuleOutcome::Compatible);
  rule.left.kind = NicKind();
  rule.left.version = VersionRange::Parse(">=2.0.0 <1.0.0").value();
  rule.right.kind = NicKind();
  document.rules.push_back(rule);
  const ValidationReport report = Validate(document);
  CHECK_NOT_PUBLISHABLE(report);
  CHECK(HasCode(report, DiagnosticCode::ImpossibleVersionRange));
}

FCR_TEST(validation, unsatisfiable_constraints_are_rejected) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule rule = MakeRule("self-contradicting", RuleOutcome::Compatible);
  rule.left.kind = NicKind();
  rule.right.kind = NicKind();
  rule.constraints.push_back(RequiresCapability{Side::Left, RoceCapability()});
  rule.constraints.push_back(ForbidsCapability{Side::Left, RoceCapability()});
  document.rules.push_back(rule);
  const ValidationReport report = Validate(document);
  CHECK_NOT_PUBLISHABLE(report);
  CHECK(HasCode(report, DiagnosticCode::UnsatisfiableConstraints));

  RegistryDocument window = MakeStandardDocument();
  window.rules.clear();
  Rule inverted = MakeRule("inverted-window", RuleOutcome::Compatible);
  inverted.left.kind = NicKind();
  inverted.right.kind = NicKind();
  inverted.constraints.push_back(VersionAtLeast{Side::Right, SemVersion(5, 0, 0)});
  inverted.constraints.push_back(VersionAtMost{Side::Right, SemVersion(4, 0, 0)});
  window.rules.push_back(inverted);
  const ValidationReport window_report = Validate(window);
  CHECK_NOT_PUBLISHABLE(window_report);
  CHECK(HasCode(window_report, DiagnosticCode::UnsatisfiableConstraints));
}

FCR_TEST(validation, undefined_references_are_rejected) {
  struct Case {
    const char* label;
    std::function<void(Rule&)> mutate;
    DiagnosticCode code;
  };
  const Case cases[] = {
      {"family", [](Rule& rule) { rule.left.family = ComponentFamilyId::Parse("ghost.family").value(); },
       DiagnosticCode::UndefinedFamily},
      {"kind", [](Rule& rule) { rule.left.kind = ComponentKindId::Parse("ghost.kind").value(); },
       DiagnosticCode::UndefinedKind},
      {"hardware",
       [](Rule& rule) { rule.left.hardware_class = HardwareClassId::Parse("ghost.hw").value(); },
       DiagnosticCode::UndefinedHardwareClass},
      {"capability",
       [](Rule& rule) {
         rule.constraints.push_back(
             RequiresCapability{Side::Left, CapabilityId::Parse("ghost.cap").value()});
       },
       DiagnosticCode::UndefinedCapability},
      {"feature",
       [](Rule& rule) {
         rule.constraints.push_back(
             RequiresFeature{Side::Left, FeatureId::Parse("ghost.feature").value()});
       },
       DiagnosticCode::UndefinedFeature},
      {"protocol",
       [](Rule& rule) {
         rule.constraints.push_back(
             RequiresProtocol{Side::Left, ProtocolId::Parse("ghost.proto").value(),
                              VersionRange::Any()});
       },
       DiagnosticCode::UndefinedProtocol},
      {"schema",
       [](Rule& rule) {
         rule.constraints.push_back(RequiresSchema{Side::Left,
                                                    SchemaId::Parse("ghost.schema").value(),
                                                    VersionRange::Any()});
       },
       DiagnosticCode::UndefinedSchema},
  };
  for (const Case& item : cases) {
    RegistryDocument document = MakeStandardDocument();
    document.rules.clear();
    Rule rule = MakeRule("reference-check", RuleOutcome::Compatible);
    rule.left.kind = NicKind();
    rule.right.kind = NicKind();
    item.mutate(rule);
    document.rules.push_back(rule);
    const ValidationReport report = Validate(document);
    if (report.Publishable() || !HasCode(report, item.code)) {
      FCR_FAIL("undefined " << item.label << " reference was not reported");
    }
  }
}

FCR_TEST(validation, capability_type_and_operator_are_checked) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule rule = MakeRule("type-check", RuleOutcome::Compatible);
  rule.left.kind = NicKind();
  rule.right.kind = NicKind();
  // link.lanes is an integer capability; a text literal cannot type it.
  rule.constraints.push_back(RequiresCapabilityValue{Side::Left, LanesCapability(),
                                                     ComparisonOp::Ge,
                                                     ConstraintLiteral::Text("many")});
  document.rules.push_back(rule);
  const ValidationReport report = Validate(document);
  CHECK_NOT_PUBLISHABLE(report);
  CHECK(HasCode(report, DiagnosticCode::CapabilityTypeMismatch));

  RegistryDocument operator_document = MakeStandardDocument();
  operator_document.rules.clear();
  Rule flag_rule = MakeRule("operator-check", RuleOutcome::Compatible);
  flag_rule.left.kind = NicKind();
  flag_rule.right.kind = NicKind();
  // A flag capability only supports equality.
  flag_rule.constraints.push_back(RequiresCapabilityValue{
      Side::Left, RoceCapability(), ComparisonOp::Gt, ConstraintLiteral::Boolean(true)});
  operator_document.rules.push_back(flag_rule);
  const ValidationReport operator_report = Validate(operator_document);
  CHECK_NOT_PUBLISHABLE(operator_report);
  CHECK(HasCode(operator_report, DiagnosticCode::UnsupportedComparison));
}

FCR_TEST(validation, kind_family_mismatch_is_rejected) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule rule = MakeRule("family-mismatch", RuleOutcome::Compatible);
  rule.left.family = ComputeFamily();
  rule.left.kind = NicKind();  // rdma.nic belongs to fabric.transport
  rule.right.kind = NicKind();
  document.rules.push_back(rule);
  const ValidationReport report = Validate(document);
  CHECK_NOT_PUBLISHABLE(report);
  CHECK(HasCode(report, DiagnosticCode::KindFamilyMismatch));
}

FCR_TEST(validation, unreachable_rule_is_reported) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule broad = MakeRule("broad", RuleOutcome::Compatible);
  broad.priority = RulePriority(10);
  broad.left.kind = NicKind();
  broad.right.kind = NicKind();
  Rule narrow = MakeRule("narrow", RuleOutcome::Compatible);
  narrow.priority = RulePriority(0);
  narrow.left.kind = NicKind();
  narrow.left.version = VersionRange::Parse("^2.0.0").value();
  narrow.right.kind = NicKind();
  narrow.right.version = VersionRange::Parse("^1.0.0").value();
  document.rules.push_back(broad);
  document.rules.push_back(narrow);
  document.Normalize();
  const ValidationReport report = Validate(document);
  CHECK_NOT_PUBLISHABLE(report);
  CHECK(HasCode(report, DiagnosticCode::UnreachableRule));
}

FCR_TEST(validation, unreachable_rule_is_detected_across_a_union_of_regions) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule left_cover = MakeRule("cover-left", RuleOutcome::Compatible);
  left_cover.priority = RulePriority(10);
  left_cover.left.kind = NicKind();
  left_cover.left.version = VersionRange::Parse("<2.0.0").value();
  left_cover.right.kind = NicKind();
  Rule right_cover = MakeRule("cover-right", RuleOutcome::Compatible);
  right_cover.priority = RulePriority(10);
  right_cover.left.kind = NicKind();
  right_cover.left.version = VersionRange::Parse(">=2.0.0").value();
  right_cover.right.kind = NicKind();
  Rule covered = MakeRule("covered", RuleOutcome::Compatible);
  covered.priority = RulePriority(0);
  covered.left.kind = NicKind();
  covered.right.kind = NicKind();
  document.rules.push_back(left_cover);
  document.rules.push_back(right_cover);
  document.rules.push_back(covered);
  document.Normalize();
  const ValidationReport report = Validate(document);
  CHECK_NOT_PUBLISHABLE(report);
  CHECK(HasCode(report, DiagnosticCode::UnreachableRule));
  CHECK(report.reachability_exhaustive);
}

FCR_TEST(validation, partially_covered_rule_is_reachable) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule partial = MakeRule("partial-cover", RuleOutcome::Compatible);
  partial.priority = RulePriority(10);
  partial.left.kind = NicKind();
  partial.left.version = VersionRange::Parse("<2.0.0").value();
  partial.right.kind = NicKind();
  Rule target = MakeRule("target", RuleOutcome::Compatible);
  target.priority = RulePriority(0);
  target.left.kind = NicKind();
  target.right.kind = NicKind();
  document.rules.push_back(partial);
  document.rules.push_back(target);
  document.Normalize();
  const ValidationReport report = Validate(document);
  CHECK_PUBLISHABLE(report);
  CHECK_FALSE(HasCode(report, DiagnosticCode::UnreachableRule));
}

FCR_TEST(validation, skipped_policies_do_not_shadow) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule skipper = MakeRule("skipper", RuleOutcome::Compatible);
  skipper.priority = RulePriority(10);
  skipper.on_unmet = OnUnmetRequirement::Skip;
  skipper.on_indeterminate = OnIndeterminate::Skip;
  skipper.left.kind = NicKind();
  skipper.right.kind = NicKind();
  Rule target = MakeRule("below-skipper", RuleOutcome::Incompatible);
  target.priority = RulePriority(0);
  target.left.kind = NicKind();
  target.right.kind = NicKind();
  document.rules.push_back(skipper);
  document.rules.push_back(target);
  document.Normalize();
  const ValidationReport report = Validate(document);
  CHECK_PUBLISHABLE(report);
  CHECK_FALSE(HasCode(report, DiagnosticCode::UnreachableRule));
}

FCR_TEST(validation, implication_cycles_are_rejected) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule rule = MakeRule("cycle", RuleOutcome::Compatible);
  rule.left.kind = NicKind();
  rule.right.kind = NicKind();
  rule.implications.push_back(CapabilityImplication{Side::Left, RoceCapability(), LanesCapability()});
  rule.implications.push_back(CapabilityImplication{Side::Left, LanesCapability(), RoceCapability()});
  document.rules.push_back(rule);
  const ValidationReport report = Validate(document);
  CHECK_NOT_PUBLISHABLE(report);
  CHECK(HasCode(report, DiagnosticCode::ImplicationCycle));

  RegistryDocument self = MakeStandardDocument();
  self.rules.clear();
  Rule self_rule = MakeRule("self", RuleOutcome::Compatible);
  self_rule.left.kind = NicKind();
  self_rule.right.kind = NicKind();
  self_rule.implications.push_back(
      CapabilityImplication{Side::Left, RoceCapability(), RoceCapability()});
  self.rules.push_back(self_rule);
  const ValidationReport self_report = Validate(self);
  CHECK_NOT_PUBLISHABLE(self_report);
  CHECK(HasCode(self_report, DiagnosticCode::SelfImplication));
}

FCR_TEST(validation, acyclic_implications_are_accepted) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule rule = MakeRule("chain", RuleOutcome::Compatible);
  rule.left.kind = NicKind();
  rule.right.kind = NicKind();
  rule.implications.push_back(CapabilityImplication{Side::Left, RoceCapability(), LanesCapability()});
  rule.implications.push_back(CapabilityImplication{Side::Left, LanesCapability(), ApiLevelCapability()});
  document.rules.push_back(rule);
  const ValidationReport report = Validate(document);
  CHECK_PUBLISHABLE(report);
  CHECK_FALSE(HasCode(report, DiagnosticCode::ImplicationCycle));
}

FCR_TEST(validation, set_rules_may_not_use_the_right_side) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule rule = MakeRule("bad-set-rule", RuleOutcome::Compatible);
  rule.arity = RuleArity::Set;
  rule.left.family = TransportFamily();
  rule.constraints.push_back(RequiresCapability{Side::Right, RoceCapability()});
  document.rules.push_back(rule);
  const ValidationReport report = Validate(document);
  CHECK_NOT_PUBLISHABLE(report);
  CHECK(HasCode(report, DiagnosticCode::SetRuleUsesRightSide));
}

FCR_TEST(validation, lifecycle_consistency_is_reported) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule deprecated = MakeRule("deprecated-rule", RuleOutcome::Compatible);
  deprecated.left.kind = NicKind();
  deprecated.right.kind = NicKind();
  deprecated.lifecycle.state = LifecycleState::Deprecated;
  deprecated.lifecycle.superseded_by = RuleRef{RuleId::Parse("missing-rule").value(), RuleRevision(1)};
  Rule orphan = MakeRule("orphan-deprecated-rule", RuleOutcome::Compatible);
  orphan.left.kind = GpuKind();
  orphan.right.kind = GpuKind();
  orphan.lifecycle.state = LifecycleState::Deprecated;
  Rule retired = MakeRule("retired-rule", RuleOutcome::Incompatible);
  retired.left.kind = SwitchKind();
  retired.right.kind = SwitchKind();
  retired.lifecycle.state = LifecycleState::Retired;
  document.rules.push_back(deprecated);
  document.rules.push_back(orphan);
  document.rules.push_back(retired);
  document.Normalize();
  const ValidationReport report = Validate(document);
  CHECK_PUBLISHABLE(report);
  CHECK(HasCode(report, DiagnosticCode::DeprecatedWithoutSuccessor));
  CHECK(HasCode(report, DiagnosticCode::SupersededByMissing));
  CHECK(HasCode(report, DiagnosticCode::RetiredRuleDeclared));
}

FCR_TEST(validation, report_rendering_is_deterministic) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  Rule rule = MakeRule("broken", RuleOutcome::Compatible);
  rule.left.family = ComponentFamilyId::Parse("ghost").value();
  rule.constraints.push_back(
      RequiresCapability{Side::Left, CapabilityId::Parse("ghost.cap").value()});
  document.rules.push_back(rule);
  const ValidationReport first = Validate(document);
  const ValidationReport second = Validate(document);
  CHECK_EQ(first.Render(), second.Render());
  CHECK_EQ(first.ToJson().Dump(false), second.ToJson().Dump(false));
  CHECK(CountCode(first, DiagnosticCode::UndefinedFamily) == 1);
}
