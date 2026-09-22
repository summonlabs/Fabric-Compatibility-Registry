#include "fcr/rule.hpp"

#include <algorithm>
#include <cstdio>
#include <type_traits>

#include "fcr/digest.hpp"

namespace fcr {
namespace {

std::string PadIndex(std::size_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%03zu", value);
  return std::string(buffer);
}

std::size_t SideIndex(Side side) { return static_cast<std::size_t>(side); }

}  // namespace

std::string_view SideName(Side side) {
  switch (side) {
    case Side::Left: return "left";
    case Side::Right: return "right";
    case Side::AllMembers: return "all_members";
  }
  return "left";
}

Result<Side> ParseSide(std::string_view text) {
  if (text == "left") return Side::Left;
  if (text == "right") return Side::Right;
  if (text == "all_members" || text == "all") return Side::AllMembers;
  return MakeError(ErrorCode::InvalidConstraint, "unknown constraint side", std::string(text));
}

std::string_view RuleOriginName(RuleOrigin origin) {
  switch (origin) {
    case RuleOrigin::Standard: return "standard";
    case RuleOrigin::Vendor: return "vendor";
    case RuleOrigin::Operator: return "operator";
    case RuleOrigin::LocalOverride: return "local_override";
  }
  return "standard";
}

Result<RuleOrigin> ParseRuleOrigin(std::string_view text) {
  if (text == "standard") return RuleOrigin::Standard;
  if (text == "vendor") return RuleOrigin::Vendor;
  if (text == "operator") return RuleOrigin::Operator;
  if (text == "local_override" || text == "local") return RuleOrigin::LocalOverride;
  return MakeError(ErrorCode::InvalidDocument, "unknown rule origin", std::string(text));
}

int RuleOriginRank(RuleOrigin origin) {
  switch (origin) {
    case RuleOrigin::Standard: return 0;
    case RuleOrigin::Vendor: return 10;
    case RuleOrigin::Operator: return 20;
    case RuleOrigin::LocalOverride: return 30;
  }
  return 0;
}

std::string_view RuleSymmetryName(RuleSymmetry symmetry) {
  switch (symmetry) {
    case RuleSymmetry::Symmetric: return "symmetric";
    case RuleSymmetry::LeftToRight: return "left_to_right";
    case RuleSymmetry::RightToLeft: return "right_to_left";
  }
  return "symmetric";
}

Result<RuleSymmetry> ParseRuleSymmetry(std::string_view text) {
  if (text == "symmetric") return RuleSymmetry::Symmetric;
  if (text == "left_to_right") return RuleSymmetry::LeftToRight;
  if (text == "right_to_left") return RuleSymmetry::RightToLeft;
  return MakeError(ErrorCode::InvalidDocument, "unknown rule symmetry", std::string(text));
}

std::string_view RuleArityName(RuleArity arity) {
  switch (arity) {
    case RuleArity::Pair: return "pair";
    case RuleArity::Set: return "set";
  }
  return "pair";
}

Result<RuleArity> ParseRuleArity(std::string_view text) {
  if (text == "pair") return RuleArity::Pair;
  if (text == "set") return RuleArity::Set;
  return MakeError(ErrorCode::InvalidDocument, "unknown rule arity", std::string(text));
}

std::string_view RuleOutcomeName(RuleOutcome outcome) {
  switch (outcome) {
    case RuleOutcome::Compatible: return "compatible";
    case RuleOutcome::Incompatible: return "incompatible";
    case RuleOutcome::Unknown: return "unknown";
  }
  return "unknown";
}

Result<RuleOutcome> ParseRuleOutcome(std::string_view text) {
  if (text == "compatible") return RuleOutcome::Compatible;
  if (text == "incompatible") return RuleOutcome::Incompatible;
  if (text == "unknown") return RuleOutcome::Unknown;
  return MakeError(ErrorCode::InvalidDocument, "unknown rule outcome", std::string(text));
}

std::string_view OnUnmetRequirementName(OnUnmetRequirement policy) {
  switch (policy) {
    case OnUnmetRequirement::Incompatible: return "incompatible";
    case OnUnmetRequirement::Unknown: return "unknown";
    case OnUnmetRequirement::Skip: return "skip";
  }
  return "incompatible";
}

Result<OnUnmetRequirement> ParseOnUnmetRequirement(std::string_view text) {
  if (text == "incompatible") return OnUnmetRequirement::Incompatible;
  if (text == "unknown") return OnUnmetRequirement::Unknown;
  if (text == "skip") return OnUnmetRequirement::Skip;
  return MakeError(ErrorCode::InvalidDocument, "unknown unmet-requirement policy",
                   std::string(text));
}

std::string_view OnIndeterminateName(OnIndeterminate policy) {
  switch (policy) {
    case OnIndeterminate::Unknown: return "unknown";
    case OnIndeterminate::Incompatible: return "incompatible";
    case OnIndeterminate::Skip: return "skip";
  }
  return "unknown";
}

Result<OnIndeterminate> ParseOnIndeterminate(std::string_view text) {
  if (text == "unknown") return OnIndeterminate::Unknown;
  if (text == "incompatible") return OnIndeterminate::Incompatible;
  if (text == "skip") return OnIndeterminate::Skip;
  return MakeError(ErrorCode::InvalidDocument, "unknown indeterminate policy", std::string(text));
}

std::string_view NegotiationPolicyName(NegotiationPolicy policy) {
  switch (policy) {
    case NegotiationPolicy::HighestCommon: return "highest_common";
    case NegotiationPolicy::LowestCommon: return "lowest_common";
    case NegotiationPolicy::ExactRequired: return "exact_required";
  }
  return "highest_common";
}

Result<NegotiationPolicy> ParseNegotiationPolicy(std::string_view text) {
  if (text == "highest_common") return NegotiationPolicy::HighestCommon;
  if (text == "lowest_common") return NegotiationPolicy::LowestCommon;
  if (text == "exact_required") return NegotiationPolicy::ExactRequired;
  return MakeError(ErrorCode::InvalidDocument, "unknown negotiation policy", std::string(text));
}

std::string_view LifecycleStateName(LifecycleState state) {
  switch (state) {
    case LifecycleState::Active: return "active";
    case LifecycleState::Deprecated: return "deprecated";
    case LifecycleState::Retired: return "retired";
  }
  return "active";
}

Result<LifecycleState> ParseLifecycleState(std::string_view text) {
  if (text == "active") return LifecycleState::Active;
  if (text == "deprecated") return LifecycleState::Deprecated;
  if (text == "retired") return LifecycleState::Retired;
  return MakeError(ErrorCode::InvalidDocument, "unknown lifecycle state", std::string(text));
}

// ---------------------------------------------------------------------------
// ConstraintLiteral
// ---------------------------------------------------------------------------
ConstraintLiteral ConstraintLiteral::Boolean(bool v) {
  ConstraintLiteral out;
  out.kind_ = Kind::Boolean;
  out.boolean_ = v;
  return out;
}

ConstraintLiteral ConstraintLiteral::Integer(std::int64_t v) {
  ConstraintLiteral out;
  out.kind_ = Kind::Integer;
  out.integer_ = v;
  return out;
}

ConstraintLiteral ConstraintLiteral::Text(std::string v) {
  ConstraintLiteral out;
  out.kind_ = Kind::Text;
  out.text_ = std::move(v);
  return out;
}

ConstraintLiteral ConstraintLiteral::Version(SemVersion v) {
  ConstraintLiteral out;
  out.kind_ = Kind::Version;
  out.version_ = std::move(v);
  return out;
}

bool ConstraintLiteral::MatchesType(CapabilityType type) const {
  switch (type) {
    case CapabilityType::Flag: return kind_ == Kind::Boolean;
    case CapabilityType::Integer: return kind_ == Kind::Integer;
    case CapabilityType::Text: return kind_ == Kind::Text;
    case CapabilityType::Version: return kind_ == Kind::Version;
  }
  return false;
}

Result<CapabilityValue> ConstraintLiteral::Resolve() const {
  switch (kind_) {
    case Kind::Boolean: return CapabilityValue::Flag(boolean_);
    case Kind::Integer: return CapabilityValue::Integer(integer_);
    case Kind::Text: return CapabilityValue::Text(text_);
    case Kind::Version: return CapabilityValue::Version(version_);
  }
  return MakeError(ErrorCode::InvalidConstraint, "unresolvable constraint literal");
}

std::string ConstraintLiteral::ToString() const {
  switch (kind_) {
    case Kind::Boolean: return boolean_ ? "true" : "false";
    case Kind::Integer: return std::to_string(integer_);
    case Kind::Text: return text_;
    case Kind::Version: return version_.ToString();
  }
  return {};
}

JsonValue ConstraintLiteral::ToJson() const {
  switch (kind_) {
    case Kind::Boolean: return JsonValue::Bool(boolean_);
    case Kind::Integer: return JsonValue::Int(integer_);
    case Kind::Text: return JsonValue::Str(text_);
    case Kind::Version: return JsonValue::Str(version_.ToString());
  }
  return JsonValue::Null();
}

Result<ConstraintLiteral> ConstraintLiteral::FromJson(const JsonValue& json) {
  if (json.IsBool()) return Boolean(json.AsBool(false));
  if (json.IsInt() || json.IsUInt()) {
    auto value = json.AsInt();
    if (!value.has_value()) {
      return MakeError(ErrorCode::InvalidConstraint, "constraint integer literal out of range");
    }
    return Integer(value.value());
  }
  const std::string* text = json.AsString();
  if (text == nullptr) {
    return MakeError(ErrorCode::InvalidConstraint,
                     "constraint literal must be a boolean, integer, or string");
  }
  auto version = SemVersion::Parse(*text);
  if (version.has_value()) return Version(version.value());
  if (text->size() > kMaxCapabilityTextBytes) {
    return MakeError(ErrorCode::LimitExceeded, "constraint text literal exceeds size limit");
  }
  return Text(*text);
}

bool operator==(const ConstraintLiteral& a, const ConstraintLiteral& b) {
  if (a.kind_ != b.kind_) return false;
  switch (a.kind_) {
    case ConstraintLiteral::Kind::Boolean: return a.boolean_ == b.boolean_;
    case ConstraintLiteral::Kind::Integer: return a.integer_ == b.integer_;
    case ConstraintLiteral::Kind::Text: return a.text_ == b.text_;
    case ConstraintLiteral::Kind::Version:
      return a.version_.Precedence(b.version_) == std::strong_ordering::equal;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------
std::string_view ConstraintKindName(const Constraint& constraint) {
  switch (constraint.index()) {
    case 0: return "requires_capability";
    case 1: return "requires_capability_value";
    case 2: return "forbids_capability";
    case 3: return "requires_protocol";
    case 4: return "forbids_protocol";
    case 5: return "requires_schema";
    case 6: return "forbids_schema";
    case 7: return "requires_hardware_class";
    case 8: return "forbids_hardware_class";
    case 9: return "version_at_least";
    case 10: return "version_at_most";
    case 11: return "requires_feature";
    case 12: return "forbids_feature";
    default: return "unknown";
  }
}

Side ConstraintSide(const Constraint& constraint) {
  return std::visit([](const auto& c) { return c.side; }, constraint);
}

const CapabilityId* ConstraintCapability(const Constraint& constraint) {
  if (const auto* c = std::get_if<RequiresCapability>(&constraint)) return &c->capability;
  if (const auto* c = std::get_if<RequiresCapabilityValue>(&constraint)) return &c->capability;
  if (const auto* c = std::get_if<ForbidsCapability>(&constraint)) return &c->capability;
  return nullptr;
}

JsonValue ConstraintToJson(const Constraint& constraint) {
  JsonValue out = JsonValue::Obj();
  out.Set("type", JsonValue::Str(std::string(ConstraintKindName(constraint))));
  std::visit(
      [&out](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        out.Set("side", JsonValue::Str(std::string(SideName(c.side))));
        if constexpr (std::is_same_v<T, RequiresCapability> || std::is_same_v<T, ForbidsCapability>) {
          out.Set("capability", JsonValue::Str(c.capability.value()));
        } else if constexpr (std::is_same_v<T, RequiresCapabilityValue>) {
          out.Set("capability", JsonValue::Str(c.capability.value()));
          out.Set("op", JsonValue::Str(std::string(ComparisonOpName(c.op))));
          out.Set("value", c.value.ToJson());
        } else if constexpr (std::is_same_v<T, RequiresProtocol>) {
          out.Set("protocol", JsonValue::Str(c.protocol.value()));
          out.Set("range", JsonValue::Str(c.range.ToString()));
        } else if constexpr (std::is_same_v<T, ForbidsProtocol>) {
          out.Set("protocol", JsonValue::Str(c.protocol.value()));
        } else if constexpr (std::is_same_v<T, RequiresSchema>) {
          out.Set("schema", JsonValue::Str(c.schema.value()));
          out.Set("range", JsonValue::Str(c.range.ToString()));
        } else if constexpr (std::is_same_v<T, ForbidsSchema>) {
          out.Set("schema", JsonValue::Str(c.schema.value()));
        } else if constexpr (std::is_same_v<T, RequiresHardwareClass> ||
                             std::is_same_v<T, ForbidsHardwareClass>) {
          out.Set("hardware_class", JsonValue::Str(c.hardware_class.value()));
        } else if constexpr (std::is_same_v<T, VersionAtLeast> || std::is_same_v<T, VersionAtMost>) {
          out.Set("version", JsonValue::Str(c.version.ToString()));
        } else if constexpr (std::is_same_v<T, RequiresFeature> || std::is_same_v<T, ForbidsFeature>) {
          out.Set("feature", JsonValue::Str(c.feature.value()));
        }
      },
      constraint);
  return out;
}

Result<Constraint> ParseConstraint(const JsonValue& json) {
  auto type = RequireString(json, "type", "constraint");
  if (!type.has_value()) return type.error();
  auto side_text = RequireString(json, "side", "constraint");
  if (!side_text.has_value()) return side_text.error();
  auto side = ParseSide(side_text.value());
  if (!side.has_value()) return side.error();

  const auto read_id = [&json](std::string_view key, auto parse) {
    using ResultType = decltype(parse(std::string_view{}));
    auto text = RequireString(json, key, "constraint");
    if (!text.has_value()) return ResultType(text.error());
    return parse(text.value());
  };

  if (type.value() == "requires_capability" || type.value() == "forbids_capability") {
    auto id = read_id("capability", [](std::string_view t) { return CapabilityId::Parse(t); });
    if (!id.has_value()) return id.error();
    if (type.value() == "requires_capability") {
      return Constraint(RequiresCapability{side.value(), id.value()});
    }
    return Constraint(ForbidsCapability{side.value(), id.value()});
  }
  if (type.value() == "requires_capability_value") {
    auto id = read_id("capability", [](std::string_view t) { return CapabilityId::Parse(t); });
    if (!id.has_value()) return id.error();
    auto op_text = RequireString(json, "op", "constraint");
    if (!op_text.has_value()) return op_text.error();
    auto op = ParseComparisonOp(op_text.value());
    if (!op.has_value()) return op.error();
    auto value_json = RequireField(json, "value", "constraint");
    if (!value_json.has_value()) return value_json.error();
    auto literal = ConstraintLiteral::FromJson(*value_json.value());
    if (!literal.has_value()) return literal.error();
    return Constraint(RequiresCapabilityValue{side.value(), id.value(), op.value(),
                                              literal.value()});
  }
  if (type.value() == "requires_protocol" || type.value() == "forbids_protocol") {
    auto id = read_id("protocol", [](std::string_view t) { return ProtocolId::Parse(t); });
    if (!id.has_value()) return id.error();
    if (type.value() == "forbids_protocol") {
      return Constraint(ForbidsProtocol{side.value(), id.value()});
    }
    auto range_text = OptionalString(json, "range", "*");
    if (!range_text.has_value()) return range_text.error();
    auto range = VersionRange::Parse(range_text.value());
    if (!range.has_value()) return range.error();
    return Constraint(RequiresProtocol{side.value(), id.value(), range.value()});
  }
  if (type.value() == "requires_schema" || type.value() == "forbids_schema") {
    auto id = read_id("schema", [](std::string_view t) { return SchemaId::Parse(t); });
    if (!id.has_value()) return id.error();
    if (type.value() == "forbids_schema") {
      return Constraint(ForbidsSchema{side.value(), id.value()});
    }
    auto range_text = OptionalString(json, "range", "*");
    if (!range_text.has_value()) return range_text.error();
    auto range = VersionRange::Parse(range_text.value());
    if (!range.has_value()) return range.error();
    return Constraint(RequiresSchema{side.value(), id.value(), range.value()});
  }
  if (type.value() == "requires_hardware_class" || type.value() == "forbids_hardware_class") {
    auto id = read_id("hardware_class", [](std::string_view t) { return HardwareClassId::Parse(t); });
    if (!id.has_value()) return id.error();
    if (type.value() == "requires_hardware_class") {
      return Constraint(RequiresHardwareClass{side.value(), id.value()});
    }
    return Constraint(ForbidsHardwareClass{side.value(), id.value()});
  }
  if (type.value() == "requires_feature" || type.value() == "forbids_feature") {
    auto id = read_id("feature", [](std::string_view t) { return FeatureId::Parse(t); });
    if (!id.has_value()) return id.error();
    if (type.value() == "requires_feature") {
      return Constraint(RequiresFeature{side.value(), id.value()});
    }
    return Constraint(ForbidsFeature{side.value(), id.value()});
  }
  if (type.value() == "version_at_least" || type.value() == "version_at_most") {
    auto version_text = RequireString(json, "version", "constraint");
    if (!version_text.has_value()) return version_text.error();
    auto version = SemVersion::Parse(version_text.value());
    if (!version.has_value()) return version.error();
    if (type.value() == "version_at_least") {
      return Constraint(VersionAtLeast{side.value(), version.value()});
    }
    return Constraint(VersionAtMost{side.value(), version.value()});
  }
  return MakeError(ErrorCode::InvalidConstraint, "unknown constraint type", type.value());
}

std::string ConstraintSortKey(const Constraint& constraint) {
  std::string key = PadIndex(constraint.index());
  key.push_back('|');
  key.append(PadIndex(SideIndex(ConstraintSide(constraint))));
  key.push_back('|');
  std::visit(
      [&key](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, RequiresCapability> || std::is_same_v<T, ForbidsCapability>) {
          key.append(c.capability.value());
        } else if constexpr (std::is_same_v<T, RequiresCapabilityValue>) {
          key.append(c.capability.value());
          key.push_back('|');
          key.append(PadIndex(static_cast<std::size_t>(c.op)));
          key.push_back('|');
          key.append(c.value.ToString());
        } else if constexpr (std::is_same_v<T, RequiresProtocol>) {
          key.append(c.protocol.value());
          key.push_back('|');
          key.append(c.range.ToString());
        } else if constexpr (std::is_same_v<T, ForbidsProtocol>) {
          key.append(c.protocol.value());
        } else if constexpr (std::is_same_v<T, RequiresSchema>) {
          key.append(c.schema.value());
          key.push_back('|');
          key.append(c.range.ToString());
        } else if constexpr (std::is_same_v<T, ForbidsSchema>) {
          key.append(c.schema.value());
        } else if constexpr (std::is_same_v<T, RequiresHardwareClass> ||
                             std::is_same_v<T, ForbidsHardwareClass>) {
          key.append(c.hardware_class.value());
        } else if constexpr (std::is_same_v<T, VersionAtLeast> || std::is_same_v<T, VersionAtMost>) {
          key.append(c.version.ToString());
        } else if constexpr (std::is_same_v<T, RequiresFeature> || std::is_same_v<T, ForbidsFeature>) {
          key.append(c.feature.value());
        }
      },
      constraint);
  return key;
}

bool ConstraintLess(const Constraint& a, const Constraint& b) {
  return ConstraintSortKey(a) < ConstraintSortKey(b);
}

bool ConstraintEquals(const Constraint& a, const Constraint& b) {
  return ConstraintSortKey(a) == ConstraintSortKey(b);
}

bool ConstraintListsEqual(const std::vector<Constraint>& a, const std::vector<Constraint>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!ConstraintEquals(a[i], b[i])) return false;
  }
  return true;
}

