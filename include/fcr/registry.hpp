// Fabric Compatibility Registry - Summon Software Labs
// Compiled registry generations, compatibility decisions and explanations.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fcr/component.hpp"
#include "fcr/document.hpp"
#include "fcr/error.hpp"
#include "fcr/identity.hpp"
#include "fcr/json.hpp"
#include "fcr/rule.hpp"
#include "fcr/validation.hpp"

namespace fcr {

// ---------------------------------------------------------------------------
// Decision vocabulary
// ---------------------------------------------------------------------------
// UNKNOWN is a first-class outcome. It is never collapsed into COMPATIBLE:
// absence of knowledge is reported as absence of knowledge.
enum class DecisionOutcome : std::uint8_t { Compatible, Incompatible, Unknown };
std::string_view DecisionOutcomeName(DecisionOutcome outcome);
Result<DecisionOutcome> ParseDecisionOutcome(std::string_view text);
// A rule declares an outcome; a decision carries one. The mapping is explicit
// so the two vocabularies cannot be confused at a call site.
DecisionOutcome FromRuleOutcome(RuleOutcome outcome);

enum class DecisionReason : std::uint8_t {
  // A rule reached a verdict.
  DecidedByRule,
  // No rule matched the query region at all.
  NoMatchingRule,
  // Every matched rule declined to apply and none reached a verdict.
  AllMatchedRulesSkipped,
  // The query named a component kind the taxonomy does not declare.
  UnregisteredComponentKind,
  // The query named a component family the taxonomy does not declare.
  UnregisteredComponentFamily,
  // The component kind is declared under a different family than the query.
  KindFamilyMismatch,
  // The generation contains no active rules at all.
  NoActiveRules,
};
std::string_view DecisionReasonName(DecisionReason reason);

enum class ConstraintStatus : std::uint8_t { Satisfied, Violated, Indeterminate };
std::string_view ConstraintStatusName(ConstraintStatus status);

enum class RuleEvaluationStatus : std::uint8_t {
  Satisfied,
  Violated,
  Indeterminate,
  Skipped,
};
std::string_view RuleEvaluationStatusName(RuleEvaluationStatus status);

enum class NegotiationStatus : std::uint8_t {
  // Both sides (or every member) support the subject and share a version.
  Agreed,
  // Both sides support it but share no version.
  NoOverlap,
  // A participant's knowledge is open, so the common set cannot be decided.
  Indeterminate,
  // A participant definitively does not support it.
  NotSupported,
};
std::string_view NegotiationStatusName(NegotiationStatus status);

// Typed identity of a decision. Two queries with the same semantics under the
// same generation produce the same identifier.
struct DecisionId {
  ContentDigest digest;

  std::string ToHex() const { return digest.ToHex(); }
  friend bool operator==(const DecisionId& a, const DecisionId& b) { return a.digest == b.digest; }
  friend bool operator!=(const DecisionId& a, const DecisionId& b) { return !(a == b); }
};

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------
struct ComponentRef {
  ComponentInstanceId instance;
  std::string label;
  ComponentKindId kind;
  ComponentFamilyId family;
  SemVersion version;
  std::optional<HardwareClassId> hardware_class;
};

ComponentRef MakeComponentRef(const ComponentSpec& spec);

struct ConstraintEvaluation {
  Constraint constraint;
  ConstraintStatus status = ConstraintStatus::Satisfied;
  std::string detail;
};

struct RuleEvaluation {
  RuleRef rule;
  PrecedenceKey precedence;
  // 0 is the highest-precedence matched rule.
  std::size_t precedence_rank = 0;
  // The rule matched with its two selectors swapped.
  bool mirrored = false;
  RuleEvaluationStatus status = RuleEvaluationStatus::Satisfied;
  std::optional<DecisionOutcome> resolved_outcome;
  // This rule produced the decision.
  bool decided = false;
  std::vector<ConstraintEvaluation> constraints;
  std::string rationale;
};

struct UnmetRequirement {
  RuleRef rule;
  Side side = Side::Left;
  std::string requirement;
  ConstraintStatus status = ConstraintStatus::Violated;
  std::string detail;
};

struct NegotiatedSubject {
  // Protocol or schema identifier.
  std::string subject;
  NegotiationStatus status = NegotiationStatus::Indeterminate;
  std::optional<SemVersion> selected;
  std::vector<SemVersion> window;
  // Component kind that made the negotiation indeterminate or unsupported.
  std::string owner;
};

struct NegotiationResult {
  NegotiationPolicy policy = NegotiationPolicy::HighestCommon;
  std::vector<NegotiatedSubject> protocols;
  std::vector<NegotiatedSubject> schemas;

  bool empty() const { return protocols.empty() && schemas.empty(); }
};

// A matched rule that was outranked by the deciding rule and would have
// produced a different outcome. Precedence resolved it; the explanation keeps
// the record so a reader can see what was overridden and by what.
struct OverriddenNote {
  RuleRef rule;
  DecisionOutcome outcome = DecisionOutcome::Unknown;
  std::string detail;
};

struct Decision {
  DecisionId id;
  DecisionOutcome outcome = DecisionOutcome::Unknown;
  DecisionReason reason = DecisionReason::NoMatchingRule;
  GenerationId generation;
  RuleArity arity = RuleArity::Pair;
  std::vector<ComponentRef> subjects;
  std::vector<RuleEvaluation> matched_rules;
  std::size_t total_matched_rules = 0;
  bool evidence_truncated = false;
  std::optional<RuleRef> deciding_rule;
  std::vector<UnmetRequirement> unmet_requirements;
  std::vector<OverriddenNote> overridden;
  NegotiationResult negotiation;
  // The rendered explanation of this decision, produced by RenderDecision. It
  // is a deterministic function of the fields above plus the generation, so
  // equal inputs always produce equal text.
  std::string narrative;

