#include "fcr/validation.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fcr {
namespace {

constexpr std::size_t kMaxReachabilityRules = 2048;

struct RuleBox {
  ComponentSelector left;
  ComponentSelector right;
};

void RuleOrientations(const Rule& rule, std::vector<RuleBox>& out) {
  if (rule.arity == RuleArity::Set) {
    out.push_back(RuleBox{rule.left, ComponentSelector{}});
    return;
  }
  switch (rule.symmetry) {
    case RuleSymmetry::LeftToRight:
      out.push_back(RuleBox{rule.left, rule.right});
      break;
    case RuleSymmetry::RightToLeft:
      out.push_back(RuleBox{rule.right, rule.left});
      break;
    case RuleSymmetry::Symmetric:
      out.push_back(RuleBox{rule.left, rule.right});
      out.push_back(RuleBox{rule.right, rule.left});
      break;
  }
}

bool BoxOverlaps(const RuleBox& a, const RuleBox& b, bool pair) {
  if (!pair) return SelectorsOverlap(a.left, b.left);
  return SelectorsOverlap(a.left, b.left) && SelectorsOverlap(a.right, b.right);
}

// Non-version subsumption: the version dimension is deliberately excluded
// because coverage of the version plane is decided separately by
// VersionPlaneCovered, and a bounded rule can cover a wildcard rule's version
// plane in pieces.
bool SelectorSubsumesModuloVersion(const ComponentSelector& outer,
                                   const ComponentSelector& inner) {
  const auto id_subsumes = [](const auto& outer_id, const auto& inner_id) {
    if (!outer_id.has_value()) return true;
    if (!inner_id.has_value()) return false;
    return *outer_id == *inner_id;
  };
  return id_subsumes(outer.family, inner.family) && id_subsumes(outer.kind, inner.kind) &&
         id_subsumes(outer.hardware_class, inner.hardware_class);
}

bool BoxSubsumesModuloVersion(const RuleBox& outer, const RuleBox& inner, bool pair) {
  if (!pair) return SelectorSubsumesModuloVersion(outer.left, inner.left);
  return SelectorSubsumesModuloVersion(outer.left, inner.left) &&
         SelectorSubsumesModuloVersion(outer.right, inner.right);
}

bool RuleRegionSubsumesModuloVersion(const Rule& outer, const Rule& inner) {
  if (outer.arity != inner.arity) return false;
  const bool pair = outer.arity == RuleArity::Pair;
  std::vector<RuleBox> outer_boxes;
  std::vector<RuleBox> inner_boxes;
  RuleOrientations(outer, outer_boxes);
  RuleOrientations(inner, inner_boxes);
  for (const RuleBox& inner_box : inner_boxes) {
    bool covered = false;
    for (const RuleBox& outer_box : outer_boxes) {
      if (BoxSubsumesModuloVersion(outer_box, inner_box, pair)) {
        covered = true;
        break;
      }
    }
    if (!covered) return false;
  }
  return true;
}

bool BoxSubsumes(const RuleBox& outer, const RuleBox& inner, bool pair) {
  if (!pair) return SelectorSubsumes(outer.left, inner.left);
  return SelectorSubsumes(outer.left, inner.left) && SelectorSubsumes(outer.right, inner.right);
}

// ---------------------------------------------------------------------------
// Version-plane coverage
// ---------------------------------------------------------------------------
struct Coord {
  // 0 = negative infinity, 1 = finite, 2 = positive infinity.
  int kind = 1;
  SemVersion version;
};

int CompareCoord(const Coord& a, const Coord& b) {
  if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
  if (a.kind != 1) return 0;
  const std::strong_ordering cmp = a.version.Precedence(b.version);
  if (cmp == std::strong_ordering::less) return -1;
  if (cmp == std::strong_ordering::greater) return 1;
  return 0;
}

void AddCoord(std::vector<Coord>& coords, const Coord& candidate) {
  for (const Coord& existing : coords) {
    if (CompareCoord(existing, candidate) == 0) return;
  }
  coords.push_back(candidate);
}

void AddRangeCoords(std::vector<Coord>& coords, const std::optional<VersionRange>& range) {
  if (!range.has_value()) return;
  for (const VersionInterval& interval : range->intervals()) {
    if (interval.low.has_value()) AddCoord(coords, Coord{1, *interval.low});
    if (interval.high.has_value()) AddCoord(coords, Coord{1, *interval.high});
  }
}

// True when every version strictly between lo and hi is inside one of the
// range's intervals. Cell boundaries are always drawn from the union of all
// candidate and target bounds, so no bound can fall strictly inside a cell.
bool RangeCoversCell(const std::optional<VersionRange>& range, const Coord& lo, const Coord& hi) {
  if (lo.kind == 2 || hi.kind == 0) return true;  // degenerate cell, vacuously covered
  if (!range.has_value()) return true;            // wildcard matches everything
  for (const VersionInterval& interval : range->intervals()) {
    bool low_ok = true;
    if (interval.low.has_value()) {
      if (lo.kind == 0) {
        low_ok = false;
      } else {
        low_ok = !(lo.version < *interval.low);
      }
    }
    bool high_ok = true;
    if (interval.high.has_value()) {
      if (hi.kind == 2) {
        high_ok = false;
      } else {
        high_ok = !(*interval.high < hi.version);
      }
    }
    if (low_ok && high_ok) return true;
  }
  return false;
}

struct CoverageResult {
  bool covered = false;
  bool exhaustive = true;
};

// Determines whether the union of candidate boxes covers the target box across
// the two version dimensions. Non-version dimensions must already have been
// checked by the caller.
CoverageResult VersionPlaneCovered(const RuleBox& target,
                                   const std::vector<RuleBox>& candidates,
                                   std::size_t max_cells) {
  std::vector<Coord> xs{Coord{0, SemVersion{}}, Coord{2, SemVersion{}}};
  std::vector<Coord> ys{Coord{0, SemVersion{}}, Coord{2, SemVersion{}}};
  AddRangeCoords(xs, target.left.version);
  AddRangeCoords(ys, target.right.version);
  for (const RuleBox& candidate : candidates) {
    AddRangeCoords(xs, candidate.left.version);
    AddRangeCoords(ys, candidate.right.version);
  }
  std::sort(xs.begin(), xs.end(), [](const Coord& a, const Coord& b) {
    return CompareCoord(a, b) < 0;
  });
  std::sort(ys.begin(), ys.end(), [](const Coord& a, const Coord& b) {
    return CompareCoord(a, b) < 0;
  });
  const std::size_t cells = (xs.size() - 1) * (ys.size() - 1);
  if (cells > max_cells) {
    CoverageResult result;
    result.covered = false;
    result.exhaustive = false;
    return result;
  }
  for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
    for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
      if (!RangeCoversCell(target.left.version, xs[i], xs[i + 1])) continue;
      if (!RangeCoversCell(target.right.version, ys[j], ys[j + 1])) continue;
      bool cell_covered = false;
      for (const RuleBox& candidate : candidates) {
        if (RangeCoversCell(candidate.left.version, xs[i], xs[i + 1]) &&
            RangeCoversCell(candidate.right.version, ys[j], ys[j + 1])) {
          cell_covered = true;
          break;
        }
      }
      if (!cell_covered) {
        CoverageResult result;
        result.covered = false;
        result.exhaustive = true;
        return result;
      }
    }
  }
  CoverageResult result;
  result.covered = true;
  result.exhaustive = true;
  return result;
}