bool operator<(const CapabilityImplication& a, const CapabilityImplication& b) {
  if (a.side != b.side) return SideIndex(a.side) < SideIndex(b.side);
  if (a.if_present != b.if_present) return a.if_present < b.if_present;
  return a.then_required < b.then_required;
}

// ---------------------------------------------------------------------------
// Precedence
// ---------------------------------------------------------------------------
int ComputeSpecificity(const Rule& rule) {
  int score = 0;
  const ComponentSelector* selectors[2] = {&rule.left, &rule.right};
  const std::size_t selector_count = rule.arity == RuleArity::Pair ? 2u : 1u;
  for (std::size_t i = 0; i < selector_count; ++i) {
    const ComponentSelector& selector = *selectors[i];
    if (selector.family.has_value()) score += 8;
    if (selector.kind.has_value()) score += 8;
    if (selector.hardware_class.has_value()) score += 8;
    if (selector.version.has_value()) {
      if (selector.version->IsExact()) {
        score += 24;
      } else if (!selector.version->IsAny()) {
        score += 12;
      }
    }
  }
  score += static_cast<int>(std::min<std::size_t>(rule.constraints.size(), 16)) * 2;
  score += static_cast<int>(std::min<std::size_t>(rule.implications.size(), 16));
  return score;
}

