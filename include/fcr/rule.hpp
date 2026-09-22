// Fabric Compatibility Registry - Summon Software Labs
// The compatibility rule model: constraints, implications, precedence.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "fcr/component.hpp"
#include "fcr/error.hpp"
#include "fcr/identity.hpp"
#include "fcr/json.hpp"
#include "fcr/taxonomy.hpp"
#include "fcr/version.hpp"

namespace fcr {

// Which participant(s) of a query a constraint applies to.
//   Left / Right : the corresponding side of a pair rule.
//   AllMembers   : every component of the query (both sides of a pair, every
//                  member of a set query).
enum class Side : std::uint8_t { Left, Right, AllMembers };
std::string_view SideName(Side side);
Result<Side> ParseSide(std::string_view text);

// Provenance class of a rule. Origin participates in precedence below the
// explicit priority and the derived specificity, so an operator override must
// still state its intent with a priority.
enum class RuleOrigin : std::uint8_t { Standard, Vendor, Operator, LocalOverride };
std::string_view RuleOriginName(RuleOrigin origin);
Result<RuleOrigin> ParseRuleOrigin(std::string_view text);
int RuleOriginRank(RuleOrigin origin);

// Symmetric rules also apply with the two selectors swapped.
enum class RuleSymmetry : std::uint8_t { Symmetric, LeftToRight, RightToLeft };
std::string_view RuleSymmetryName(RuleSymmetry symmetry);
Result<RuleSymmetry> ParseRuleSymmetry(std::string_view text);

enum class RuleArity : std::uint8_t { Pair, Set };
std::string_view RuleArityName(RuleArity arity);
Result<RuleArity> ParseRuleArity(std::string_view text);

enum class RuleOutcome : std::uint8_t { Compatible, Incompatible, Unknown };
std::string_view RuleOutcomeName(RuleOutcome outcome);
Result<RuleOutcome> ParseRuleOutcome(std::string_view text);

// What happens when a rule constraint is definitely violated.
enum class OnUnmetRequirement : std::uint8_t { Incompatible, Unknown, Skip };
std::string_view OnUnmetRequirementName(OnUnmetRequirement policy);
Result<OnUnmetRequirement> ParseOnUnmetRequirement(std::string_view text);

// What happens when a rule constraint cannot be decided from the evidence.
enum class OnIndeterminate : std::uint8_t { Unknown, Incompatible, Skip };
std::string_view OnIndeterminateName(OnIndeterminate policy);
Result<OnIndeterminate> ParseOnIndeterminate(std::string_view text);

enum class NegotiationPolicy : std::uint8_t { HighestCommon, LowestCommon, ExactRequired };
std::string_view NegotiationPolicyName(NegotiationPolicy policy);
Result<NegotiationPolicy> ParseNegotiationPolicy(std::string_view text);

enum class LifecycleState : std::uint8_t { Active, Deprecated, Retired };
std::string_view LifecycleStateName(LifecycleState state);
Result<LifecycleState> ParseLifecycleState(std::string_view text);

// ---------------------------------------------------------------------------
// Constraint literals
// ---------------------------------------------------------------------------
// A literal as written in a rule. Its type is resolved against the taxonomy at
// validation time, so an undefined capability is reported as a validation
// diagnostic rather than an opaque parse failure.
class ConstraintLiteral {
 public:
  enum class Kind : std::uint8_t { Boolean, Integer, Text, Version };

  ConstraintLiteral() = default;
  static ConstraintLiteral Boolean(bool v);
  static ConstraintLiteral Integer(std::int64_t v);
  static ConstraintLiteral Text(std::string v);
  static ConstraintLiteral Version(SemVersion v);

  Kind kind() const noexcept { return kind_; }
  bool MatchesType(CapabilityType type) const;
  Result<CapabilityValue> Resolve() const;
  std::string ToString() const;
  JsonValue ToJson() const;
  static Result<ConstraintLiteral> FromJson(const JsonValue& json);

  friend bool operator==(const ConstraintLiteral& a, const ConstraintLiteral& b);

 private:
  Kind kind_ = Kind::Boolean;
  bool boolean_ = false;
  std::int64_t integer_ = 0;
  std::string text_;
  SemVersion version_;
};

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------
struct RequiresCapability {
  Side side = Side::Left;
  CapabilityId capability;
};
struct RequiresCapabilityValue {
  Side side = Side::Left;
  CapabilityId capability;
  ComparisonOp op = ComparisonOp::Eq;
  ConstraintLiteral value;
};
struct ForbidsCapability {
  Side side = Side::Left;
  CapabilityId capability;
};
struct RequiresProtocol {
  Side side = Side::Left;
  ProtocolId protocol;
  VersionRange range = VersionRange::Any();
};
struct ForbidsProtocol {
  Side side = Side::Left;
  ProtocolId protocol;
};
struct RequiresSchema {
  Side side = Side::Left;
  SchemaId schema;
  VersionRange range = VersionRange::Any();
};
struct ForbidsSchema {
  Side side = Side::Left;
  SchemaId schema;
};
struct RequiresHardwareClass {
  Side side = Side::Left;
  HardwareClassId hardware_class;
};
struct ForbidsHardwareClass {
  Side side = Side::Left;
  HardwareClassId hardware_class;
};
struct VersionAtLeast {
  Side side = Side::Left;
  SemVersion version;
};
struct VersionAtMost {
  Side side = Side::Left;
  SemVersion version;
};
struct RequiresFeature {
  Side side = Side::Left;
  FeatureId feature;
};
struct ForbidsFeature {
  Side side = Side::Left;
  FeatureId feature;
};

using Constraint =
    std::variant<RequiresCapability, RequiresCapabilityValue, ForbidsCapability, RequiresProtocol,
                 ForbidsProtocol, RequiresSchema, ForbidsSchema, RequiresHardwareClass,
                 ForbidsHardwareClass, VersionAtLeast, VersionAtMost, RequiresFeature,
                 ForbidsFeature>;

std::string_view ConstraintKindName(const Constraint& constraint);
JsonValue ConstraintToJson(const Constraint& constraint);
Result<Constraint> ParseConstraint(const JsonValue& json);
// Total, deterministic ordering used to canonicalize constraint lists.
bool ConstraintLess(const Constraint& a, const Constraint& b);
// Canonical string key: two constraints are equal exactly when their keys are.
std::string ConstraintSortKey(const Constraint& constraint);
// Semantic equality of two constraints.
bool ConstraintEquals(const Constraint& a, const Constraint& b);
bool ConstraintListsEqual(const std::vector<Constraint>& a, const std::vector<Constraint>& b);
// The side a constraint applies to.
Side ConstraintSide(const Constraint& constraint);
// The capability a constraint names, if any (used for static analysis).
const CapabilityId* ConstraintCapability(const Constraint& constraint);

// "if capability X is present on a side, capability Y must also be present".
// Used for capability-closure analysis; a cycle among implications is a
// validation finding.
struct CapabilityImplication {
  Side side = Side::Left;
  CapabilityId if_present;
  CapabilityId then_required;