// A rule can only be shadowed by a rule that always reaches a verdict, i.e.
// one whose policies never skip it.
bool RuleAlwaysApplies(const Rule& rule) {
  return rule.on_unmet != OnUnmetRequirement::Skip && rule.on_indeterminate != OnIndeterminate::Skip;
}

struct TierKey {
  std::int32_t priority = 0;
  int specificity = 0;
  int origin_rank = 0;

  friend bool operator<(const TierKey& a, const TierKey& b) {
    if (a.priority != b.priority) return a.priority < b.priority;
    if (a.specificity != b.specificity) return a.specificity < b.specificity;
    return a.origin_rank < b.origin_rank;
  }
  friend bool operator==(const TierKey& a, const TierKey& b) {
    return a.priority == b.priority && a.specificity == b.specificity &&
           a.origin_rank == b.origin_rank;
  }
};

TierKey MakeTierKey(const Rule& rule) {
  const PrecedenceTier tier = RuleTier(rule);
  TierKey key;
  key.priority = tier.priority.value();
  key.specificity = tier.specificity;
  key.origin_rank = tier.origin_rank;
  return key;
}

Diagnostic MakeDiagnostic(Severity severity, DiagnosticCode code, std::string message,
                          std::string path) {
  Diagnostic diagnostic;
  diagnostic.severity = severity;
  diagnostic.code = code;
  diagnostic.message = std::move(message);
  diagnostic.path = std::move(path);
  return diagnostic;
}

}  // namespace

std::string_view SeverityName(Severity severity) {
  switch (severity) {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    case Severity::Info: return "info";
  }
  return "error";
}

