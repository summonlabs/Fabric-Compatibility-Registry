#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "fcr/digest.hpp"
#include "fcr/registry.hpp"

namespace fcr {
namespace {

std::string RefText(const std::optional<RuleRef>& ref) {
  return ref.has_value() ? ref->ToString() : std::string("-");
}

template <class Descriptor>
bool DescriptorEqual(const Descriptor& a, const Descriptor& b) {
  if constexpr (std::is_same_v<Descriptor, KindDescriptor>) {
    return a.family == b.family && a.description == b.description;
  } else if constexpr (std::is_same_v<Descriptor, CapabilityDeclaration>) {
    return a.type == b.type && a.description == b.description;
  } else {
    return a.description == b.description;
  }
}

void AppendSubject(std::string& out, std::size_t index, const ComponentRef& subject) {
  out.append("  subject[");
  out.append(std::to_string(index));
  out.append("]: ");
  out.append(subject.kind.value());
  out.push_back(' ');
  out.append(subject.version.ToString());
  out.append(" (family=");
  out.append(subject.family.value());
  if (subject.hardware_class.has_value()) {
    out.append(", hardware_class=");
    out.append(subject.hardware_class->value());
  }
  if (!subject.instance.empty()) {
    out.append(", instance=");
    out.append(subject.instance.value());
  }
  if (!subject.label.empty()) {
    out.append(", label=");
    out.append(subject.label);
  }
  out.append(")\n");
}

}  // namespace

std::string_view RuleDiffKindName(RuleDiffKind kind) {
  switch (kind) {
    case RuleDiffKind::Added: return "added";
    case RuleDiffKind::Removed: return "removed";
    case RuleDiffKind::Modified: return "modified";
    case RuleDiffKind::Unchanged: return "unchanged";
  }
  return "unchanged";
}

JsonValue DecisionToJson(const Decision& decision) {
  JsonValue out = JsonValue::Obj();
  out.Set("decision_id", JsonValue::Str(decision.id.ToHex()));
  out.Set("outcome", JsonValue::Str(std::string(DecisionOutcomeName(decision.outcome))));
  out.Set("reason", JsonValue::Str(std::string(DecisionReasonName(decision.reason))));
  out.Set("arity", JsonValue::Str(std::string(RuleArityName(decision.arity))));
  {
    JsonValue generation = JsonValue::Obj();
    generation.Set("number", JsonValue::UInt(decision.generation.number.value()));
    generation.Set("digest", JsonValue::Str(decision.generation.digest.ToHex()));
    generation.Set("id", JsonValue::Str(decision.generation.ToString()));
    out.Set("generation", std::move(generation));
  }
  {
    JsonValue::Array subjects;
    for (const ComponentRef& subject : decision.subjects) {
      JsonValue entry = JsonValue::Obj();
      if (!subject.instance.empty()) entry.Set("instance", JsonValue::Str(subject.instance.value()));
      if (!subject.label.empty()) entry.Set("label", JsonValue::Str(subject.label));
      entry.Set("family", JsonValue::Str(subject.family.value()));
      entry.Set("kind", JsonValue::Str(subject.kind.value()));
      entry.Set("version", JsonValue::Str(subject.version.ToString()));
      if (subject.hardware_class.has_value()) {
        entry.Set("hardware_class", JsonValue::Str(subject.hardware_class->value()));
      }
      subjects.push_back(std::move(entry));
    }
    out.Set("subjects", JsonValue::Arr(std::move(subjects)));
  }
  if (decision.deciding_rule.has_value()) {
    out.Set("deciding_rule", JsonValue::Str(decision.deciding_rule->ToString()));
  }
  out.Set("total_matched_rules", JsonValue::UInt(decision.total_matched_rules));
  out.Set("evidence_truncated", JsonValue::Bool(decision.evidence_truncated));
  {
    JsonValue::Array matched;
    matched.reserve(decision.matched_rules.size());
    for (const RuleEvaluation& evaluation : decision.matched_rules) {
      JsonValue entry = JsonValue::Obj();
      entry.Set("rule", JsonValue::Str(evaluation.rule.ToString()));
      entry.Set("precedence_rank", JsonValue::UInt(evaluation.precedence_rank));
      JsonValue tier = JsonValue::Obj();
      tier.Set("priority", JsonValue::Int(evaluation.precedence.tier.priority.value()));
      tier.Set("specificity", JsonValue::Int(evaluation.precedence.tier.specificity));
      tier.Set("origin_rank", JsonValue::Int(evaluation.precedence.tier.origin_rank));
      entry.Set("tier", std::move(tier));
      entry.Set("mirrored", JsonValue::Bool(evaluation.mirrored));
      entry.Set("status", JsonValue::Str(std::string(RuleEvaluationStatusName(evaluation.status))));
      if (evaluation.resolved_outcome.has_value()) {
        entry.Set("resolved_outcome",
                  JsonValue::Str(std::string(DecisionOutcomeName(*evaluation.resolved_outcome))));
      }
      entry.Set("decided", JsonValue::Bool(evaluation.decided));
      entry.Set("rationale", JsonValue::Str(evaluation.rationale));
      JsonValue::Array constraints;
      constraints.reserve(evaluation.constraints.size());
      for (const ConstraintEvaluation& constraint : evaluation.constraints) {
        JsonValue item = ConstraintToJson(constraint.constraint);
        item.Set("status", JsonValue::Str(std::string(ConstraintStatusName(constraint.status))));
        if (!constraint.detail.empty()) item.Set("detail", JsonValue::Str(constraint.detail));
        constraints.push_back(std::move(item));
      }
      entry.Set("constraints", JsonValue::Arr(std::move(constraints)));
      matched.push_back(std::move(entry));
    }
    out.Set("matched_rules", JsonValue::Arr(std::move(matched)));
  }
  {
    JsonValue::Array unmet;
    unmet.reserve(decision.unmet_requirements.size());
    for (const UnmetRequirement& requirement : decision.unmet_requirements) {
      JsonValue entry = JsonValue::Obj();
      entry.Set("rule", JsonValue::Str(requirement.rule.ToString()));
      entry.Set("side", JsonValue::Str(std::string(SideName(requirement.side))));
      entry.Set("requirement", JsonValue::Str(requirement.requirement));
      entry.Set("status", JsonValue::Str(std::string(ConstraintStatusName(requirement.status))));
      entry.Set("detail", JsonValue::Str(requirement.detail));
      unmet.push_back(std::move(entry));
    }
    out.Set("unmet_requirements", JsonValue::Arr(std::move(unmet)));
  }
  {
    JsonValue::Array overridden;
    overridden.reserve(decision.overridden.size());
    for (const OverriddenNote& note : decision.overridden) {
      JsonValue entry = JsonValue::Obj();
      entry.Set("rule", JsonValue::Str(note.rule.ToString()));
      entry.Set("outcome", JsonValue::Str(std::string(DecisionOutcomeName(note.outcome))));
      entry.Set("detail", JsonValue::Str(note.detail));
      overridden.push_back(std::move(entry));
    }
    out.Set("overridden", JsonValue::Arr(std::move(overridden)));
  }
  {
    JsonValue negotiation = JsonValue::Obj();
    negotiation.Set("policy",
                    JsonValue::Str(std::string(NegotiationPolicyName(decision.negotiation.policy))));
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
    negotiation.Set("protocols", emit(decision.negotiation.protocols));
    negotiation.Set("schemas", emit(decision.negotiation.schemas));
    out.Set("negotiation", std::move(negotiation));
  }
  out.Set("narrative", JsonValue::Str(decision.narrative));
  return out;
}

JsonValue SetDecisionToJson(const SetDecision& decision) {
  JsonValue out = JsonValue::Obj();
  out.Set("decision_id", JsonValue::Str(decision.id.ToHex()));
  out.Set("outcome", JsonValue::Str(std::string(DecisionOutcomeName(decision.outcome))));
  {
    JsonValue generation = JsonValue::Obj();
    generation.Set("number", JsonValue::UInt(decision.generation.number.value()));
    generation.Set("digest", JsonValue::Str(decision.generation.digest.ToHex()));
    generation.Set("id", JsonValue::Str(decision.generation.ToString()));
    out.Set("generation", std::move(generation));
  }
  {
    JsonValue::Array members;
    for (const ComponentRef& member : decision.members) {
      JsonValue entry = JsonValue::Obj();
      if (!member.instance.empty()) entry.Set("instance", JsonValue::Str(member.instance.value()));
      if (!member.label.empty()) entry.Set("label", JsonValue::Str(member.label));
      entry.Set("family", JsonValue::Str(member.family.value()));
      entry.Set("kind", JsonValue::Str(member.kind.value()));
      entry.Set("version", JsonValue::Str(member.version.ToString()));
      if (member.hardware_class.has_value()) {
        entry.Set("hardware_class", JsonValue::Str(member.hardware_class->value()));
      }
      members.push_back(std::move(entry));
    }
    out.Set("members", JsonValue::Arr(std::move(members)));
  }
  out.Set("set_level", DecisionToJson(decision.set_level));
  {
    JsonValue::Array pairs;
    pairs.reserve(decision.pairs.size());
    for (const Decision& pair : decision.pairs) pairs.push_back(DecisionToJson(pair));
    out.Set("pairs", JsonValue::Arr(std::move(pairs)));
  }
  out.Set("narrative", JsonValue::Str(decision.narrative));
  return out;
}

std::string RenderDecision(const Decision& decision) {
  std::string out;
  out.append("decision ");
  out.append(decision.id.ToHex());
  out.push_back('\n');
  out.append("  outcome: ");
  out.append(DecisionOutcomeName(decision.outcome));
  out.push_back('\n');
  out.append("  reason: ");
  out.append(DecisionReasonName(decision.reason));
  out.push_back('\n');
  out.append("  generation: ");
  out.append(decision.generation.ToString());
  out.push_back('\n');
  out.append("  arity: ");
  out.append(RuleArityName(decision.arity));
  out.push_back('\n');
  for (std::size_t i = 0; i < decision.subjects.size(); ++i) {
    AppendSubject(out, i, decision.subjects[i]);
  }
  out.append("  deciding_rule: ");
  out.append(RefText(decision.deciding_rule));
  out.push_back('\n');
  out.append("  matched_rules: ");
  out.append(std::to_string(decision.total_matched_rules));
  if (decision.evidence_truncated) out.append(" (evidence list truncated)");
  out.push_back('\n');
  for (const RuleEvaluation& evaluation : decision.matched_rules) {
    out.append("    [");
    out.append(std::to_string(evaluation.precedence_rank));
    out.append("] ");
    out.append(evaluation.rule.ToString());
    out.append(" priority=");
    out.append(std::to_string(evaluation.precedence.tier.priority.value()));
    out.append(" specificity=");
    out.append(std::to_string(evaluation.precedence.tier.specificity));
    out.append(" origin_rank=");
    out.append(std::to_string(evaluation.precedence.tier.origin_rank));
    out.append(" status=");
    out.append(RuleEvaluationStatusName(evaluation.status));
    if (evaluation.resolved_outcome.has_value()) {
      out.append(" outcome=");
      out.append(DecisionOutcomeName(*evaluation.resolved_outcome));
    }
    if (evaluation.mirrored) out.append(" mirrored=true");
    if (evaluation.decided) out.append(" DECIDED");
    out.push_back('\n');
    for (const ConstraintEvaluation& constraint : evaluation.constraints) {
      out.append("        ");
      out.append(ConstraintKindName(constraint.constraint));
      out.append(" side=");
      out.append(SideName(ConstraintSide(constraint.constraint)));
      out.append(" status=");
      out.append(ConstraintStatusName(constraint.status));
      if (!constraint.detail.empty()) {
        out.append(": ");
        out.append(constraint.detail);
      }
      out.push_back('\n');
    }
  }
  if (!decision.unmet_requirements.empty()) {
    out.append("  unmet_requirements: ");
    out.append(std::to_string(decision.unmet_requirements.size()));
    out.push_back('\n');
    for (const UnmetRequirement& requirement : decision.unmet_requirements) {
      out.append("    - ");
      out.append(requirement.rule.ToString());
      out.append(" side=");
      out.append(SideName(requirement.side));
      out.append(" ");
      out.append(requirement.requirement);
      out.append(" ");
      out.append(ConstraintStatusName(requirement.status));
      out.append(": ");
      out.append(requirement.detail);
      out.push_back('\n');
    }
  }
  if (!decision.overridden.empty()) {
    out.append("  overridden: ");
    out.append(std::to_string(decision.overridden.size()));
    out.push_back('\n');
    for (const OverriddenNote& note : decision.overridden) {
      out.append("    - ");
      out.append(note.rule.ToString());
      out.append(" would have produced ");
      out.append(DecisionOutcomeName(note.outcome));
      out.push_back('\n');
    }
  }
  if (!decision.negotiation.empty()) {
    out.append("  negotiation: policy=");
    out.append(NegotiationPolicyName(decision.negotiation.policy));
    out.push_back('\n');
    const auto emit = [&out](const char* label, const std::vector<NegotiatedSubject>& subjects) {
      for (const NegotiatedSubject& subject : subjects) {
        out.append("    ");
        out.append(label);
        out.push_back(' ');
        out.append(subject.subject);
        out.append(" status=");
        out.append(NegotiationStatusName(subject.status));
        if (subject.selected.has_value()) {
          out.append(" selected=");
          out.append(subject.selected->ToString());
        }
        if (!subject.window.empty()) {
          out.append(" window=[");
          for (std::size_t i = 0; i < subject.window.size(); ++i) {
            if (i != 0) out.push_back(',');
            out.append(subject.window[i].ToString());
          }
          out.push_back(']');
        }
        if (!subject.owner.empty()) {
          out.append(" owner=");
          out.append(subject.owner);
        }
        out.push_back('\n');
      }
    };
    emit("protocol", decision.negotiation.protocols);
    emit("schema", decision.negotiation.schemas);
  }
  return out;
}

std::string RenderSetDecision(const SetDecision& decision) {
  std::string out;
  out.append("set decision ");
  out.append(decision.id.ToHex());
  out.push_back('\n');
  out.append("  outcome: ");
  out.append(DecisionOutcomeName(decision.outcome));
  out.push_back('\n');
  out.append("  generation: ");
  out.append(decision.generation.ToString());
  out.push_back('\n');
  out.append("  members: ");
  out.append(std::to_string(decision.members.size()));
  out.push_back('\n');
  for (std::size_t i = 0; i < decision.members.size(); ++i) {
    AppendSubject(out, i, decision.members[i]);
  }
  out.append("  pair outcomes:");
  if (decision.pairs.empty()) {
    out.append(" none\n");
  } else {
    out.push_back('\n');
    for (const Decision& pair : decision.pairs) {
      out.append("    ");
      out.append(pair.subjects.size() >= 2 ? pair.subjects[0].kind.value() : std::string("-"));
      out.push_back(' ');
      out.append(pair.subjects.size() >= 2 ? pair.subjects[0].version.ToString() : std::string("-"));
      out.append(" vs ");
      out.append(pair.subjects.size() >= 2 ? pair.subjects[1].kind.value() : std::string("-"));
      out.push_back(' ');
      out.append(pair.subjects.size() >= 2 ? pair.subjects[1].version.ToString() : std::string("-"));
      out.append(": ");
      out.append(DecisionOutcomeName(pair.outcome));
      out.append(" (");
      out.append(pair.id.ToHex().substr(0, 16));
      out.append(")\n");
    }
  }
  out.append("  set-level: ");
  out.append(DecisionOutcomeName(decision.set_level.outcome));
  out.append(" reason=");
  out.append(DecisionReasonName(decision.set_level.reason));
  out.push_back('\n');
  return out;
}

GenerationDiff DiffGenerations(const RegistryGeneration& before, const RegistryGeneration& after) {
  GenerationDiff diff;
  diff.from = before.id();
  diff.to = after.id();

  std::map<RuleId, const Rule*> before_rules;
  std::map<RuleId, const Rule*> after_rules;
  for (const Rule& rule : before.document().rules) before_rules.emplace(rule.id, &rule);
  for (const Rule& rule : after.document().rules) after_rules.emplace(rule.id, &rule);

  std::set<RuleId> ids;
  for (const auto& [id, rule] : before_rules) {
    (void)rule;
    ids.insert(id);
  }
  for (const auto& [id, rule] : after_rules) {
    (void)rule;
    ids.insert(id);
  }
  for (const RuleId& id : ids) {
    const auto before_it = before_rules.find(id);
    const auto after_it = after_rules.find(id);
    RuleDiffEntry entry;
    if (before_it == before_rules.end()) {
      entry.rule = after_it->second->Ref();
      entry.kind = RuleDiffKind::Added;
      entry.after_digest = RuleContentDigest(*after_it->second);
      ++diff.added;
    } else if (after_it == after_rules.end()) {
      entry.rule = before_it->second->Ref();
      entry.kind = RuleDiffKind::Removed;
      entry.before_digest = RuleContentDigest(*before_it->second);
      ++diff.removed;
    } else {
      const Rule& a = *before_it->second;
      const Rule& b = *after_it->second;
      entry.rule = b.Ref();
      entry.before_digest = RuleContentDigest(a);
      entry.after_digest = RuleContentDigest(b);
      if (*entry.before_digest == *entry.after_digest) {
        entry.kind = RuleDiffKind::Unchanged;
        ++diff.unchanged;
      } else {
        entry.kind = RuleDiffKind::Modified;
        ++diff.modified;
        if (a.revision != b.revision) entry.changed_fields.push_back("revision");
        if (a.priority != b.priority) entry.changed_fields.push_back("priority");
        if (a.origin != b.origin) entry.changed_fields.push_back("origin");
        if (a.symmetry != b.symmetry) entry.changed_fields.push_back("symmetry");
        if (a.arity != b.arity) entry.changed_fields.push_back("arity");
        if (a.outcome != b.outcome) entry.changed_fields.push_back("outcome");
        if (!(a.left == b.left)) entry.changed_fields.push_back("left");
        if (!(a.right == b.right)) entry.changed_fields.push_back("right");
        if (!ConstraintListsEqual(a.constraints, b.constraints)) {
          entry.changed_fields.push_back("constraints");
        }
        if (!std::equal(a.implications.begin(), a.implications.end(), b.implications.begin(),
                        b.implications.end(),
                        [](const CapabilityImplication& x, const CapabilityImplication& y) {
                          return x.side == y.side && x.if_present == y.if_present &&
                                 x.then_required == y.then_required;
                        })) {
          entry.changed_fields.push_back("implications");
        }
        if (a.negotiation != b.negotiation) entry.changed_fields.push_back("negotiation");
        if (a.on_unmet != b.on_unmet) entry.changed_fields.push_back("on_unmet_requirement");
        if (a.on_indeterminate != b.on_indeterminate) {
          entry.changed_fields.push_back("on_indeterminate");
        }
        if (!(a.lifecycle == b.lifecycle)) entry.changed_fields.push_back("lifecycle");
        if (!(a.provenance == b.provenance)) entry.changed_fields.push_back("provenance");
        if (a.rationale != b.rationale) entry.changed_fields.push_back("rationale");
      }
    }
    diff.rules.push_back(std::move(entry));
  }
  std::sort(diff.rules.begin(), diff.rules.end(),
            [](const RuleDiffEntry& a, const RuleDiffEntry& b) { return a.rule < b.rule; });

  const Taxonomy& ta = before.taxonomy();
  const Taxonomy& tb = after.taxonomy();
  const auto compare_section = [&diff](const char* label, const auto& a, const auto& b) {
    for (const auto& [id, descriptor] : a) {
      (void)descriptor;
      if (b.count(id) == 0) {
        diff.taxonomy_changes.push_back(std::string(label) + ": removed '" + id.value() + "'");
      }
    }
    for (const auto& [id, descriptor] : b) {
      const auto it = a.find(id);
      if (it == a.end()) {
        diff.taxonomy_changes.push_back(std::string(label) + ": added '" + id.value() + "'");
        continue;
      }
      if (!DescriptorEqual(it->second, descriptor)) {
        diff.taxonomy_changes.push_back(std::string(label) + ": changed '" + id.value() + "'");
      }
    }
  };
  compare_section("families", ta.families(), tb.families());
  compare_section("kinds", ta.kinds(), tb.kinds());
  compare_section("protocols", ta.protocols(), tb.protocols());
  compare_section("schemas", ta.schemas(), tb.schemas());
  compare_section("hardware_classes", ta.hardware_classes(), tb.hardware_classes());
  compare_section("features", ta.features(), tb.features());
  compare_section("capabilities", ta.capabilities(), tb.capabilities());
  std::sort(diff.taxonomy_changes.begin(), diff.taxonomy_changes.end());
  return diff;
}

JsonValue GenerationDiffToJson(const GenerationDiff& diff) {
  JsonValue out = JsonValue::Obj();
  {
    JsonValue from = JsonValue::Obj();
    from.Set("number", JsonValue::UInt(diff.from.number.value()));
    from.Set("digest", JsonValue::Str(diff.from.digest.ToHex()));
    from.Set("id", JsonValue::Str(diff.from.ToString()));
    out.Set("from", std::move(from));
  }
  {
    JsonValue to = JsonValue::Obj();
    to.Set("number", JsonValue::UInt(diff.to.number.value()));
    to.Set("digest", JsonValue::Str(diff.to.digest.ToHex()));
    to.Set("id", JsonValue::Str(diff.to.ToString()));
    out.Set("to", std::move(to));
  }
  out.Set("added", JsonValue::UInt(diff.added));
  out.Set("removed", JsonValue::UInt(diff.removed));
  out.Set("modified", JsonValue::UInt(diff.modified));
  out.Set("unchanged", JsonValue::UInt(diff.unchanged));
  out.Set("empty", JsonValue::Bool(diff.IsEmpty()));
  {
    JsonValue::Array rules;
    rules.reserve(diff.rules.size());
    for (const RuleDiffEntry& entry : diff.rules) {
      JsonValue item = JsonValue::Obj();
      item.Set("rule", JsonValue::Str(entry.rule.ToString()));
      item.Set("kind", JsonValue::Str(std::string(RuleDiffKindName(entry.kind))));
      if (entry.before_digest.has_value()) {
        item.Set("before_digest", JsonValue::Str(entry.before_digest->ToHex()));
      }
      if (entry.after_digest.has_value()) {
        item.Set("after_digest", JsonValue::Str(entry.after_digest->ToHex()));
      }
      JsonValue::Array fields;
      for (const std::string& field : entry.changed_fields) {
        fields.push_back(JsonValue::Str(field));
      }
      item.Set("changed_fields", JsonValue::Arr(std::move(fields)));
      rules.push_back(std::move(item));
    }
    out.Set("rules", JsonValue::Arr(std::move(rules)));
  }
  {
    JsonValue::Array changes;
    for (const std::string& change : diff.taxonomy_changes) {
      changes.push_back(JsonValue::Str(change));
    }
    out.Set("taxonomy_changes", JsonValue::Arr(std::move(changes)));
  }
  return out;
}

std::string RenderGenerationDiff(const GenerationDiff& diff) {
  std::string out;
  out.append("generation diff ");
  out.append(diff.from.ToString());
  out.append(" -> ");
  out.append(diff.to.ToString());
  out.push_back('\n');
  out.append("  added: ");
  out.append(std::to_string(diff.added));
  out.append("  removed: ");
  out.append(std::to_string(diff.removed));
  out.append("  modified: ");
  out.append(std::to_string(diff.modified));
  out.append("  unchanged: ");
  out.append(std::to_string(diff.unchanged));
  out.push_back('\n');
  for (const RuleDiffEntry& entry : diff.rules) {
    if (entry.kind == RuleDiffKind::Unchanged) continue;
    out.append("  ");
    out.append(RuleDiffKindName(entry.kind));
    out.push_back(' ');
    out.append(entry.rule.ToString());
    if (!entry.changed_fields.empty()) {
      out.append(" fields=");
      for (std::size_t i = 0; i < entry.changed_fields.size(); ++i) {
        if (i != 0) out.push_back(',');
        out.append(entry.changed_fields[i]);
      }
    }
    out.push_back('\n');
  }
  for (const std::string& change : diff.taxonomy_changes) {
    out.append("  taxonomy ");
    out.append(change);
    out.push_back('\n');
  }
  return out;
}

Result<DecisionReplayDiff> ReplayDiff(const RegistryGeneration& before,
                                      const RegistryGeneration& after,
                                      const std::vector<std::vector<ComponentSpec>>& queries) {
  DecisionReplayDiff result;
  result.from = before.id();
  result.to = after.id();
  for (const std::vector<ComponentSpec>& query : queries) {
    Decision before_decision;
    Decision after_decision;
    if (query.size() == 2) {
      before_decision = before.EvaluatePair(query[0], query[1]);
      after_decision = after.EvaluatePair(query[0], query[1]);
    } else if (query.size() > 2) {
      auto before_set = before.EvaluateSet(query);
      if (!before_set.has_value()) return before_set.error();
      auto after_set = after.EvaluateSet(query);
      if (!after_set.has_value()) return after_set.error();
      DecisionChange change;
      for (const ComponentRef& member : before_set.value().members) {
        change.subjects.push_back(member);
      }
      change.before_outcome = before_set.value().outcome;
      change.after_outcome = after_set.value().outcome;
      change.before_id = before_set.value().id;
      change.after_id = after_set.value().id;
      ++result.compared;
      if (change.before_outcome != change.after_outcome ||
          !(change.before_id == change.after_id)) {
        result.changes.push_back(std::move(change));
      }
      continue;
    } else {
      return MakeError(ErrorCode::InvalidArgument,
                       "a replay query needs at least two components");
    }
    DecisionChange change;
    for (const ComponentRef& subject : before_decision.subjects) {
      change.subjects.push_back(subject);
    }
    change.before_outcome = before_decision.outcome;
    change.after_outcome = after_decision.outcome;
    change.before_id = before_decision.id;
    change.after_id = after_decision.id;
    change.before_rule = before_decision.deciding_rule;
    change.after_rule = after_decision.deciding_rule;
    ++result.compared;
    if (change.before_outcome != change.after_outcome || !(change.before_id == change.after_id)) {
      result.changes.push_back(std::move(change));
    }
  }
  return result;
}

std::vector<ProvenanceRecord> ProvenanceHistory(
    const RuleId& rule, const std::vector<const RegistryGeneration*>& generations_ascending) {
  std::vector<ProvenanceRecord> history;
  std::optional<ContentDigest> current_digest;
  for (const RegistryGeneration* generation : generations_ascending) {
    if (generation == nullptr) continue;
    const Rule* found = nullptr;
    for (const Rule& candidate : generation->document().rules) {
      if (candidate.id == rule) {
        found = &candidate;
        break;
      }
    }
    if (found == nullptr) {
      if (current_digest.has_value()) {
        ProvenanceRecord record;
        record.rule = RuleRef{rule, RuleRevision(0)};
        record.generation = generation->id().number;
        record.rule_digest = *current_digest;
        record.present = false;
        history.push_back(std::move(record));
        current_digest.reset();
      }
      continue;
    }
    const ContentDigest digest = RuleContentDigest(*found);
    if (current_digest.has_value() && *current_digest == digest) continue;
    ProvenanceRecord record;
    record.rule = found->Ref();
    record.generation = generation->id().number;
    record.rule_digest = digest;
    record.provenance = found->provenance;
    record.lifecycle = found->lifecycle.state;
    record.present = true;
    history.push_back(std::move(record));
    current_digest = digest;
  }
  return history;
}

std::string RenderProvenanceHistory(const std::vector<ProvenanceRecord>& history) {
  std::string out;
  out.append("rule provenance history (");
  out.append(std::to_string(history.size()));
  out.append(" record(s))\n");
  for (const ProvenanceRecord& record : history) {
    out.append("  gen-");
    out.append(std::to_string(record.generation.value()));
    out.push_back(' ');
    if (!record.present) {
      out.append("removed (last digest ");
      out.append(record.rule_digest.ToHex().substr(0, 16));
      out.append(")\n");
      continue;
    }
    out.append(record.rule.ToString());
    out.append(" digest=");
    out.append(record.rule_digest.ToHex().substr(0, 16));
    out.append(" state=");
    out.append(LifecycleStateName(record.lifecycle));
    out.append(" publisher=");
    out.append(record.provenance.publisher.value());
    if (!record.provenance.source.empty()) {
      out.append(" source=");
      out.append(record.provenance.source);
    }
    if (!record.provenance.reference.empty()) {
      out.append(" reference=");
      out.append(record.provenance.reference);
    }
    out.append(" recorded_at=");
    out.append(record.provenance.recorded_at.ToIso8601());
    if (!record.provenance.change_note.empty()) {
      out.append(" note=");
      out.append(record.provenance.change_note);
    }
    out.push_back('\n');
  }
  return out;
}

}  // namespace fcr