PrecedenceTier RuleTier(const Rule& rule) {
  PrecedenceTier tier;
  tier.priority = rule.priority;
  tier.specificity = ComputeSpecificity(rule);
  tier.origin_rank = RuleOriginRank(rule.origin);
  return tier;
}

PrecedenceKey RulePrecedence(const Rule& rule) {
  PrecedenceKey key;
  key.tier = RuleTier(rule);
  key.id = rule.id;
  key.revision = rule.revision;
  return key;
}

bool PrecedenceHigher(const PrecedenceKey& a, const PrecedenceKey& b) {
  if (!(a.tier == b.tier)) return b.tier < a.tier;
  if (a.id != b.id) return a.id < b.id;
  return a.revision < b.revision;
}

std::string PrecedenceKeyToString(const PrecedenceKey& key) {
  return "priority=" + std::to_string(key.tier.priority.value()) +
         " specificity=" + std::to_string(key.tier.specificity) +
         " origin=" + std::to_string(key.tier.origin_rank) + " rule=" + key.id.value() + "@r" +
         std::to_string(key.revision.value());
}

// ---------------------------------------------------------------------------
// Normalization, serialization, parsing
// ---------------------------------------------------------------------------
void NormalizeRule(Rule& rule) {
  std::stable_sort(rule.constraints.begin(), rule.constraints.end(), ConstraintLess);
  std::sort(rule.implications.begin(), rule.implications.end());
}