std::string_view DiagnosticCodeName(DiagnosticCode code) {
  switch (code) {
    case DiagnosticCode::UndefinedFamily: return "undefined_family";
    case DiagnosticCode::UndefinedKind: return "undefined_kind";
    case DiagnosticCode::UndefinedProtocol: return "undefined_protocol";
    case DiagnosticCode::UndefinedSchema: return "undefined_schema";
    case DiagnosticCode::UndefinedCapability: return "undefined_capability";
    case DiagnosticCode::UndefinedHardwareClass: return "undefined_hardware_class";
    case DiagnosticCode::UndefinedFeature: return "undefined_feature";
    case DiagnosticCode::KindFamilyMismatch: return "kind_family_mismatch";
    case DiagnosticCode::DuplicateRuleIdentity: return "duplicate_rule_identity";
    case DiagnosticCode::ConflictingRules: return "conflicting_rules";
    case DiagnosticCode::ImpossibleVersionRange: return "impossible_version_range";
    case DiagnosticCode::UnsatisfiableConstraints: return "unsatisfiable_constraints";
    case DiagnosticCode::CapabilityTypeMismatch: return "capability_type_mismatch";
    case DiagnosticCode::UnsupportedComparison: return "unsupported_comparison";
    case DiagnosticCode::UnreachableRule: return "unreachable_rule";
    case DiagnosticCode::ImplicationCycle: return "implication_cycle";
    case DiagnosticCode::SelfImplication: return "self_implication";
    case DiagnosticCode::SetRuleUsesRightSide: return "set_rule_uses_right_side";
    case DiagnosticCode::SetRuleDeclaresRightSelector: return "set_rule_declares_right_selector";
    case DiagnosticCode::PairRuleMissingRightSelector: return "pair_rule_missing_right_selector";
    case DiagnosticCode::DeprecatedWithoutSuccessor: return "deprecated_without_successor";
    case DiagnosticCode::SupersededByMissing: return "superseded_by_missing";
    case DiagnosticCode::RetiredRuleDeclared: return "retired_rule_declared";
    case DiagnosticCode::EmptySelector: return "empty_selector";
    case DiagnosticCode::UnusedCapability: return "unused_capability";
    case DiagnosticCode::UnreferencedTaxonomyEntry: return "unreferenced_taxonomy_entry";
    case DiagnosticCode::NegotiationNeverNegotiates: return "negotiation_never_negotiates";
    case DiagnosticCode::RuleCountLimit: return "rule_count_limit";
    case DiagnosticCode::ReachabilityAnalysisSkipped: return "reachability_analysis_skipped";
  }
  return "unknown";
}

void ValidationReport::Add(Diagnostic diagnostic) {
  switch (diagnostic.severity) {
    case Severity::Error: ++error_count; break;
    case Severity::Warning: ++warning_count; break;
    case Severity::Info: ++info_count; break;
  }
  diagnostics.push_back(std::move(diagnostic));
}

std::string ValidationReport::Render() const {
  std::string out;
  out.append("validation: ");
  out.append(std::to_string(error_count));
  out.append(" error(s), ");
  out.append(std::to_string(warning_count));
  out.append(" warning(s), ");
  out.append(std::to_string(info_count));
  out.append(" info");
  if (!reachability_exhaustive) out.append(" (reachability analysis was approximate)");
  out.push_back('\n');
  for (const Diagnostic& diagnostic : diagnostics) {
    out.append("  [");
    out.append(SeverityName(diagnostic.severity));
    out.append("] ");
    out.append(DiagnosticCodeName(diagnostic.code));
    out.push_back(' ');
    out.append(diagnostic.path);
    if (diagnostic.rule.has_value()) {
      out.append(" rule=");
      out.append(diagnostic.rule->ToString());
    }
    out.append(": ");
    out.append(diagnostic.message);
    out.push_back('\n');
  }
  return out;
}

JsonValue ValidationReport::ToJson() const {
  JsonValue out = JsonValue::Obj();
  out.Set("publishable", JsonValue::Bool(Publishable()));
  out.Set("errors", JsonValue::UInt(error_count));
  out.Set("warnings", JsonValue::UInt(warning_count));
  out.Set("infos", JsonValue::UInt(info_count));
  out.Set("reachability_exhaustive", JsonValue::Bool(reachability_exhaustive));
  JsonValue::Array array;
  array.reserve(diagnostics.size());
  for (const Diagnostic& diagnostic : diagnostics) {
    JsonValue entry = JsonValue::Obj();
    entry.Set("severity", JsonValue::Str(std::string(SeverityName(diagnostic.severity))));
    entry.Set("code", JsonValue::Str(std::string(DiagnosticCodeName(diagnostic.code))));
    entry.Set("path", JsonValue::Str(diagnostic.path));
    entry.Set("message", JsonValue::Str(diagnostic.message));
    if (diagnostic.rule.has_value()) {
      JsonValue rule = JsonValue::Obj();
      rule.Set("id", JsonValue::Str(diagnostic.rule->id.value()));
      rule.Set("revision", JsonValue::UInt(diagnostic.rule->revision.value()));
      entry.Set("rule", std::move(rule));
    }
    array.push_back(std::move(entry));
  }
  out.Set("diagnostics", JsonValue::Arr(std::move(array)));
  return out;
}