  friend bool operator<(const CapabilityImplication& a, const CapabilityImplication& b);
};

// ---------------------------------------------------------------------------
// Lifecycle and provenance
// ---------------------------------------------------------------------------
struct Lifecycle {
  LifecycleState state = LifecycleState::Active;
  std::optional<RuleRef> superseded_by;
  std::string note;

  friend bool operator==(const Lifecycle& a, const Lifecycle& b) {
    return a.state == b.state && a.superseded_by == b.superseded_by && a.note == b.note;
  }
};

struct RuleProvenance {
  PublisherId publisher;
  std::string source;
  std::string reference;
  Timestamp recorded_at;
  std::string change_note;

  friend bool operator==(const RuleProvenance& a, const RuleProvenance& b) {
    return a.publisher == b.publisher && a.source == b.source && a.reference == b.reference &&
           a.recorded_at == b.recorded_at && a.change_note == b.change_note;
  }
};

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------
constexpr std::size_t kMaxRulesPerGeneration = 65536;
constexpr std::size_t kMaxConstraintsPerRule = 128;
constexpr std::size_t kMaxImplicationsPerRule = 128;
constexpr std::size_t kMaxRationaleBytes = 1024;

struct Rule {
  RuleId id;
  RuleRevision revision;
  RulePriority priority;
  RuleOrigin origin = RuleOrigin::Standard;
  RuleSymmetry symmetry = RuleSymmetry::Symmetric;
  RuleArity arity = RuleArity::Pair;
  RuleOutcome outcome = RuleOutcome::Compatible;
  ComponentSelector left;
  ComponentSelector right;
  std::vector<Constraint> constraints;
  std::vector<CapabilityImplication> implications;
  NegotiationPolicy negotiation = NegotiationPolicy::HighestCommon;
  OnUnmetRequirement on_unmet = OnUnmetRequirement::Incompatible;
  OnIndeterminate on_indeterminate = OnIndeterminate::Unknown;
  Lifecycle lifecycle;
  RuleProvenance provenance;
  std::string rationale;

  RuleRef Ref() const { return RuleRef{id, revision}; }
};

using RuleList = std::vector<Rule>;

// Derived specificity: a deterministic integer ranking how narrowly a rule is
// scoped. Explicit priority dominates it; origin ranks below it.
int ComputeSpecificity(const Rule& rule);

// The semantic precedence tier. Rules in the same tier that overlap and
// disagree are a publication-blocking conflict: no arbitrary tie-break is
// allowed to decide a compatibility question.
struct PrecedenceTier {
  RulePriority priority;
  int specificity = 0;
  int origin_rank = 0;

  friend bool operator==(const PrecedenceTier& a, const PrecedenceTier& b) {
    return a.priority == b.priority && a.specificity == b.specificity &&
           a.origin_rank == b.origin_rank;
  }
  friend bool operator<(const PrecedenceTier& a, const PrecedenceTier& b) {
    if (a.priority != b.priority) return a.priority < b.priority;
    if (a.specificity != b.specificity) return a.specificity < b.specificity;
    return a.origin_rank < b.origin_rank;
  }
};

PrecedenceTier RuleTier(const Rule& rule);

// The total order actually used to rank matched rules: tier first, then the
// canonical rule identity as the documented final tie-break.
struct PrecedenceKey {
  PrecedenceTier tier;
  RuleId id;
  RuleRevision revision;
};

PrecedenceKey RulePrecedence(const Rule& rule);
// True when a outranks b.
bool PrecedenceHigher(const PrecedenceKey& a, const PrecedenceKey& b);
std::string PrecedenceKeyToString(const PrecedenceKey& key);

// Sorts constraints and implications into canonical order.
void NormalizeRule(Rule& rule);
// Canonical (key-sorted, array-normalized) JSON for a rule.
JsonValue RuleToJson(const Rule& rule);
Result<Rule> ParseRule(const JsonValue& json, std::string_view context);
// Stable content digest of a normalized rule.
ContentDigest RuleContentDigest(const Rule& rule);

bool RuleMatchesSelectors(const Rule& rule, const ComponentSpec& left, const ComponentSpec& right,
                          bool* mirrored);

}  // namespace fcr