  bool IsCompatible() const { return outcome == DecisionOutcome::Compatible; }
  bool IsIncompatible() const { return outcome == DecisionOutcome::Incompatible; }
  bool IsUnknown() const { return outcome == DecisionOutcome::Unknown; }
};

// Aggregate answer for an n-ary query. The aggregate is the conservative meet
// of the set-level evaluation and every pairwise evaluation.
struct SetDecision {
  DecisionId id;
  DecisionOutcome outcome = DecisionOutcome::Unknown;
  GenerationId generation;
  std::vector<ComponentRef> members;
  Decision set_level;
  std::vector<Decision> pairs;
  std::string narrative;
};

JsonValue DecisionToJson(const Decision& decision);
JsonValue SetDecisionToJson(const SetDecision& decision);
std::string RenderDecision(const Decision& decision);
std::string RenderSetDecision(const SetDecision& decision);

// A decision is rendered as text by a stable, deterministic formatter; the
// explanation is part of the contract, so it is covered by tests.

// ---------------------------------------------------------------------------
// Compiled registry generation
// ---------------------------------------------------------------------------
constexpr std::size_t kMaxEvidenceRules = 256;
constexpr std::size_t kMaxNegotiatedSubjects = 64;
constexpr std::size_t kMaxSetMembers = 64;

class RegistryGeneration {
 public:
  // Validates and compiles. Refuses to produce a generation whose rule set has
  // any validation error.
  static Result<std::shared_ptr<const RegistryGeneration>> Compile(
      const RegistryDocument& document, const ValidationOptions& validation = ValidationOptions{});

  const GenerationId& id() const noexcept { return id_; }
  const RegistryDocument& document() const noexcept { return document_; }
  const Taxonomy& taxonomy() const noexcept { return document_.taxonomy; }

  Decision EvaluatePair(const ComponentSpec& left, const ComponentSpec& right) const;
  Result<SetDecision> EvaluateSet(const std::vector<ComponentSpec>& members) const;

  // All active rules whose selectors match the pair, in precedence order.
  std::vector<std::size_t> MatchPairRules(const ComponentSpec& left,
                                          const ComponentSpec& right) const;

 private:
  RegistryGeneration() = default;

  Decision EvaluatePairInternal(const ComponentSpec& left, const ComponentSpec& right) const;
  Decision EvaluateSetLevel(const std::vector<ComponentSpec>& members) const;

  RegistryDocument document_;
  GenerationId id_;
  std::vector<std::size_t> active_pair_rules_;
  std::vector<std::size_t> active_set_rules_;
};

// ---------------------------------------------------------------------------
// Generation diff
// ---------------------------------------------------------------------------
enum class RuleDiffKind : std::uint8_t { Added, Removed, Modified, Unchanged };
std::string_view RuleDiffKindName(RuleDiffKind kind);

struct RuleDiffEntry {
  RuleRef rule;
  RuleDiffKind kind = RuleDiffKind::Unchanged;
  std::optional<ContentDigest> before_digest;
  std::optional<ContentDigest> after_digest;
  std::vector<std::string> changed_fields;
};

struct GenerationDiff {
  GenerationId from;
  GenerationId to;
  std::vector<RuleDiffEntry> rules;
  std::vector<std::string> taxonomy_changes;
  std::size_t added = 0;
  std::size_t removed = 0;
  std::size_t modified = 0;
  std::size_t unchanged = 0;

  bool IsEmpty() const { return added == 0 && removed == 0 && modified == 0 && taxonomy_changes.empty(); }
};

GenerationDiff DiffGenerations(const RegistryGeneration& before, const RegistryGeneration& after);
JsonValue GenerationDiffToJson(const GenerationDiff& diff);
std::string RenderGenerationDiff(const GenerationDiff& diff);

// Replays a corpus of queries against two generations to show which decisions
// actually change. Completed work only: every query is evaluated under both.
struct DecisionChange {
  std::vector<ComponentRef> subjects;
  DecisionOutcome before_outcome = DecisionOutcome::Unknown;
  DecisionOutcome after_outcome = DecisionOutcome::Unknown;
  DecisionId before_id;
  DecisionId after_id;
  std::optional<RuleRef> before_rule;
  std::optional<RuleRef> after_rule;
};

struct DecisionReplayDiff {
  GenerationId from;
  GenerationId to;
  std::vector<DecisionChange> changes;
  std::size_t compared = 0;
};

Result<DecisionReplayDiff> ReplayDiff(const RegistryGeneration& before,
                                      const RegistryGeneration& after,
                                      const std::vector<std::vector<ComponentSpec>>& queries);

// ---------------------------------------------------------------------------
// Rule provenance history
// ---------------------------------------------------------------------------
struct ProvenanceRecord {
  RuleRef rule;
  GenerationNumber generation;
  ContentDigest rule_digest;
  RuleProvenance provenance;
  LifecycleState lifecycle = LifecycleState::Active;
  // False when the generation removed a rule that existed before.
  bool present = true;
};

std::vector<ProvenanceRecord> ProvenanceHistory(const RuleId& rule,
                                                const std::vector<const RegistryGeneration*>&
                                                    generations_ascending);
std::string RenderProvenanceHistory(const std::vector<ProvenanceRecord>& history);

}  // namespace fcr