bool RuleRegionsOverlap(const Rule& a, const Rule& b) {
  if (a.arity != b.arity) return false;
  const bool pair = a.arity == RuleArity::Pair;
  std::vector<RuleBox> boxes_a;
  std::vector<RuleBox> boxes_b;
  RuleOrientations(a, boxes_a);
  RuleOrientations(b, boxes_b);
  for (const RuleBox& box_a : boxes_a) {
    for (const RuleBox& box_b : boxes_b) {
      if (BoxOverlaps(box_a, box_b, pair)) return true;
    }
  }
  return false;
}

bool RuleRegionSubsumes(const Rule& outer, const Rule& inner) {
  if (outer.arity != inner.arity) return false;
  const bool pair = outer.arity == RuleArity::Pair;
  std::vector<RuleBox> outer_boxes;
  std::vector<RuleBox> inner_boxes;
  RuleOrientations(outer, outer_boxes);
  RuleOrientations(inner, inner_boxes);
  for (const RuleBox& inner_box : inner_boxes) {
    bool covered = false;
    for (const RuleBox& outer_box : outer_boxes) {
      if (BoxSubsumes(outer_box, inner_box, pair)) {
        covered = true;
        break;
      }
    }
    if (!covered) return false;
  }
  return true;
}

ValidationReport ValidateRegistryDocument(const RegistryDocument& document,
                                          const ValidationOptions& options) {
  ValidationReport report;
  const RuleList& rules = document.rules;
  const Taxonomy& taxonomy = document.taxonomy;

  const auto add_rule_diagnostic = [&report](Severity severity, DiagnosticCode code,
                                             std::string message, const Rule& rule,
                                             std::string path) {
    Diagnostic diagnostic = MakeDiagnostic(severity, code, std::move(message), std::move(path));
    diagnostic.rule = rule.Ref();
    report.Add(std::move(diagnostic));
  };

  // --- Taxonomy integrity -------------------------------------------------
  for (const auto& [id, descriptor] : taxonomy.kinds()) {
    if (!taxonomy.HasFamily(descriptor.family)) {
      report.Add(MakeDiagnostic(Severity::Error, DiagnosticCode::KindFamilyMismatch,
                                "component kind '" + id.value() + "' declares family '" +
                                    descriptor.family.value() + "' which is not in the taxonomy",
                                "taxonomy.kinds." + id.value()));
    }
  }

  if (rules.size() > kMaxRulesPerGeneration) {
    report.Add(MakeDiagnostic(Severity::Error, DiagnosticCode::RuleCountLimit,
                              "generation declares more rules than the supported maximum",
                              "rules"));
  }

  // --- Duplicate identities ----------------------------------------------
  {
    std::map<RuleId, std::size_t> seen;
    for (std::size_t i = 0; i < rules.size(); ++i) {
      const auto it = seen.find(rules[i].id);
      if (it != seen.end()) {
        add_rule_diagnostic(Severity::Error, DiagnosticCode::DuplicateRuleIdentity,
                            "rule identifier is declared more than once in this generation "
                            "(first at rules[" + std::to_string(it->second) + "])",
                            rules[i], "rules[" + std::to_string(i) + "]");
      } else {
        seen.emplace(rules[i].id, i);
      }
    }
  }

  // --- Per-rule structural checks ----------------------------------------
  std::map<CapabilityId, std::size_t> capability_references;
  std::set<FeatureId> referenced_features;
  for (std::size_t index = 0; index < rules.size(); ++index) {
    const Rule& rule = rules[index];
    const std::string base = "rules[" + std::to_string(index) + "]";

    if (rule.arity == RuleArity::Set) {
      if (!rule.right.IsWildcard()) {
        add_rule_diagnostic(Severity::Error, DiagnosticCode::SetRuleDeclaresRightSelector,
                            "set-arity rule must not declare a right selector", rule,
                            base + ".right");
      }
    } else if (rule.right.IsWildcard() && rule.left.IsWildcard()) {
      add_rule_diagnostic(Severity::Info, DiagnosticCode::EmptySelector,
                          "pair rule matches every registered component pair", rule, base);
    }

    const ComponentSelector* selectors[2] = {&rule.left, &rule.right};
    const char* selector_names[2] = {"left", "right"};
    const std::size_t selector_count = rule.arity == RuleArity::Pair ? 2u : 1u;
    for (std::size_t s = 0; s < selector_count; ++s) {
      const ComponentSelector& selector = *selectors[s];
      const std::string path = base + "." + selector_names[s];
      if (selector.family.has_value() && !taxonomy.HasFamily(*selector.family)) {
        add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedFamily,
                            "selector references undefined component family '" +
                                selector.family->value() + "'",
                            rule, path + ".family");
      }
      if (selector.kind.has_value()) {
        const KindDescriptor* kind = taxonomy.FindKind(*selector.kind);
        if (kind == nullptr) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedKind,
                              "selector references undefined component kind '" +
                                  selector.kind->value() + "'",
                              rule, path + ".kind");
        } else if (selector.family.has_value() && !(kind->family == *selector.family)) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::KindFamilyMismatch,
                              "selector combines kind '" + selector.kind->value() +
                                  "' with family '" + selector.family->value() + "'",
                              rule, path);
        }
      }
      if (selector.hardware_class.has_value() &&
          !taxonomy.HasHardwareClass(*selector.hardware_class)) {
        add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedHardwareClass,
                            "selector references undefined hardware class '" +
                                selector.hardware_class->value() + "'",
                            rule, path + ".hardware_class");
      }
      if (selector.version.has_value() && selector.version->IsEmpty()) {
        add_rule_diagnostic(Severity::Error, DiagnosticCode::ImpossibleVersionRange,
                            "selector version range matches no version at all", rule,
                            path + ".version");
      }
    }

    // Constraint-level checks.
    std::map<int, std::vector<std::string>> requires_by_side;
    std::map<int, std::vector<std::string>> forbids_by_side;
    for (std::size_t c = 0; c < rule.constraints.size(); ++c) {
      const Constraint& constraint = rule.constraints[c];
      const std::string path = base + ".constraints[" + std::to_string(c) + "]";
      const Side side = ConstraintSide(constraint);
      if (rule.arity == RuleArity::Set && side == Side::Right) {
        add_rule_diagnostic(Severity::Error, DiagnosticCode::SetRuleUsesRightSide,
                            "set-arity rule constraint applies to the right side, which does not "
                            "exist for a set query",
                            rule, path);
      }
      const int side_index = static_cast<int>(side);
      if (const CapabilityId* capability = ConstraintCapability(constraint)) {
        capability_references[*capability] += 1;
        const CapabilityDeclaration* declaration = taxonomy.FindCapability(*capability);
        if (declaration == nullptr) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedCapability,
                              "constraint references undefined capability '" +
                                  capability->value() + "'",
                              rule, path);
        }
      }
      if (const auto* rc = std::get_if<RequiresCapability>(&constraint)) {
        requires_by_side[side_index].push_back("capability:" + rc->capability.value());
      } else if (const auto* fc = std::get_if<ForbidsCapability>(&constraint)) {
        forbids_by_side[side_index].push_back("capability:" + fc->capability.value());
      } else if (const auto* rv = std::get_if<RequiresCapabilityValue>(&constraint)) {
        requires_by_side[side_index].push_back("capability:" + rv->capability.value());
        const CapabilityDeclaration* declaration = taxonomy.FindCapability(rv->capability);
        if (declaration != nullptr) {
          if (!rv->value.MatchesType(declaration->type)) {
            add_rule_diagnostic(Severity::Error, DiagnosticCode::CapabilityTypeMismatch,
                                "constraint literal does not match declared type '" +
                                    std::string(CapabilityTypeName(declaration->type)) +
                                    "' of capability '" + rv->capability.value() + "'",
                                rule, path + ".value");
          }
          if (!ComparisonOpSupported(declaration->type, rv->op)) {
            add_rule_diagnostic(Severity::Error, DiagnosticCode::UnsupportedComparison,
                                "comparison operator '" +
                                    std::string(ComparisonOpName(rv->op)) +
                                    "' is not defined for capability type '" +
                                    std::string(CapabilityTypeName(declaration->type)) + "'",
                                rule, path + ".op");
          }
        }
      } else if (const auto* rp = std::get_if<RequiresProtocol>(&constraint)) {
        requires_by_side[side_index].push_back("protocol:" + rp->protocol.value());
        if (!taxonomy.HasProtocol(rp->protocol)) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedProtocol,
                              "constraint references undefined protocol '" +
                                  rp->protocol.value() + "'",
                              rule, path + ".protocol");
        }
        if (rp->range.IsEmpty()) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::ImpossibleVersionRange,
                              "required protocol version range matches no version", rule,
                              path + ".range");
        }
      } else if (const auto* fp = std::get_if<ForbidsProtocol>(&constraint)) {
        forbids_by_side[side_index].push_back("protocol:" + fp->protocol.value());
        if (!taxonomy.HasProtocol(fp->protocol)) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedProtocol,
                              "constraint references undefined protocol '" +
                                  fp->protocol.value() + "'",
                              rule, path + ".protocol");
        }
      } else if (const auto* rs = std::get_if<RequiresSchema>(&constraint)) {
        requires_by_side[side_index].push_back("schema:" + rs->schema.value());
        if (!taxonomy.HasSchema(rs->schema)) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedSchema,
                              "constraint references undefined schema '" + rs->schema.value() + "'",
                              rule, path + ".schema");
        }
        if (rs->range.IsEmpty()) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::ImpossibleVersionRange,
                              "required schema version range matches no version", rule,
                              path + ".range");
        }
      } else if (const auto* fs = std::get_if<ForbidsSchema>(&constraint)) {
        forbids_by_side[side_index].push_back("schema:" + fs->schema.value());
        if (!taxonomy.HasSchema(fs->schema)) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedSchema,
                              "constraint references undefined schema '" + fs->schema.value() + "'",
                              rule, path + ".schema");
        }
      } else if (const auto* rh = std::get_if<RequiresHardwareClass>(&constraint)) {
        requires_by_side[side_index].push_back("hardware:" + rh->hardware_class.value());
        if (!taxonomy.HasHardwareClass(rh->hardware_class)) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedHardwareClass,
                              "constraint references undefined hardware class '" +
                                  rh->hardware_class.value() + "'",
                              rule, path + ".hardware_class");
        }
      } else if (const auto* fh = std::get_if<ForbidsHardwareClass>(&constraint)) {
        forbids_by_side[side_index].push_back("hardware:" + fh->hardware_class.value());
        if (!taxonomy.HasHardwareClass(fh->hardware_class)) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedHardwareClass,
                              "constraint references undefined hardware class '" +
                                  fh->hardware_class.value() + "'",
                              rule, path + ".hardware_class");
        }
      } else if (const auto* rf = std::get_if<RequiresFeature>(&constraint)) {
        requires_by_side[side_index].push_back("feature:" + rf->feature.value());
        referenced_features.insert(rf->feature);
        if (!taxonomy.HasFeature(rf->feature)) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedFeature,
                              "constraint references undefined feature '" + rf->feature.value() +
                                  "'",
                              rule, path + ".feature");
        }
      } else if (const auto* ff = std::get_if<ForbidsFeature>(&constraint)) {
        forbids_by_side[side_index].push_back("feature:" + ff->feature.value());
        referenced_features.insert(ff->feature);
        if (!taxonomy.HasFeature(ff->feature)) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedFeature,
                              "constraint references undefined feature '" + ff->feature.value() +
                                  "'",
                              rule, path + ".feature");
        }
      }
    }

    // Requires/forbids of the same fact on the same side is unsatisfiable.
    for (const auto& [side_index, required] : requires_by_side) {
      const auto it = forbids_by_side.find(side_index);
      if (it == forbids_by_side.end()) continue;
      for (const std::string& fact : required) {
        if (std::find(it->second.begin(), it->second.end(), fact) != it->second.end()) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UnsatisfiableConstraints,
                              "rule requires and forbids " + fact + " on the same side", rule,
                              base + ".constraints");
        }
      }
    }

    // Version window inversion.
    {
      std::map<int, const SemVersion*> lowest;
      std::map<int, const SemVersion*> highest;
      for (const Constraint& constraint : rule.constraints) {
        if (const auto* at_least = std::get_if<VersionAtLeast>(&constraint)) {
          lowest[static_cast<int>(at_least->side)] = &at_least->version;
        } else if (const auto* at_most = std::get_if<VersionAtMost>(&constraint)) {
          highest[static_cast<int>(at_most->side)] = &at_most->version;
        }
      }
      for (const auto& [side_index, low] : lowest) {
        const auto it = highest.find(side_index);
        if (it == highest.end()) continue;
        if (it->second->Precedence(*low) == std::strong_ordering::less) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UnsatisfiableConstraints,
                              "rule requires a minimum version above its maximum version", rule,
                              base + ".constraints");
        }
      }
    }

    // Implications.
    for (std::size_t i = 0; i < rule.implications.size(); ++i) {
      const CapabilityImplication& implication = rule.implications[i];
      const std::string path = base + ".implications[" + std::to_string(i) + "]";
      if (rule.arity == RuleArity::Set && implication.side == Side::Right) {
        add_rule_diagnostic(Severity::Error, DiagnosticCode::SetRuleUsesRightSide,
                            "set-arity implication applies to the right side", rule, path);
      }
      if (!taxonomy.HasCapability(implication.if_present)) {
        add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedCapability,
                            "implication references undefined capability '" +
                                implication.if_present.value() + "'",
                            rule, path + ".if_present");
      } else {
        capability_references[implication.if_present] += 1;
      }
      if (!taxonomy.HasCapability(implication.then_required)) {
        add_rule_diagnostic(Severity::Error, DiagnosticCode::UndefinedCapability,
                            "implication references undefined capability '" +
                                implication.then_required.value() + "'",
                            rule, path + ".then_required");
      } else {
        capability_references[implication.then_required] += 1;
      }
      if (implication.if_present == implication.then_required) {
        add_rule_diagnostic(Severity::Error, DiagnosticCode::SelfImplication,
                            "capability '" + implication.if_present.value() +
                                "' implies itself",
                            rule, path);
      }
    }

    // Lifecycle consistency.
    if (rule.lifecycle.state == LifecycleState::Deprecated &&
        !rule.lifecycle.superseded_by.has_value()) {
      add_rule_diagnostic(Severity::Warning, DiagnosticCode::DeprecatedWithoutSuccessor,
                          "deprecated rule does not name a successor", rule, base + ".lifecycle");
    }
    if (rule.lifecycle.superseded_by.has_value()) {
      bool found = false;
      for (const Rule& other : rules) {
        if (other.id == rule.lifecycle.superseded_by->id) {
          found = true;
          break;
        }
      }
      if (!found) {
        add_rule_diagnostic(Severity::Warning, DiagnosticCode::SupersededByMissing,
                            "rule names successor '" +
                                rule.lifecycle.superseded_by->id.value() +
                                "' which is not present in this generation",
                            rule, base + ".lifecycle.superseded_by");
      }
    }
    if (rule.lifecycle.state == LifecycleState::Retired) {
      add_rule_diagnostic(Severity::Info, DiagnosticCode::RetiredRuleDeclared,
                          "retired rule is not evaluated but is retained for history", rule, base);
    }
  }

  // --- Conflicting rules within a precedence tier -------------------------
  {
    std::map<TierKey, std::vector<std::size_t>> tiers;
    for (std::size_t i = 0; i < rules.size(); ++i) {
      tiers[MakeTierKey(rules[i])].push_back(i);
    }
    for (const auto& [tier, members] : tiers) {
      (void)tier;
      for (std::size_t a = 0; a < members.size(); ++a) {
        for (std::size_t b = a + 1; b < members.size(); ++b) {
          const Rule& first = rules[members[a]];
          const Rule& second = rules[members[b]];
          if (first.outcome == second.outcome) continue;
          if (!RuleRegionsOverlap(first, second)) continue;
          add_rule_diagnostic(Severity::Error, DiagnosticCode::ConflictingRules,
                              "rules '" + first.id.value() + "' and '" + second.id.value() +
                                  "' share a precedence tier, overlap, and disagree (" +
                                  std::string(RuleOutcomeName(first.outcome)) + " vs " +
                                  std::string(RuleOutcomeName(second.outcome)) + ")",
                              first, "rules[" + std::to_string(members[a]) + "]");
        }
      }
    }
  }

  // --- Reachability -------------------------------------------------------
  if (options.analyze_reachability && !rules.empty()) {
    if (rules.size() > kMaxReachabilityRules) {
      report.reachability_exhaustive = false;
      report.Add(MakeDiagnostic(
          Severity::Info, DiagnosticCode::ReachabilityAnalysisSkipped,
          "reachability analysis skipped: generation declares more than " +
              std::to_string(kMaxReachabilityRules) + " rules",
          "rules"));
    } else {
      for (std::size_t index = 0; index < rules.size(); ++index) {
        const Rule& target = rules[index];
        if (target.lifecycle.state != LifecycleState::Active) continue;
        const TierKey target_tier = MakeTierKey(target);
        std::vector<const Rule*> shadowers;
        bool budget_exceeded = false;
        for (std::size_t other = 0; other < rules.size(); ++other) {
          if (other == index) continue;
          const Rule& candidate = rules[other];
          if (candidate.lifecycle.state != LifecycleState::Active) continue;
          if (candidate.arity != target.arity) continue;
          if (!RuleAlwaysApplies(candidate)) continue;
          const TierKey candidate_tier = MakeTierKey(candidate);
          if (!(target_tier < candidate_tier)) continue;
          if (!RuleRegionSubsumesModuloVersion(candidate, target)) continue;
          if (shadowers.size() >= options.max_reachability_candidates) {
            budget_exceeded = true;
            break;
          }
          shadowers.push_back(&candidate);
        }
        if (shadowers.empty()) continue;
        if (budget_exceeded) {
          report.reachability_exhaustive = false;
          report.Add(MakeDiagnostic(Severity::Info, DiagnosticCode::ReachabilityAnalysisSkipped,
                                    "reachability analysis for rule '" + target.id.value() +
                                        "' exceeded its candidate budget",
                                    "rules[" + std::to_string(index) + "]"));
          continue;
        }
        // Either a single shadower covers the whole region, or the union of
        // shadowers covers it across the two version dimensions.
        std::vector<RuleBox> target_boxes;
        RuleOrientations(target, target_boxes);
        std::vector<std::vector<RuleBox>> shadower_boxes;
        for (const Rule* shadower : shadowers) {
          std::vector<RuleBox> boxes;
          RuleOrientations(*shadower, boxes);
          shadower_boxes.push_back(std::move(boxes));
        }
        bool exhaustive = true;
        bool dead = true;
        for (const RuleBox& target_box : target_boxes) {
          std::vector<RuleBox> candidates;
          for (const auto& boxes : shadower_boxes) {
            for (const RuleBox& box : boxes) {
              const bool pair = target.arity == RuleArity::Pair;
              if (BoxSubsumesModuloVersion(box, target_box, pair)) candidates.push_back(box);
            }
          }
          const CoverageResult coverage =
              VersionPlaneCovered(target_box, candidates, options.max_reachability_cells);
          if (!coverage.covered) {
            dead = false;
            if (!coverage.exhaustive) exhaustive = false;
            break;
          }
        }
        if (!exhaustive) report.reachability_exhaustive = false;
        if (dead) {
          add_rule_diagnostic(Severity::Error, DiagnosticCode::UnreachableRule,
                              "rule can never decide a query: " + std::to_string(shadowers.size()) +
                                  " higher-precedence active rule(s) always match its region",
                              target, "rules[" + std::to_string(index) + "]");
        }
      }
    }
  }

  // --- Capability implication cycles --------------------------------------
  if (options.analyze_implication_cycles) {
    std::map<std::pair<int, CapabilityId>, std::vector<CapabilityId>> adjacency;
    for (const Rule& rule : rules) {
      if (rule.lifecycle.state != LifecycleState::Active) continue;
      for (const CapabilityImplication& implication : rule.implications) {
        adjacency[{static_cast<int>(implication.side), implication.if_present}].push_back(
            implication.then_required);
      }
    }
    std::map<std::pair<int, CapabilityId>, int> color;  // 0 unseen, 1 on stack, 2 done
    std::vector<std::pair<int, CapabilityId>> stack;
    std::size_t reported = 0;
    constexpr std::size_t kMaxImplicationDepth = 256;
    std::function<void(const std::pair<int, CapabilityId>&, std::size_t)> visit =
        [&](const std::pair<int, CapabilityId>& node, std::size_t depth) {
          if (depth > kMaxImplicationDepth) return;
          color[node] = 1;
          stack.push_back(node);
          const auto it = adjacency.find(node);
          if (it != adjacency.end()) {
            for (const CapabilityId& next : it->second) {
              const std::pair<int, CapabilityId> target{node.first, next};
              const int state = color[target];
              if (state == 1) {
                if (reported < options.max_reported_cycles) {
                  ++reported;
                  std::string path;
                  bool in_cycle = false;
                  for (const auto& entry : stack) {
                    if (entry == target) in_cycle = true;
                    if (in_cycle) {
                      if (!path.empty()) path.append(" -> ");
                      path.append(entry.second.value());
                    }
                  }
                  path.append(" -> ");
                  path.append(next.value());
                  report.Add(MakeDiagnostic(
                      Severity::Error, DiagnosticCode::ImplicationCycle,
                      "capability implication cycle detected: " + path, "rules"));
                }
              } else if (state == 0) {
                visit(target, depth + 1);
              }
            }
          }
          color[node] = 2;
          stack.pop_back();
        };
    for (const auto& [node, edges] : adjacency) {
      (void)edges;
      if (color[node] == 0) visit(node, 0);
    }
    if (reported >= options.max_reported_cycles) {
      report.Add(MakeDiagnostic(Severity::Info, DiagnosticCode::ReachabilityAnalysisSkipped,
                                "additional implication cycles were not reported",
                                "rules"));
    }
  }

  // --- Unused declarations ------------------------------------------------
  if (options.report_unused_capabilities) {
    for (const auto& [id, declaration] : taxonomy.capabilities()) {
      (void)declaration;
      if (capability_references.count(id) == 0) {
        report.Add(MakeDiagnostic(Severity::Info, DiagnosticCode::UnusedCapability,
                                  "capability '" + id.value() +
                                      "' is declared but no rule references it",
                                  "taxonomy.capabilities." + id.value()));
      }
    }
    for (const auto& [id, descriptor] : taxonomy.features()) {
      (void)descriptor;
      if (referenced_features.count(id) == 0) {
        report.Add(MakeDiagnostic(Severity::Info, DiagnosticCode::UnreferencedTaxonomyEntry,
                                  "feature '" + id.value() +
                                      "' is declared but no rule references it",
                                  "taxonomy.features." + id.value()));
      }
    }
  }

  // Deterministic ordering: errors first, then warnings, then info; ties are
  // broken by diagnostic code, path and rule identity.
  std::stable_sort(report.diagnostics.begin(), report.diagnostics.end(),
                   [](const Diagnostic& a, const Diagnostic& b) {
                     if (a.severity != b.severity) return a.severity < b.severity;
                     if (a.code != b.code) return a.code < b.code;
                     if (a.path != b.path) return a.path < b.path;
                     const std::string a_rule = a.rule.has_value() ? a.rule->ToString() : "";
                     const std::string b_rule = b.rule.has_value() ? b.rule->ToString() : "";
                     return a_rule < b_rule;
                   });
  if (report.diagnostics.size() > options.max_diagnostics) {
    report.diagnostics.resize(options.max_diagnostics);
  }
  return report;
}

}  // namespace fcr
