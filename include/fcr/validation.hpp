// Fabric Compatibility Registry - Summon Software Labs
// Static validation of a registry generation. Errors block publication.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fcr/document.hpp"
#include "fcr/error.hpp"
#include "fcr/identity.hpp"
#include "fcr/json.hpp"
#include "fcr/rule.hpp"

namespace fcr {

enum class Severity : std::uint8_t { Error, Warning, Info };
std::string_view SeverityName(Severity severity);

enum class DiagnosticCode : std::uint8_t {
  UndefinedFamily,
  UndefinedKind,
  UndefinedProtocol,
  UndefinedSchema,
  UndefinedCapability,
  UndefinedHardwareClass,
  UndefinedFeature,
  KindFamilyMismatch,
  DuplicateRuleIdentity,
  ConflictingRules,
  ImpossibleVersionRange,
  UnsatisfiableConstraints,
  CapabilityTypeMismatch,
  UnsupportedComparison,
  UnreachableRule,
  ImplicationCycle,
  SelfImplication,
  SetRuleUsesRightSide,
  SetRuleDeclaresRightSelector,
  PairRuleMissingRightSelector,
  DeprecatedWithoutSuccessor,
  SupersededByMissing,
  RetiredRuleDeclared,
  EmptySelector,
  UnusedCapability,
  UnreferencedTaxonomyEntry,
  NegotiationNeverNegotiates,
  RuleCountLimit,
  ReachabilityAnalysisSkipped,
};

std::string_view DiagnosticCodeName(DiagnosticCode code);

struct Diagnostic {
  Severity severity = Severity::Error;
  DiagnosticCode code = DiagnosticCode::DuplicateRuleIdentity;
  std::string message;
  std::optional<RuleRef> rule;
  std::string path;
};

struct ValidationOptions {
  // Enables region-coverage analysis for unreachable rules. When disabled (or
  // when the analysis would exceed its budget) only direct subsumption is
  // reported and a diagnostic records that the analysis was approximate.
  bool analyze_reachability = true;
  std::size_t max_reachability_candidates = 64;
  std::size_t max_reachability_cells = 4096;
  bool analyze_implication_cycles = true;
  std::size_t max_reported_cycles = 32;
  std::size_t max_diagnostics = 512;
  // Declared capabilities that no rule references are reported as info.
  bool report_unused_capabilities = true;
};

struct ValidationReport {
  std::vector<Diagnostic> diagnostics;
  std::size_t error_count = 0;
  std::size_t warning_count = 0;
  std::size_t info_count = 0;
  // True when the reachability analysis was able to run exhaustively.
  bool reachability_exhaustive = true;

  // A generation may only be published when it has no errors.
  bool Publishable() const { return error_count == 0; }
  bool HasErrors() const { return error_count != 0; }

  void Add(Diagnostic diagnostic);
  std::string Render() const;
  JsonValue ToJson() const;
};

ValidationReport ValidateRegistryDocument(const RegistryDocument& document,
                                          const ValidationOptions& options = ValidationOptions{});

// Region-level predicates reused by the decision engine and by tests.
bool RuleRegionsOverlap(const Rule& a, const Rule& b);
bool RuleRegionSubsumes(const Rule& outer, const Rule& inner);

}  // namespace fcr