JsonValue RuleToJson(const Rule& rule) {
  JsonValue out = JsonValue::Obj();
  out.Set("id", JsonValue::Str(rule.id.value()));
  out.Set("revision", JsonValue::UInt(rule.revision.value()));
  out.Set("priority", JsonValue::Int(rule.priority.value()));
  out.Set("origin", JsonValue::Str(std::string(RuleOriginName(rule.origin))));
  out.Set("symmetry", JsonValue::Str(std::string(RuleSymmetryName(rule.symmetry))));
  out.Set("arity", JsonValue::Str(std::string(RuleArityName(rule.arity))));
  out.Set("outcome", JsonValue::Str(std::string(RuleOutcomeName(rule.outcome))));
  out.Set("left", ComponentSelectorToJson(rule.left));
  if (rule.arity == RuleArity::Pair) {
    out.Set("right", ComponentSelectorToJson(rule.right));
  }
  {
    JsonValue::Array constraints;
    for (const Constraint& constraint : rule.constraints) {
      constraints.push_back(ConstraintToJson(constraint));
    }
    out.Set("constraints", JsonValue::Arr(std::move(constraints)));
  }
  if (!rule.implications.empty()) {
    JsonValue::Array implications;
    for (const CapabilityImplication& implication : rule.implications) {
      JsonValue entry = JsonValue::Obj();
      entry.Set("side", JsonValue::Str(std::string(SideName(implication.side))));
      entry.Set("if_present", JsonValue::Str(implication.if_present.value()));
      entry.Set("then_required", JsonValue::Str(implication.then_required.value()));
      implications.push_back(std::move(entry));
    }
    out.Set("implications", JsonValue::Arr(std::move(implications)));
  }
  out.Set("negotiation", JsonValue::Str(std::string(NegotiationPolicyName(rule.negotiation))));
  out.Set("on_unmet_requirement",
          JsonValue::Str(std::string(OnUnmetRequirementName(rule.on_unmet))));
  out.Set("on_indeterminate", JsonValue::Str(std::string(OnIndeterminateName(rule.on_indeterminate))));
  {
    JsonValue lifecycle = JsonValue::Obj();
    lifecycle.Set("state", JsonValue::Str(std::string(LifecycleStateName(rule.lifecycle.state))));
    if (rule.lifecycle.superseded_by.has_value()) {
      JsonValue reference = JsonValue::Obj();
      reference.Set("id", JsonValue::Str(rule.lifecycle.superseded_by->id.value()));
      reference.Set("revision", JsonValue::UInt(rule.lifecycle.superseded_by->revision.value()));
      lifecycle.Set("superseded_by", std::move(reference));
    }
    if (!rule.lifecycle.note.empty()) {
      lifecycle.Set("note", JsonValue::Str(rule.lifecycle.note));
    }
    out.Set("lifecycle", std::move(lifecycle));
  }
  {
    JsonValue provenance = JsonValue::Obj();
    provenance.Set("publisher", JsonValue::Str(rule.provenance.publisher.value()));
    if (!rule.provenance.source.empty()) {
      provenance.Set("source", JsonValue::Str(rule.provenance.source));
    }
    if (!rule.provenance.reference.empty()) {
      provenance.Set("reference", JsonValue::Str(rule.provenance.reference));
    }
    provenance.Set("recorded_at", JsonValue::Str(rule.provenance.recorded_at.ToIso8601()));
    if (!rule.provenance.change_note.empty()) {
      provenance.Set("change_note", JsonValue::Str(rule.provenance.change_note));
    }
    out.Set("provenance", std::move(provenance));
  }
  if (!rule.rationale.empty()) out.Set("rationale", JsonValue::Str(rule.rationale));
  return out;
}

