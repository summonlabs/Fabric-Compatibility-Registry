#include "fcr/registry.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include "fcr/digest.hpp"

namespace fcr {
namespace {

struct SideResolver {
  std::vector<const ComponentSpec*> left;
  std::vector<const ComponentSpec*> right;
  std::vector<const ComponentSpec*> all;

  const std::vector<const ComponentSpec*>& Resolve(Side side) const {
    switch (side) {
      case Side::Left: return left;
      case Side::Right: return right;
      case Side::AllMembers: return all;
    }
    return left;
  }
};

struct ConstraintOutcome {
  ConstraintStatus status = ConstraintStatus::Satisfied;
  std::string detail;
};

ConstraintOutcome Satisfied() { return ConstraintOutcome{}; }

ConstraintOutcome Violated(std::string detail) {
  ConstraintOutcome out;
  out.status = ConstraintStatus::Violated;
  out.detail = std::move(detail);
  return out;
}

ConstraintOutcome Indeterminate(std::string detail) {
  ConstraintOutcome out;
  out.status = ConstraintStatus::Indeterminate;
  out.detail = std::move(detail);
  return out;
}

ConstraintOutcome EvaluateConstraintAgainstSpec(const Constraint& constraint,
                                                const ComponentSpec& spec,
                                                const Taxonomy& taxonomy) {
  if (const auto* rc = std::get_if<RequiresCapability>(&constraint)) {
    if (spec.capabilities.Contains(rc->capability)) return Satisfied();
    if (spec.CapabilitiesClosed()) {
      return Violated("capability '" + rc->capability.value() + "' is not present");
    }
    return Indeterminate("capability '" + rc->capability.value() +
                         "' is not declared and the component's capability knowledge is open");
  }
  if (const auto* rv = std::get_if<RequiresCapabilityValue>(&constraint)) {
    const CapabilityValue* actual = spec.capabilities.Get(rv->capability);
    if (actual == nullptr) {
      if (spec.CapabilitiesClosed()) {
        return Violated("capability '" + rv->capability.value() + "' is not present");
      }
      return Indeterminate("capability '" + rv->capability.value() +
                           "' is not declared and the component's capability knowledge is open");
    }
    auto expected = rv->value.Resolve();
    if (!expected.has_value()) {
      return Violated("constraint literal cannot be resolved");
    }
    const CapabilityDeclaration* declaration = taxonomy.FindCapability(rv->capability);
    if (declaration != nullptr && expected.value().type() != declaration->type) {
      return Violated("capability '" + rv->capability.value() + "' has an incompatible value type");
    }
    if (ApplyComparison(rv->op, *actual, expected.value())) return Satisfied();
    return Violated("capability '" + rv->capability.value() + "' is " + actual->ToString() +
                    ", which does not satisfy " + std::string(ComparisonOpName(rv->op)) + " " +
                    expected.value().ToString());
  }
  if (const auto* fc = std::get_if<ForbidsCapability>(&constraint)) {
    if (!spec.capabilities.Contains(fc->capability)) {
      if (spec.CapabilitiesClosed()) return Satisfied();
      return Indeterminate("capability '" + fc->capability.value() +
                           "' is not declared and the component's capability knowledge is open");
    }
    return Violated("capability '" + fc->capability.value() + "' is present and forbidden");
  }
  if (const auto* rp = std::get_if<RequiresProtocol>(&constraint)) {
    const std::vector<SemVersion>* versions = spec.protocols.Get(rp->protocol);
    if (versions == nullptr) {
      if (spec.ProtocolsClosed()) {
        return Violated("protocol '" + rp->protocol.value() + "' is not supported");
      }
      return Indeterminate("protocol '" + rp->protocol.value() +
                           "' is not declared and the component's protocol knowledge is open");
    }
    for (const SemVersion& version : *versions) {
      if (rp->range.Contains(version)) return Satisfied();
    }
    return Violated("protocol '" + rp->protocol.value() +
                    "' is supported only outside the required range " + rp->range.ToString());
  }
  if (const auto* fp = std::get_if<ForbidsProtocol>(&constraint)) {
    if (spec.protocols.Contains(fp->protocol)) {
      return Violated("protocol '" + fp->protocol.value() + "' is supported and forbidden");
    }
    if (spec.ProtocolsClosed()) return Satisfied();
    return Indeterminate("protocol '" + fp->protocol.value() +
                         "' is not declared and the component's protocol knowledge is open");
  }
  if (const auto* rs = std::get_if<RequiresSchema>(&constraint)) {
    const std::vector<SemVersion>* versions = spec.schemas.Get(rs->schema);
    if (versions == nullptr) {
      if (spec.SchemasClosed()) {
        return Violated("schema '" + rs->schema.value() + "' is not supported");
      }
      return Indeterminate("schema '" + rs->schema.value() +
                           "' is not declared and the component's schema knowledge is open");
    }
    for (const SemVersion& version : *versions) {
      if (rs->range.Contains(version)) return Satisfied();
    }
    return Violated("schema '" + rs->schema.value() +
                    "' is supported only outside the required range " + rs->range.ToString());
  }
  if (const auto* fs = std::get_if<ForbidsSchema>(&constraint)) {
    if (spec.schemas.Contains(fs->schema)) {
      return Violated("schema '" + fs->schema.value() + "' is supported and forbidden");
    }
    if (spec.SchemasClosed()) return Satisfied();
    return Indeterminate("schema '" + fs->schema.value() +
                         "' is not declared and the component's schema knowledge is open");
  }
  if (const auto* rh = std::get_if<RequiresHardwareClass>(&constraint)) {
    if (spec.hardware_class.has_value()) {
      if (*spec.hardware_class == rh->hardware_class) return Satisfied();
      return Violated("hardware class is '" + spec.hardware_class->value() + "', required '" +
                      rh->hardware_class.value() + "'");
    }
    return Indeterminate("component does not report a hardware class");
  }
  if (const auto* fh = std::get_if<ForbidsHardwareClass>(&constraint)) {
    if (spec.hardware_class.has_value()) {
      if (*spec.hardware_class == fh->hardware_class) {
        return Violated("hardware class '" + fh->hardware_class.value() + "' is forbidden");
      }
      return Satisfied();
    }
    return Indeterminate("component does not report a hardware class");
  }
  if (const auto* rfe = std::get_if<RequiresFeature>(&constraint)) {
    if (spec.features.Contains(rfe->feature)) return Satisfied();
    if (spec.CapabilitiesClosed()) {
      return Violated("feature '" + rfe->feature.value() + "' is not present");
    }
    return Indeterminate("feature '" + rfe->feature.value() +
                         "' is not declared and the component's capability knowledge is open");
  }
  if (const auto* ffe = std::get_if<ForbidsFeature>(&constraint)) {
    if (!spec.features.Contains(ffe->feature)) {
      if (spec.CapabilitiesClosed()) return Satisfied();
      return Indeterminate("feature '" + ffe->feature.value() +
                           "' is not declared and the component's capability knowledge is open");
    }
    return Violated("feature '" + ffe->feature.value() + "' is present and forbidden");
  }
  if (const auto* va = std::get_if<VersionAtLeast>(&constraint)) {
    if (!(spec.version < va->version)) return Satisfied();
    return Violated("component version " + spec.version.ToString() + " is below the required " +
                    va->version.ToString());
  }
  if (const auto* vm = std::get_if<VersionAtMost>(&constraint)) {
    if (!(vm->version < spec.version)) return Satisfied();
    return Violated("component version " + spec.version.ToString() + " is above the permitted " +
                    vm->version.ToString());
  }
  return Indeterminate("unrecognised constraint");
}

struct RuleApplication {
  RuleEvaluationStatus status = RuleEvaluationStatus::Satisfied;
  std::optional<DecisionOutcome> resolved;
  std::vector<ConstraintEvaluation> constraints;
  std::vector<UnmetRequirement> unmet;
  std::string rationale;
};

RuleApplication ApplyRule(const Rule& rule, const Taxonomy& taxonomy,
                          const SideResolver& resolver) {
  RuleApplication application;
  bool any_violated = false;
  bool any_indeterminate = false;
  for (const Constraint& constraint : rule.constraints) {
    const Side side = ConstraintSide(constraint);
    const std::vector<const ComponentSpec*>& targets = resolver.Resolve(side);
    ConstraintEvaluation evaluation;
    evaluation.constraint = constraint;
    evaluation.status = ConstraintStatus::Satisfied;
    if (targets.empty()) {
      evaluation.status = ConstraintStatus::Indeterminate;
      evaluation.detail = "constraint side selects no component";
    }
    for (const ComponentSpec* target : targets) {
      const ConstraintOutcome outcome = EvaluateConstraintAgainstSpec(constraint, *target, taxonomy);
      if (outcome.status == ConstraintStatus::Violated) {
        evaluation.status = ConstraintStatus::Violated;
        evaluation.detail = outcome.detail;
        break;
      }
      if (outcome.status == ConstraintStatus::Indeterminate &&
          evaluation.status == ConstraintStatus::Satisfied) {
        evaluation.status = ConstraintStatus::Indeterminate;
        evaluation.detail = outcome.detail;
      }
    }
    if (evaluation.status == ConstraintStatus::Violated) {
      any_violated = true;
      UnmetRequirement unmet;
      unmet.rule = rule.Ref();
      unmet.side = side;
      unmet.requirement = std::string(ConstraintKindName(constraint));
      unmet.status = ConstraintStatus::Violated;
      unmet.detail = evaluation.detail;
      application.unmet.push_back(std::move(unmet));
    } else if (evaluation.status == ConstraintStatus::Indeterminate) {
      any_indeterminate = true;
      UnmetRequirement unmet;
      unmet.rule = rule.Ref();
      unmet.side = side;
      unmet.requirement = std::string(ConstraintKindName(constraint));
      unmet.status = ConstraintStatus::Indeterminate;
      unmet.detail = evaluation.detail;
      application.unmet.push_back(std::move(unmet));
    }
    application.constraints.push_back(std::move(evaluation));
  }

  if (any_violated) {
    switch (rule.on_unmet) {
      case OnUnmetRequirement::Skip:
        application.status = RuleEvaluationStatus::Skipped;
        application.rationale = "requirements are unmet; the rule declines to apply";
        return application;
      case OnUnmetRequirement::Unknown:
        application.status = RuleEvaluationStatus::Violated;
        application.resolved = DecisionOutcome::Unknown;
        application.rationale = "requirements are unmet and the rule's policy maps that to UNKNOWN";
        return application;
      case OnUnmetRequirement::Incompatible:
        application.status = RuleEvaluationStatus::Violated;
        application.resolved = DecisionOutcome::Incompatible;
        application.rationale =
            "requirements are unmet and the rule's policy maps that to INCOMPATIBLE";
        return application;
    }
  }
  if (any_indeterminate) {
    switch (rule.on_indeterminate) {
      case OnIndeterminate::Skip:
        application.status = RuleEvaluationStatus::Skipped;
        application.rationale = "evidence is incomplete; the rule declines to apply";
        return application;
      case OnIndeterminate::Incompatible:
        application.status = RuleEvaluationStatus::Indeterminate;
        application.resolved = DecisionOutcome::Incompatible;
        application.rationale =
            "evidence is incomplete and the rule's policy maps that to INCOMPATIBLE";
        return application;
      case OnIndeterminate::Unknown:
        application.status = RuleEvaluationStatus::Indeterminate;
        application.resolved = DecisionOutcome::Unknown;
        application.rationale = "evidence is incomplete; the rule cannot reach a verdict";
        return application;
    }
  }
  application.status = RuleEvaluationStatus::Satisfied;
  application.resolved = FromRuleOutcome(rule.outcome);
  application.rationale = "all constraints are satisfied; the declared outcome " +
                          std::string(RuleOutcomeName(rule.outcome)) + " applies";
  return application;
}

std::vector<SemVersion> IntersectVersionLists(const std::vector<SemVersion>& a,
                                              const std::vector<SemVersion>& b) {
  std::vector<SemVersion> out;
  for (const SemVersion& left : a) {
    for (const SemVersion& right : b) {
      if (left.Precedence(right) == std::strong_ordering::equal) {
        out.push_back(left);
        break;
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const SemVersion& x, const SemVersion& y) {
    return x.TotalOrder(y) == std::strong_ordering::less;
  });
  return out;
}

std::optional<SemVersion> SelectNegotiated(const std::vector<SemVersion>& window,
                                           NegotiationPolicy policy) {
  if (window.empty()) return std::nullopt;
  SemVersion best = window.front();
  for (const SemVersion& candidate : window) {
    if (policy == NegotiationPolicy::LowestCommon) {
      if (candidate.Precedence(best) == std::strong_ordering::less) best = candidate;
    } else {
      if (candidate.Precedence(best) == std::strong_ordering::greater) best = candidate;
    }
  }
  return best;
}

template <class Getter>
NegotiatedSubject NegotiateOne(const std::string& subject_id,
                               const std::vector<const ComponentSpec*>& specs,
                               NegotiationPolicy policy, Getter get, bool closed,
                               const char* label) {
  NegotiatedSubject subject;
  subject.subject = subject_id;
  std::vector<SemVersion> window;
  bool first = true;
  for (const ComponentSpec* spec : specs) {
    const std::vector<SemVersion>* versions = get(*spec);
    if (versions == nullptr) {
      const bool is_closed = closed ? spec->SchemasClosed() : spec->ProtocolsClosed();
      if (is_closed) {
        subject.status = NegotiationStatus::NotSupported;
        subject.owner = spec->kind.value();
        return subject;
      }
      subject.status = NegotiationStatus::Indeterminate;
      subject.owner = spec->kind.value();
      return subject;
    }
    window = first ? *versions : IntersectVersionLists(window, *versions);
    first = false;
    if (window.empty()) {
      subject.status = NegotiationStatus::NoOverlap;
      return subject;
    }
  }
  subject.window = window;
  if (policy == NegotiationPolicy::ExactRequired && window.size() != 1) {
    subject.status = NegotiationStatus::NoOverlap;
    return subject;
  }
  subject.selected = SelectNegotiated(window, policy);
  subject.status =
      subject.selected.has_value() ? NegotiationStatus::Agreed : NegotiationStatus::NoOverlap;
  (void)label;
  return subject;
}

NegotiationResult Negotiate(const std::vector<const ComponentSpec*>& specs,
                            NegotiationPolicy policy, const Taxonomy& taxonomy) {
  NegotiationResult result;
  result.policy = policy;
  if (specs.size() < 2) return result;
  std::set<ProtocolId> protocols;
  std::set<SchemaId> schemas;
  for (const ComponentSpec* spec : specs) {
    for (const auto& [id, versions] : spec->protocols.items()) {
      (void)versions;
      protocols.insert(id);
    }
    for (const auto& [id, versions] : spec->schemas.items()) {
      (void)versions;
      schemas.insert(id);
    }
  }
  std::size_t emitted = 0;
  for (const ProtocolId& id : protocols) {
    if (emitted >= kMaxNegotiatedSubjects) break;
    if (!taxonomy.HasProtocol(id)) continue;
    result.protocols.push_back(NegotiateOne(
        id.value(), specs, policy,
        [&id](const ComponentSpec& spec) { return spec.protocols.Get(id); }, false, "protocol"));
    ++emitted;
  }
  emitted = 0;
  for (const SchemaId& id : schemas) {
    if (emitted >= kMaxNegotiatedSubjects) break;
    if (!taxonomy.HasSchema(id)) continue;
    result.schemas.push_back(NegotiateOne(
        id.value(), specs, policy,
        [&id](const ComponentSpec& spec) { return spec.schemas.Get(id); }, true, "schema"));
    ++emitted;
  }
  return result;
}

// Instance names, labels and vendor identity play no part in a decision, so
// they are excluded from the decision fingerprint: the same semantic query
// under the same generation always yields the same identifier.
JsonValue SemanticSpecJson(const ComponentSpec& spec) {
  JsonValue out = JsonValue::Obj();
  out.Set("family", JsonValue::Str(spec.family.value()));
  out.Set("kind", JsonValue::Str(spec.kind.value()));
  out.Set("version", JsonValue::Str(spec.version.ToString()));
  if (spec.hardware_class.has_value()) {
    out.Set("hardware_class", JsonValue::Str(spec.hardware_class->value()));
  }
  {
    JsonValue knowledge = JsonValue::Obj();
    knowledge.Set("capabilities",
                  JsonValue::Str(std::string(KnowledgeClosureName(spec.knowledge.capabilities))));
    knowledge.Set("protocols",
                  JsonValue::Str(std::string(KnowledgeClosureName(spec.knowledge.protocols))));
    knowledge.Set("schemas",
                  JsonValue::Str(std::string(KnowledgeClosureName(spec.knowledge.schemas))));
    out.Set("knowledge", std::move(knowledge));
  }
  {
    JsonValue capabilities = JsonValue::Obj();
    for (const auto& [id, value] : spec.capabilities.items()) {
      capabilities.Set(id.value(), value.ToJson());
    }
    out.Set("capabilities", std::move(capabilities));
  }
  {
    JsonValue protocols = JsonValue::Obj();
    for (const auto& [id, versions] : spec.protocols.items()) {
      JsonValue::Array array;
      for (const SemVersion& v : versions) array.push_back(JsonValue::Str(v.ToString()));
      protocols.Set(id.value(), JsonValue::Arr(std::move(array)));
    }
    out.Set("protocols", std::move(protocols));
  }
  {
    JsonValue schemas = JsonValue::Obj();
    for (const auto& [id, versions] : spec.schemas.items()) {
      JsonValue::Array array;
      for (const SemVersion& v : versions) array.push_back(JsonValue::Str(v.ToString()));
      schemas.Set(id.value(), JsonValue::Arr(std::move(array)));
    }
    out.Set("schemas", std::move(schemas));
  }
  {
    JsonValue::Array features;
    for (const FeatureId& id : spec.features.items()) {
      features.push_back(JsonValue::Str(id.value()));
    }
    out.Set("features", JsonValue::Arr(std::move(features)));
  }
  return out;
}

JsonValue NegotiationToJson(const NegotiationResult& negotiation) {
  JsonValue out = JsonValue::Obj();
  out.Set("policy", JsonValue::Str(std::string(NegotiationPolicyName(negotiation.policy))));
  const auto emit = [](const std::vector<NegotiatedSubject>& subjects) {
    JsonValue::Array array;
    array.reserve(subjects.size());
    for (const NegotiatedSubject& subject : subjects) {
      JsonValue entry = JsonValue::Obj();
      entry.Set("subject", JsonValue::Str(subject.subject));
      entry.Set("status", JsonValue::Str(std::string(NegotiationStatusName(subject.status))));
      if (subject.selected.has_value()) {
        entry.Set("selected", JsonValue::Str(subject.selected->ToString()));
      }
      if (!subject.owner.empty()) entry.Set("owner", JsonValue::Str(subject.owner));
      JsonValue::Array window;
      for (const SemVersion& version : subject.window) {
        window.push_back(JsonValue::Str(version.ToString()));
      }
      entry.Set("window", JsonValue::Arr(std::move(window)));
      array.push_back(std::move(entry));
    }
    return JsonValue::Arr(std::move(array));
  };
  out.Set("protocols", emit(negotiation.protocols));
  out.Set("schemas", emit(negotiation.schemas));
  return out;
}

DecisionId ComputeDecisionId(const GenerationId& generation, const std::string& arity,
                             const std::vector<ComponentSpec>& subjects,
                             DecisionOutcome outcome, DecisionReason reason,
                             const std::optional<RuleRef>& deciding_rule,
                             const std::vector<RuleEvaluation>& matched,
                             const NegotiationResult& negotiation, const std::string& extra) {
  JsonValue payload = JsonValue::Obj();
  {
    JsonValue gen = JsonValue::Obj();
    gen.Set("number", JsonValue::UInt(generation.number.value()));
    gen.Set("digest", JsonValue::Str(generation.digest.ToHex()));
    payload.Set("generation", std::move(gen));
  }
  payload.Set("arity", JsonValue::Str(arity));
  {
    JsonValue::Array array;
    array.reserve(subjects.size());
    for (const ComponentSpec& subject : subjects) array.push_back(SemanticSpecJson(subject));
    payload.Set("subjects", JsonValue::Arr(std::move(array)));
  }
  payload.Set("outcome", JsonValue::Str(std::string(DecisionOutcomeName(outcome))));
  payload.Set("reason", JsonValue::Str(std::string(DecisionReasonName(reason))));
  if (deciding_rule.has_value()) {
    payload.Set("deciding_rule", JsonValue::Str(deciding_rule->ToString()));
  }
  {
    JsonValue::Array array;
    array.reserve(matched.size());
    for (const RuleEvaluation& evaluation : matched) {
      array.push_back(JsonValue::Str(evaluation.rule.ToString()));
    }
    payload.Set("matched_rules", JsonValue::Arr(std::move(array)));
  }
  payload.Set("negotiation", NegotiationToJson(negotiation));
  if (!extra.empty()) payload.Set("extra", JsonValue::Str(extra));
  DecisionId id;
  id.digest = Sha256::Hash(payload.Dump(false));
  return id;
}

std::vector<const ComponentSpec*> ToPointerList(const std::vector<ComponentSpec>& specs) {
  std::vector<const ComponentSpec*> out;
  out.reserve(specs.size());
  for (const ComponentSpec& spec : specs) out.push_back(&spec);
  return out;
}

void RecordOverrideNotes(Decision* decision) {
  if (!decision->deciding_rule.has_value()) return;
  for (const RuleEvaluation& evaluation : decision->matched_rules) {
    if (evaluation.decided) continue;
    if (!evaluation.resolved_outcome.has_value()) continue;
    if (*evaluation.resolved_outcome == decision->outcome) continue;
    OverriddenNote note;
    note.rule = evaluation.rule;
    note.outcome = *evaluation.resolved_outcome;
    note.detail = "matched but outranked by " + decision->deciding_rule->ToString();
    decision->overridden.push_back(std::move(note));
  }
}

}  // namespace

std::string_view DecisionOutcomeName(DecisionOutcome outcome) {
  switch (outcome) {
    case DecisionOutcome::Compatible: return "compatible";
    case DecisionOutcome::Incompatible: return "incompatible";
    case DecisionOutcome::Unknown: return "unknown";
  }
  return "unknown";
}

DecisionOutcome FromRuleOutcome(RuleOutcome outcome) {
  switch (outcome) {
    case RuleOutcome::Compatible: return DecisionOutcome::Compatible;
    case RuleOutcome::Incompatible: return DecisionOutcome::Incompatible;
    case RuleOutcome::Unknown: return DecisionOutcome::Unknown;
  }
  return DecisionOutcome::Unknown;
}

Result<DecisionOutcome> ParseDecisionOutcome(std::string_view text) {
  if (text == "compatible") return DecisionOutcome::Compatible;
  if (text == "incompatible") return DecisionOutcome::Incompatible;
  if (text == "unknown") return DecisionOutcome::Unknown;
  return MakeError(ErrorCode::InvalidArgument, "unknown decision outcome", std::string(text));
}

std::string_view DecisionReasonName(DecisionReason reason) {
  switch (reason) {
    case DecisionReason::DecidedByRule: return "decided_by_rule";
    case DecisionReason::NoMatchingRule: return "no_matching_rule";
    case DecisionReason::AllMatchedRulesSkipped: return "all_matched_rules_skipped";
    case DecisionReason::UnregisteredComponentKind: return "unregistered_component_kind";
    case DecisionReason::UnregisteredComponentFamily: return "unregistered_component_family";
    case DecisionReason::KindFamilyMismatch: return "kind_family_mismatch";
    case DecisionReason::NoActiveRules: return "no_active_rules";
  }
  return "no_matching_rule";
}

std::string_view ConstraintStatusName(ConstraintStatus status) {
  switch (status) {
    case ConstraintStatus::Satisfied: return "satisfied";
    case ConstraintStatus::Violated: return "violated";
    case ConstraintStatus::Indeterminate: return "indeterminate";
  }
  return "satisfied";
}

std::string_view RuleEvaluationStatusName(RuleEvaluationStatus status) {
  switch (status) {
    case RuleEvaluationStatus::Satisfied: return "satisfied";
    case RuleEvaluationStatus::Violated: return "violated";
    case RuleEvaluationStatus::Indeterminate: return "indeterminate";
    case RuleEvaluationStatus::Skipped: return "skipped";
  }
  return "satisfied";
}

std::string_view NegotiationStatusName(NegotiationStatus status) {
  switch (status) {
    case NegotiationStatus::Agreed: return "agreed";
    case NegotiationStatus::NoOverlap: return "no_overlap";
    case NegotiationStatus::Indeterminate: return "indeterminate";
    case NegotiationStatus::NotSupported: return "not_supported";
  }
  return "indeterminate";
}

ComponentRef MakeComponentRef(const ComponentSpec& spec) {
  ComponentRef ref;
  ref.instance = spec.instance;
  ref.label = spec.label;
  ref.kind = spec.kind;
  ref.family = spec.family;
  ref.version = spec.version;
  ref.hardware_class = spec.hardware_class;
  return ref;
}

Result<std::shared_ptr<const RegistryGeneration>> RegistryGeneration::Compile(
    const RegistryDocument& document, const ValidationOptions& validation) {
  RegistryDocument normalized = document;
  normalized.Normalize();
  const ValidationReport report = ValidateRegistryDocument(normalized, validation);
  if (!report.Publishable()) {
    std::string detail;
    for (const Diagnostic& diagnostic : report.diagnostics) {
      if (diagnostic.severity != Severity::Error) continue;
      if (!detail.empty()) detail.append("; ");
      detail.append(std::string(DiagnosticCodeName(diagnostic.code)));
      detail.push_back(' ');
      detail.append(diagnostic.path);
      detail.append(": ");
      detail.append(diagnostic.message);
      if (detail.size() > 2048) break;
    }
    return MakeError(ErrorCode::ValidationFailed, "registry generation failed static validation",
                     std::move(detail));
  }
  auto generation = std::shared_ptr<RegistryGeneration>(new RegistryGeneration());
  generation->document_ = std::move(normalized);
  generation->id_.number = generation->document_.meta.generation;
  generation->id_.digest = generation->document_.Digest();
  for (std::size_t i = 0; i < generation->document_.rules.size(); ++i) {
    const Rule& rule = generation->document_.rules[i];
    if (rule.lifecycle.state != LifecycleState::Active) continue;
    if (rule.arity == RuleArity::Set) {
      generation->active_set_rules_.push_back(i);
    } else {
      generation->active_pair_rules_.push_back(i);
    }
  }
  return std::shared_ptr<const RegistryGeneration>(std::move(generation));
}

std::vector<std::size_t> RegistryGeneration::MatchPairRules(const ComponentSpec& left,
                                                            const ComponentSpec& right) const {
  std::vector<std::size_t> matched;
  for (std::size_t index : active_pair_rules_) {
    const Rule& rule = document_.rules[index];
    bool mirrored = false;
    if (RuleMatchesSelectors(rule, left, right, &mirrored)) matched.push_back(index);
  }
  std::stable_sort(matched.begin(), matched.end(), [this](std::size_t a, std::size_t b) {
    return PrecedenceHigher(RulePrecedence(document_.rules[a]), RulePrecedence(document_.rules[b]));
  });
  return matched;
}

Decision RegistryGeneration::EvaluatePairInternal(const ComponentSpec& left,
                                                  const ComponentSpec& right) const {
  Decision decision;
  decision.generation = id_;
  decision.arity = RuleArity::Pair;
  decision.subjects = {MakeComponentRef(left), MakeComponentRef(right)};

  const std::vector<ComponentSpec> subject_storage{left, right};
  const std::vector<const ComponentSpec*> subjects = ToPointerList(subject_storage);

  const KindDescriptor* left_kind = taxonomy().FindKind(left.kind);
  const KindDescriptor* right_kind = taxonomy().FindKind(right.kind);
  bool rules_evaluated = true;
  if (!taxonomy().HasFamily(left.family) || !taxonomy().HasFamily(right.family)) {
    decision.reason = DecisionReason::UnregisteredComponentFamily;
    rules_evaluated = false;
  } else if (left_kind == nullptr || right_kind == nullptr) {
    decision.reason = DecisionReason::UnregisteredComponentKind;
    rules_evaluated = false;
  } else if (!(left_kind->family == left.family) || !(right_kind->family == right.family)) {
    decision.reason = DecisionReason::KindFamilyMismatch;
    rules_evaluated = false;
  } else if (active_pair_rules_.empty()) {
    decision.reason = DecisionReason::NoActiveRules;
    rules_evaluated = false;
  }

  std::optional<std::size_t> deciding_index;
  RuleApplication deciding_application;

  if (rules_evaluated) {
    const std::vector<std::size_t> matched = MatchPairRules(left, right);
    decision.total_matched_rules = matched.size();
    SideResolver resolver;
    resolver.left = {&left};
    resolver.right = {&right};
    resolver.all = subjects;

    bool decider_recorded = false;
    for (std::size_t rank = 0; rank < matched.size(); ++rank) {
      const std::size_t index = matched[rank];
      const Rule& rule = document_.rules[index];
      bool mirrored = false;
      (void)RuleMatchesSelectors(rule, left, right, &mirrored);
      RuleApplication application = ApplyRule(rule, taxonomy(), resolver);
      const bool decides = !deciding_index.has_value() && application.resolved.has_value() &&
                           application.status != RuleEvaluationStatus::Skipped;
      if (decides) {
        deciding_index = index;
        deciding_application = application;
      }
      RuleEvaluation evaluation;
      evaluation.rule = rule.Ref();
      evaluation.precedence = RulePrecedence(rule);
      evaluation.precedence_rank = rank;
      evaluation.mirrored = mirrored;
      evaluation.status = application.status;
      evaluation.resolved_outcome = application.resolved;
      evaluation.decided = decides;
      evaluation.constraints = std::move(application.constraints);
      evaluation.rationale = application.rationale;
      if (decision.matched_rules.size() < kMaxEvidenceRules) {
        decision.matched_rules.push_back(std::move(evaluation));
      } else {
        decision.evidence_truncated = true;
        if (decides && !decider_recorded) {
          decision.matched_rules.back() = std::move(evaluation);
          decider_recorded = true;
        }
      }
    }
    if (deciding_index.has_value()) {
      decision.reason = DecisionReason::DecidedByRule;
      decision.outcome = deciding_application.resolved.value();
      decision.deciding_rule = document_.rules[*deciding_index].Ref();
      decision.unmet_requirements = deciding_application.unmet;
      RecordOverrideNotes(&decision);
    } else if (decision.total_matched_rules == 0) {
      decision.reason = DecisionReason::NoMatchingRule;
      decision.outcome = DecisionOutcome::Unknown;
    } else {
      decision.reason = DecisionReason::AllMatchedRulesSkipped;
      decision.outcome = DecisionOutcome::Unknown;
    }
  } else {
    decision.outcome = DecisionOutcome::Unknown;
  }

  NegotiationPolicy policy = NegotiationPolicy::HighestCommon;
  if (deciding_index.has_value()) policy = document_.rules[*deciding_index].negotiation;
  decision.negotiation = Negotiate(subjects, policy, taxonomy());

  std::string extra;
  if (decision.evidence_truncated) extra = "evidence_truncated";
  decision.id = ComputeDecisionId(id_, "pair", subject_storage, decision.outcome, decision.reason,
                                  decision.deciding_rule, decision.matched_rules,
                                  decision.negotiation, extra);
  return decision;
}

Decision RegistryGeneration::EvaluatePair(const ComponentSpec& left,
                                          const ComponentSpec& right) const {
  Decision decision = EvaluatePairInternal(left, right);
  decision.narrative = RenderDecision(decision);
  return decision;
}

Decision RegistryGeneration::EvaluateSetLevel(const std::vector<ComponentSpec>& members) const {
  Decision decision;
  decision.generation = id_;
  decision.arity = RuleArity::Set;
  for (const ComponentSpec& member : members) decision.subjects.push_back(MakeComponentRef(member));
  decision.outcome = DecisionOutcome::Unknown;
  decision.reason = DecisionReason::NoMatchingRule;

  const std::vector<const ComponentSpec*> all = ToPointerList(members);
  bool well_formed = true;
  for (const ComponentSpec& member : members) {
    const KindDescriptor* kind = taxonomy().FindKind(member.kind);
    if (!taxonomy().HasFamily(member.family)) {
      decision.reason = DecisionReason::UnregisteredComponentFamily;
      well_formed = false;
      break;
    }
    if (kind == nullptr) {
      decision.reason = DecisionReason::UnregisteredComponentKind;
      well_formed = false;
      break;
    }
    if (!(kind->family == member.family)) {
      decision.reason = DecisionReason::KindFamilyMismatch;
      well_formed = false;
      break;
    }
  }
  if (well_formed && active_set_rules_.empty()) {
    decision.reason = DecisionReason::NoActiveRules;
    well_formed = false;
  }

  std::optional<std::size_t> deciding_index;
  RuleApplication deciding_application;
  if (well_formed) {
    std::vector<std::size_t> matched;
    for (std::size_t index : active_set_rules_) {
      const Rule& rule = document_.rules[index];
      for (const ComponentSpec& member : members) {
        if (SelectorMatches(rule.left, member)) {
          matched.push_back(index);
          break;
        }
      }
    }
    std::stable_sort(matched.begin(), matched.end(), [this](std::size_t a, std::size_t b) {
      return PrecedenceHigher(RulePrecedence(document_.rules[a]),
                              RulePrecedence(document_.rules[b]));
    });
    decision.total_matched_rules = matched.size();

    for (std::size_t rank = 0; rank < matched.size(); ++rank) {
      const std::size_t index = matched[rank];
      const Rule& rule = document_.rules[index];
      SideResolver resolver;
      for (const ComponentSpec* member : all) {
        if (SelectorMatches(rule.left, *member)) resolver.left.push_back(member);
      }
      resolver.all = all;
      RuleApplication application = ApplyRule(rule, taxonomy(), resolver);
      const bool decides = !deciding_index.has_value() && application.resolved.has_value() &&
                           application.status != RuleEvaluationStatus::Skipped;
      if (decides) {
        deciding_index = index;
        deciding_application = application;
      }
      RuleEvaluation evaluation;
      evaluation.rule = rule.Ref();
      evaluation.precedence = RulePrecedence(rule);
      evaluation.precedence_rank = rank;
      evaluation.status = application.status;
      evaluation.resolved_outcome = application.resolved;
      evaluation.decided = decides;
      evaluation.constraints = std::move(application.constraints);
      evaluation.rationale = application.rationale;
      if (decision.matched_rules.size() < kMaxEvidenceRules) {
        decision.matched_rules.push_back(std::move(evaluation));
      } else {
        decision.evidence_truncated = true;
        if (decides) decision.matched_rules.back() = std::move(evaluation);
      }
    }
    if (deciding_index.has_value()) {
      decision.reason = DecisionReason::DecidedByRule;
      decision.outcome = deciding_application.resolved.value();
      decision.deciding_rule = document_.rules[*deciding_index].Ref();
      decision.unmet_requirements = deciding_application.unmet;
      RecordOverrideNotes(&decision);
    } else if (decision.total_matched_rules == 0) {
      decision.reason = DecisionReason::NoMatchingRule;
      decision.outcome = DecisionOutcome::Unknown;
    } else {
      decision.reason = DecisionReason::AllMatchedRulesSkipped;
      decision.outcome = DecisionOutcome::Unknown;
    }
  }

  NegotiationPolicy policy = NegotiationPolicy::HighestCommon;
  if (deciding_index.has_value()) policy = document_.rules[*deciding_index].negotiation;
  decision.negotiation = Negotiate(all, policy, taxonomy());

  std::string extra = "set_level";
  if (decision.evidence_truncated) extra = "set_level,evidence_truncated";
  decision.id = ComputeDecisionId(id_, "set", members, decision.outcome, decision.reason,
                                  decision.deciding_rule, decision.matched_rules,
                                  decision.negotiation, extra);
  return decision;
}

Result<SetDecision> RegistryGeneration::EvaluateSet(
    const std::vector<ComponentSpec>& members) const {
  if (members.size() < 2) {
    return MakeError(ErrorCode::InvalidArgument,
                     "a set compatibility query needs at least two components");
  }
  if (members.size() > kMaxSetMembers) {
    return MakeError(ErrorCode::LimitExceeded,
                     "a set compatibility query is limited to " + std::to_string(kMaxSetMembers) +
                         " components");
  }
  SetDecision result;
  result.generation = id_;
  for (const ComponentSpec& member : members) result.members.push_back(MakeComponentRef(member));
  result.set_level = EvaluateSetLevel(members);
  result.set_level.narrative = RenderDecision(result.set_level);
  for (std::size_t i = 0; i < members.size(); ++i) {
    for (std::size_t j = i + 1; j < members.size(); ++j) {
      Decision pair = EvaluatePairInternal(members[i], members[j]);
      pair.narrative = RenderDecision(pair);
      result.pairs.push_back(std::move(pair));
    }
  }
  result.outcome = DecisionOutcome::Compatible;
  bool any_unknown = false;
  result.outcome = DecisionOutcome::Compatible;
  for (const Decision& decision : result.pairs) {
    if (decision.outcome == DecisionOutcome::Incompatible) {
      result.outcome = DecisionOutcome::Incompatible;
      break;
    }
    if (decision.outcome == DecisionOutcome::Unknown) any_unknown = true;
  }
  if (result.outcome != DecisionOutcome::Incompatible) {
    if (result.set_level.outcome == DecisionOutcome::Incompatible) {
      result.outcome = DecisionOutcome::Incompatible;
    } else if (result.set_level.outcome == DecisionOutcome::Unknown || any_unknown) {
      result.outcome = DecisionOutcome::Unknown;
    }
  }
  {
    JsonValue payload = JsonValue::Obj();
    JsonValue gen = JsonValue::Obj();
    gen.Set("number", JsonValue::UInt(id_.number.value()));
    gen.Set("digest", JsonValue::Str(id_.digest.ToHex()));
    payload.Set("generation", std::move(gen));
    JsonValue::Array subjects;
    for (const ComponentSpec& member : members) subjects.push_back(SemanticSpecJson(member));
    payload.Set("subjects", JsonValue::Arr(std::move(subjects)));
    payload.Set("outcome", JsonValue::Str(std::string(DecisionOutcomeName(result.outcome))));
    payload.Set("set_level", JsonValue::Str(result.set_level.id.ToHex()));
    JsonValue::Array pairs;
    for (const Decision& decision : result.pairs) {
      pairs.push_back(JsonValue::Str(decision.id.ToHex()));
    }
    payload.Set("pairs", JsonValue::Arr(std::move(pairs)));
    result.id.digest = Sha256::Hash(payload.Dump(false));
  }
  result.narrative = RenderSetDecision(result);
  return result;
}

}  // namespace fcr