Result<Rule> ParseRule(const JsonValue& json, std::string_view context) {
  if (!json.IsObject()) {
    return MakeError(ErrorCode::InvalidDocument, std::string(context) + " must be a JSON object");
  }
  Rule rule;
  auto id_text = RequireString(json, "id", context);
  if (!id_text.has_value()) return id_text.error();
  auto id = RuleId::Parse(id_text.value());
  if (!id.has_value()) return id.error();
  rule.id = id.value();

  auto revision = json.Find("revision");
  if (revision != nullptr && !revision->IsNull()) {
    auto value = revision->AsUInt();
    if (!value.has_value() || value.value() > UINT32_MAX) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + ".revision must be a 32-bit unsigned integer");
    }
    rule.revision = RuleRevision(static_cast<std::uint32_t>(value.value()));
  } else {
    rule.revision = RuleRevision(1);
  }

  auto priority = json.Find("priority");
  if (priority != nullptr && !priority->IsNull()) {
    auto value = priority->AsInt();
    if (!value.has_value() || value.value() < INT32_MIN || value.value() > INT32_MAX) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + ".priority must be a 32-bit signed integer");
    }
    rule.priority = RulePriority(static_cast<std::int32_t>(value.value()));
  }

  auto origin_text = OptionalString(json, "origin", "standard");
  if (!origin_text.has_value()) return origin_text.error();
  auto origin = ParseRuleOrigin(origin_text.value());
  if (!origin.has_value()) return origin.error();
  rule.origin = origin.value();

  auto symmetry_text = OptionalString(json, "symmetry", "symmetric");
  if (!symmetry_text.has_value()) return symmetry_text.error();
  auto symmetry = ParseRuleSymmetry(symmetry_text.value());
  if (!symmetry.has_value()) return symmetry.error();
  rule.symmetry = symmetry.value();

  auto arity_text = OptionalString(json, "arity", "pair");
  if (!arity_text.has_value()) return arity_text.error();
  auto arity = ParseRuleArity(arity_text.value());
  if (!arity.has_value()) return arity.error();
  rule.arity = arity.value();

  auto outcome_text = RequireString(json, "outcome", context);
  if (!outcome_text.has_value()) return outcome_text.error();
  auto outcome = ParseRuleOutcome(outcome_text.value());
  if (!outcome.has_value()) return outcome.error();
  rule.outcome = outcome.value();

  auto left = RequireField(json, "left", context);
  if (!left.has_value()) return left.error();
  auto left_selector = ParseComponentSelector(*left.value());
  if (!left_selector.has_value()) return left_selector.error();
  rule.left = left_selector.value();

  const JsonValue* right = json.Find("right");
  if (rule.arity == RuleArity::Pair) {
    if (right == nullptr || right->IsNull()) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + " pair rule requires a 'right' selector");
    }
    auto right_selector = ParseComponentSelector(*right);
    if (!right_selector.has_value()) return right_selector.error();
    rule.right = right_selector.value();
  } else if (right != nullptr && !right->IsNull()) {
    return MakeError(ErrorCode::InvalidDocument,
                     std::string(context) + " set rule must not declare a 'right' selector");
  }

  const JsonValue* constraints = json.Find("constraints");
  if (constraints != nullptr && !constraints->IsNull()) {
    const JsonValue::Array* array = constraints->AsArray();
    if (array == nullptr) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + ".constraints must be an array");
    }
    if (array->size() > kMaxConstraintsPerRule) {
      return MakeError(ErrorCode::LimitExceeded,
                       std::string(context) + " declares too many constraints");
    }
    for (const JsonValue& entry : *array) {
      auto constraint = ParseConstraint(entry);
      if (!constraint.has_value()) return constraint.error();
      rule.constraints.push_back(std::move(constraint).value());
    }
  }

  const JsonValue* implications = json.Find("implications");
  if (implications != nullptr && !implications->IsNull()) {
    const JsonValue::Array* array = implications->AsArray();
    if (array == nullptr) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + ".implications must be an array");
    }
    if (array->size() > kMaxImplicationsPerRule) {
      return MakeError(ErrorCode::LimitExceeded,
                       std::string(context) + " declares too many implications");
    }
    for (const JsonValue& entry : *array) {
      auto side_text = RequireString(entry, "side", "implication");
      if (!side_text.has_value()) return side_text.error();
      auto side = ParseSide(side_text.value());
      if (!side.has_value()) return side.error();
      auto if_text = RequireString(entry, "if_present", "implication");
      if (!if_text.has_value()) return if_text.error();
      auto if_id = CapabilityId::Parse(if_text.value());
      if (!if_id.has_value()) return if_id.error();
      auto then_text = RequireString(entry, "then_required", "implication");
      if (!then_text.has_value()) return then_text.error();
      auto then_id = CapabilityId::Parse(then_text.value());
      if (!then_id.has_value()) return then_id.error();
      rule.implications.push_back(
          CapabilityImplication{side.value(), if_id.value(), then_id.value()});
    }
  }

  auto negotiation_text = OptionalString(json, "negotiation", "highest_common");
  if (!negotiation_text.has_value()) return negotiation_text.error();
  auto negotiation = ParseNegotiationPolicy(negotiation_text.value());
  if (!negotiation.has_value()) return negotiation.error();
  rule.negotiation = negotiation.value();

  auto on_unmet_text = OptionalString(json, "on_unmet_requirement", "incompatible");
  if (!on_unmet_text.has_value()) return on_unmet_text.error();
  auto on_unmet = ParseOnUnmetRequirement(on_unmet_text.value());
  if (!on_unmet.has_value()) return on_unmet.error();
  rule.on_unmet = on_unmet.value();

  auto on_indeterminate_text = OptionalString(json, "on_indeterminate", "unknown");
  if (!on_indeterminate_text.has_value()) return on_indeterminate_text.error();
  auto on_indeterminate = ParseOnIndeterminate(on_indeterminate_text.value());
  if (!on_indeterminate.has_value()) return on_indeterminate.error();
  rule.on_indeterminate = on_indeterminate.value();

  const JsonValue* lifecycle = json.Find("lifecycle");
  if (lifecycle != nullptr && !lifecycle->IsNull()) {
    auto state_text = OptionalString(*lifecycle, "state", "active");
    if (!state_text.has_value()) return state_text.error();
    auto state = ParseLifecycleState(state_text.value());
    if (!state.has_value()) return state.error();
    rule.lifecycle.state = state.value();
    const JsonValue* superseded = lifecycle->Find("superseded_by");
    if (superseded != nullptr && !superseded->IsNull()) {
      auto reference_id_text = RequireString(*superseded, "id", "superseded_by");
      if (!reference_id_text.has_value()) return reference_id_text.error();
      auto reference_id = RuleId::Parse(reference_id_text.value());
      if (!reference_id.has_value()) return reference_id.error();
      RuleRef reference;
      reference.id = reference_id.value();
      const JsonValue* reference_revision = superseded->Find("revision");
      if (reference_revision != nullptr && !reference_revision->IsNull()) {
        auto value = reference_revision->AsUInt();
        if (!value.has_value() || value.value() > UINT32_MAX) {
          return MakeError(ErrorCode::InvalidDocument,
                           "superseded_by.revision must be a 32-bit unsigned integer");
        }
        reference.revision = RuleRevision(static_cast<std::uint32_t>(value.value()));
      } else {
        reference.revision = RuleRevision(1);
      }
      rule.lifecycle.superseded_by = reference;
    }
    auto note = OptionalString(*lifecycle, "note", "");
    if (!note.has_value()) return note.error();
    rule.lifecycle.note = note.value();
  }

  auto provenance = RequireField(json, "provenance", context);
  if (!provenance.has_value()) return provenance.error();
  auto publisher_text = RequireString(*provenance.value(), "publisher", "provenance");
  if (!publisher_text.has_value()) return publisher_text.error();
  auto publisher = PublisherId::Parse(publisher_text.value());
  if (!publisher.has_value()) return publisher.error();
  rule.provenance.publisher = publisher.value();
  auto source = OptionalString(*provenance.value(), "source", "");
  if (!source.has_value()) return source.error();
  rule.provenance.source = source.value();
  auto reference = OptionalString(*provenance.value(), "reference", "");
  if (!reference.has_value()) return reference.error();
  rule.provenance.reference = reference.value();
  auto change_note = OptionalString(*provenance.value(), "change_note", "");
  if (!change_note.has_value()) return change_note.error();
  rule.provenance.change_note = change_note.value();
  auto recorded_at = OptionalString(*provenance.value(), "recorded_at", "");
  if (!recorded_at.has_value()) return recorded_at.error();
  if (!recorded_at.value().empty()) {
    auto timestamp = Timestamp::FromIso8601(recorded_at.value());
    if (!timestamp.has_value()) return timestamp.error();
    rule.provenance.recorded_at = timestamp.value();
  }

  auto rationale = OptionalString(json, "rationale", "");
  if (!rationale.has_value()) return rationale.error();
  if (rationale.value().size() > kMaxRationaleBytes) {
    return MakeError(ErrorCode::LimitExceeded,
                     std::string(context) + ".rationale exceeds the size limit");
  }
  rule.rationale = rationale.value();

  NormalizeRule(rule);
  return rule;
}

ContentDigest RuleContentDigest(const Rule& rule) {
  return Sha256::Hash(RuleToJson(rule).Dump(false));
}

bool RuleMatchesSelectors(const Rule& rule, const ComponentSpec& left, const ComponentSpec& right,
                          bool* mirrored) {
  if (mirrored != nullptr) *mirrored = false;
  const bool forward =
      SelectorMatches(rule.left, left) && SelectorMatches(rule.right, right);
  if (rule.symmetry == RuleSymmetry::LeftToRight) return forward;
  const bool reverse = SelectorMatches(rule.left, right) && SelectorMatches(rule.right, left);
  if (rule.symmetry == RuleSymmetry::RightToLeft) {
    if (reverse && mirrored != nullptr) *mirrored = true;
    return reverse;
  }
  if (forward) return true;
  if (reverse && mirrored != nullptr) *mirrored = true;
  return reverse;
}

}  // namespace fcr
