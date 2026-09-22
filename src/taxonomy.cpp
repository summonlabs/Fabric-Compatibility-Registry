#include "fcr/taxonomy.hpp"

#include <algorithm>

namespace fcr {
namespace {

Error DuplicateError(std::string_view label, const std::string& id) {
  return MakeError(ErrorCode::DuplicateIdentity,
                   "duplicate " + std::string(label) + " '" + id + "' in taxonomy");
}

}  // namespace

std::string_view CapabilityTypeName(CapabilityType type) {
  switch (type) {
    case CapabilityType::Flag: return "flag";
    case CapabilityType::Integer: return "integer";
    case CapabilityType::Text: return "text";
    case CapabilityType::Version: return "version";
  }
  return "flag";
}

Result<CapabilityType> ParseCapabilityType(std::string_view text) {
  if (text == "flag") return CapabilityType::Flag;
  if (text == "integer") return CapabilityType::Integer;
  if (text == "text") return CapabilityType::Text;
  if (text == "version") return CapabilityType::Version;
  return MakeError(ErrorCode::InvalidCapabilityValue, "unknown capability type",
                   std::string(text));
}

CapabilityValue CapabilityValue::Flag(bool v) {
  CapabilityValue out;
  out.type_ = CapabilityType::Flag;
  out.flag_ = v;
  return out;
}

CapabilityValue CapabilityValue::Integer(std::int64_t v) {
  CapabilityValue out;
  out.type_ = CapabilityType::Integer;
  out.integer_ = v;
  return out;
}

CapabilityValue CapabilityValue::Text(std::string v) {
  CapabilityValue out;
  out.type_ = CapabilityType::Text;
  out.text_ = std::move(v);
  return out;
}

CapabilityValue CapabilityValue::Version(SemVersion v) {
  CapabilityValue out;
  out.type_ = CapabilityType::Version;
  out.version_ = std::move(v);
  return out;
}

bool CapabilityValue::AsFlag(bool fallback) const noexcept {
  return type_ == CapabilityType::Flag ? flag_ : fallback;
}

std::int64_t CapabilityValue::AsInteger(std::int64_t fallback) const noexcept {
  return type_ == CapabilityType::Integer ? integer_ : fallback;
}

std::string CapabilityValue::ToString() const {
  switch (type_) {
    case CapabilityType::Flag: return flag_ ? "true" : "false";
    case CapabilityType::Integer: return std::to_string(integer_);
    case CapabilityType::Text: return text_;
    case CapabilityType::Version: return version_.ToString();
  }
  return {};
}

JsonValue CapabilityValue::ToJson() const {
  switch (type_) {
    case CapabilityType::Flag: return JsonValue::Bool(flag_);
    case CapabilityType::Integer: return JsonValue::Int(integer_);
    case CapabilityType::Text: return JsonValue::Str(text_);
    case CapabilityType::Version: return JsonValue::Str(version_.ToString());
  }
  return JsonValue::Null();
}

Result<CapabilityValue> CapabilityValue::FromJson(CapabilityType expected, const JsonValue& json) {
  switch (expected) {
    case CapabilityType::Flag:
      if (!json.IsBool()) {
        return MakeError(ErrorCode::InvalidCapabilityValue, "flag capability requires a boolean");
      }
      return Flag(json.AsBool(false));
    case CapabilityType::Integer: {
      auto value = json.AsInt();
      if (!value.has_value()) {
        return MakeError(ErrorCode::InvalidCapabilityValue,
                         "integer capability requires a 64-bit integer");
      }
      return Integer(value.value());
    }
    case CapabilityType::Text: {
      const std::string* text = json.AsString();
      if (text == nullptr) {
        return MakeError(ErrorCode::InvalidCapabilityValue, "text capability requires a string");
      }
      if (text->size() > kMaxCapabilityTextBytes) {
        return MakeError(ErrorCode::LimitExceeded, "text capability exceeds size limit");
      }
      return Text(*text);
    }
    case CapabilityType::Version: {
      const std::string* text = json.AsString();
      if (text == nullptr) {
        return MakeError(ErrorCode::InvalidCapabilityValue, "version capability requires a string");
      }
      auto parsed = SemVersion::Parse(*text);
      if (!parsed.has_value()) return parsed.error();
      return Version(parsed.value());
    }
  }
  return MakeError(ErrorCode::InvalidCapabilityValue, "unknown capability type");
}

bool operator==(const CapabilityValue& a, const CapabilityValue& b) {
  if (a.type_ != b.type_) return false;
  switch (a.type_) {
    case CapabilityType::Flag: return a.flag_ == b.flag_;
    case CapabilityType::Integer: return a.integer_ == b.integer_;
    case CapabilityType::Text: return a.text_ == b.text_;
    case CapabilityType::Version:
      return a.version_.Precedence(b.version_) == std::strong_ordering::equal;
  }
  return false;
}

std::string_view ComparisonOpName(ComparisonOp op) {
  switch (op) {
    case ComparisonOp::Eq: return "eq";
    case ComparisonOp::Ne: return "ne";
    case ComparisonOp::Lt: return "lt";
    case ComparisonOp::Le: return "le";
    case ComparisonOp::Gt: return "gt";
    case ComparisonOp::Ge: return "ge";
  }
  return "eq";
}

Result<ComparisonOp> ParseComparisonOp(std::string_view text) {
  if (text == "eq" || text == "==" || text == "=") return ComparisonOp::Eq;
  if (text == "ne" || text == "!=") return ComparisonOp::Ne;
  if (text == "lt" || text == "<") return ComparisonOp::Lt;
  if (text == "le" || text == "<=") return ComparisonOp::Le;
  if (text == "gt" || text == ">") return ComparisonOp::Gt;
  if (text == "ge" || text == ">=") return ComparisonOp::Ge;
  return MakeError(ErrorCode::InvalidConstraint, "unknown comparison operator", std::string(text));
}

bool ComparisonOpSupported(CapabilityType type, ComparisonOp op) {
  switch (type) {
    case CapabilityType::Flag: return op == ComparisonOp::Eq || op == ComparisonOp::Ne;
    case CapabilityType::Integer:
    case CapabilityType::Version:
    case CapabilityType::Text: return true;
  }
  return false;
}

bool ApplyComparison(ComparisonOp op, const CapabilityValue& lhs, const CapabilityValue& rhs) {
  if (lhs.type() != rhs.type()) return false;
  switch (lhs.type()) {
    case CapabilityType::Flag: {
      const bool a = lhs.AsFlag(false);
      const bool b = rhs.AsFlag(false);
      if (op == ComparisonOp::Eq) return a == b;
      if (op == ComparisonOp::Ne) return a != b;
      return false;
    }
    case CapabilityType::Integer: {
      const std::int64_t a = lhs.AsInteger(0);
      const std::int64_t b = rhs.AsInteger(0);
      switch (op) {
        case ComparisonOp::Eq: return a == b;
        case ComparisonOp::Ne: return a != b;
        case ComparisonOp::Lt: return a < b;
        case ComparisonOp::Le: return a <= b;
        case ComparisonOp::Gt: return a > b;
        case ComparisonOp::Ge: return a >= b;
      }
      return false;
    }
    case CapabilityType::Text: {
      const std::string& a = lhs.AsText();
      const std::string& b = rhs.AsText();
      switch (op) {
        case ComparisonOp::Eq: return a == b;
        case ComparisonOp::Ne: return a != b;
        case ComparisonOp::Lt: return a < b;
        case ComparisonOp::Le: return a <= b;
        case ComparisonOp::Gt: return a > b;
        case ComparisonOp::Ge: return a >= b;
      }
      return false;
    }
    case CapabilityType::Version: {
      const std::strong_ordering cmp = lhs.AsVersion().Precedence(rhs.AsVersion());
      switch (op) {
        case ComparisonOp::Eq: return cmp == std::strong_ordering::equal;
        case ComparisonOp::Ne: return cmp != std::strong_ordering::equal;
        case ComparisonOp::Lt: return cmp == std::strong_ordering::less;
        case ComparisonOp::Le: return cmp != std::strong_ordering::greater;
        case ComparisonOp::Gt: return cmp == std::strong_ordering::greater;
        case ComparisonOp::Ge: return cmp != std::strong_ordering::less;
      }
      return false;
    }
  }
  return false;
}

std::string_view KnowledgeClosureName(KnowledgeClosure closure) {
  return closure == KnowledgeClosure::Closed ? "closed" : "open";
}

Result<KnowledgeClosure> ParseKnowledgeClosure(std::string_view text) {
  if (text == "closed") return KnowledgeClosure::Closed;
  if (text == "open") return KnowledgeClosure::Open;
  return MakeError(ErrorCode::InvalidDocument, "closure must be 'open' or 'closed'",
                   std::string(text));
}

Status CapabilitySet::Set(const CapabilityId& id, CapabilityValue value) {
  items_[id] = std::move(value);
  return Status{};
}

bool CapabilitySet::Contains(const CapabilityId& id) const { return items_.count(id) != 0; }

const CapabilityValue* CapabilitySet::Get(const CapabilityId& id) const {
  const auto it = items_.find(id);
  return it == items_.end() ? nullptr : &it->second;
}

namespace {

Status SetSupport(std::map<ProtocolId, std::vector<SemVersion>>* target, const ProtocolId& id,
                  std::vector<SemVersion> versions) {
  if (versions.empty()) {
    return Fail(ErrorCode::InvalidDocument,
                "protocol '" + id.value() + "' declared with an empty version list");
  }
  if (versions.size() > kMaxCapabilityVersionsPerId) {
    return Fail(ErrorCode::LimitExceeded, "too many protocol versions for '" + id.value() + "'");
  }
  std::sort(versions.begin(), versions.end(),
            [](const SemVersion& a, const SemVersion& b) {
              return a.TotalOrder(b) == std::strong_ordering::less;
            });
  versions.erase(std::unique(versions.begin(), versions.end(),
                             [](const SemVersion& a, const SemVersion& b) {
                               return a.Precedence(b) == std::strong_ordering::equal;
                             }),
                 versions.end());
  (*target)[id] = std::move(versions);
  return Status{};
}

Status SetSupport(std::map<SchemaId, std::vector<SemVersion>>* target, const SchemaId& id,
                  std::vector<SemVersion> versions) {
  if (versions.empty()) {
    return Fail(ErrorCode::InvalidDocument,
                "schema '" + id.value() + "' declared with an empty version list");
  }
  if (versions.size() > kMaxCapabilityVersionsPerId) {
    return Fail(ErrorCode::LimitExceeded, "too many schema versions for '" + id.value() + "'");
  }
  std::sort(versions.begin(), versions.end(),
            [](const SemVersion& a, const SemVersion& b) {
              return a.TotalOrder(b) == std::strong_ordering::less;
            });
  versions.erase(std::unique(versions.begin(), versions.end(),
                             [](const SemVersion& a, const SemVersion& b) {
                               return a.Precedence(b) == std::strong_ordering::equal;
                             }),
                 versions.end());
  (*target)[id] = std::move(versions);
  return Status{};
}

Result<std::vector<SemVersion>> ParseVersionList(const JsonValue& json, std::string_view context) {
  const JsonValue::Array* array = json.AsArray();
  if (array == nullptr) {
    return MakeError(ErrorCode::InvalidDocument,
                     std::string(context) + " must be an array of version strings");
  }
  if (array->empty()) {
    return MakeError(ErrorCode::InvalidDocument, std::string(context) + " must not be empty");
  }
  if (array->size() > kMaxCapabilityVersionsPerId) {
    return MakeError(ErrorCode::LimitExceeded, std::string(context) + " has too many versions");
  }
  std::vector<SemVersion> versions;
  versions.reserve(array->size());
  for (const JsonValue& entry : *array) {
    const std::string* text = entry.AsString();
    if (text == nullptr) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + " entries must be version strings");
    }
    auto parsed = SemVersion::Parse(*text);
    if (!parsed.has_value()) return parsed.error();
    versions.push_back(parsed.value());
  }
  return versions;
}

JsonValue VersionListToJson(const std::vector<SemVersion>& versions) {
  JsonValue::Array out;
  out.reserve(versions.size());
  for (const SemVersion& v : versions) out.push_back(JsonValue::Str(v.ToString()));
  return JsonValue::Arr(std::move(out));
}

}  // namespace

Status ProtocolSupportSet::Set(const ProtocolId& id, std::vector<SemVersion> versions) {
  return SetSupport(&items_, id, std::move(versions));
}

bool ProtocolSupportSet::Contains(const ProtocolId& id) const { return items_.count(id) != 0; }

const std::vector<SemVersion>* ProtocolSupportSet::Get(const ProtocolId& id) const {
  const auto it = items_.find(id);
  return it == items_.end() ? nullptr : &it->second;
}

Status SchemaSupportSet::Set(const SchemaId& id, std::vector<SemVersion> versions) {
  return SetSupport(&items_, id, std::move(versions));
}

bool SchemaSupportSet::Contains(const SchemaId& id) const { return items_.count(id) != 0; }

const std::vector<SemVersion>* SchemaSupportSet::Get(const SchemaId& id) const {
  const auto it = items_.find(id);
  return it == items_.end() ? nullptr : &it->second;
}

Status Taxonomy::AddFamily(FamilyDescriptor descriptor) {
  if (families_.count(descriptor.id) != 0) {
    return Status(DuplicateError("component family", descriptor.id.value()));
  }
  families_.emplace(descriptor.id, std::move(descriptor));
  return Status{};
}

Status Taxonomy::AddKind(KindDescriptor descriptor) {
  if (kinds_.count(descriptor.id) != 0) {
    return Status(DuplicateError("component kind", descriptor.id.value()));
  }
  kinds_.emplace(descriptor.id, std::move(descriptor));
  return Status{};
}

Status Taxonomy::AddProtocol(ProtocolDescriptor descriptor) {
  if (protocols_.count(descriptor.id) != 0) {
    return Status(DuplicateError("protocol", descriptor.id.value()));
  }
  protocols_.emplace(descriptor.id, std::move(descriptor));
  return Status{};
}

Status Taxonomy::AddSchema(SchemaDescriptor descriptor) {
  if (schemas_.count(descriptor.id) != 0) {
    return Status(DuplicateError("schema", descriptor.id.value()));
  }
  schemas_.emplace(descriptor.id, std::move(descriptor));
  return Status{};
}

Status Taxonomy::AddHardwareClass(HardwareClassDescriptor descriptor) {
  if (hardware_classes_.count(descriptor.id) != 0) {
    return Status(DuplicateError("hardware class", descriptor.id.value()));
  }
  hardware_classes_.emplace(descriptor.id, std::move(descriptor));
  return Status{};
}

Status Taxonomy::AddFeature(FeatureDescriptor descriptor) {
  if (features_.count(descriptor.id) != 0) {
    return Status(DuplicateError("feature", descriptor.id.value()));
  }
  features_.emplace(descriptor.id, std::move(descriptor));
  return Status{};
}

Status Taxonomy::AddCapability(CapabilityDeclaration declaration) {
  if (capabilities_.count(declaration.id) != 0) {
    return Status(DuplicateError("capability", declaration.id.value()));
  }
  capabilities_.emplace(declaration.id, std::move(declaration));
  return Status{};
}

const KindDescriptor* Taxonomy::FindKind(const ComponentKindId& id) const {
  const auto it = kinds_.find(id);
  return it == kinds_.end() ? nullptr : &it->second;
}

const CapabilityDeclaration* Taxonomy::FindCapability(const CapabilityId& id) const {
  const auto it = capabilities_.find(id);
  return it == capabilities_.end() ? nullptr : &it->second;
}

std::size_t Taxonomy::TotalEntries() const noexcept {
  return families_.size() + kinds_.size() + protocols_.size() + schemas_.size() +
         hardware_classes_.size() + features_.size() + capabilities_.size();
}

bool operator==(const Taxonomy& a, const Taxonomy& b) {
  return a.families_.size() == b.families_.size() && a.kinds_.size() == b.kinds_.size() &&
         a.protocols_.size() == b.protocols_.size() && a.schemas_.size() == b.schemas_.size() &&
         a.hardware_classes_.size() == b.hardware_classes_.size() &&
         a.features_.size() == b.features_.size() &&
         a.capabilities_.size() == b.capabilities_.size() &&
         std::equal(a.families_.begin(), a.families_.end(), b.families_.begin(),
                    [](const auto& x, const auto& y) {
                      return x.first == y.first && x.second.description == y.second.description;
                    }) &&
         std::equal(a.kinds_.begin(), a.kinds_.end(), b.kinds_.begin(),
                    [](const auto& x, const auto& y) {
                      return x.first == y.first && x.second.family == y.second.family &&
                             x.second.description == y.second.description;
                    }) &&
         std::equal(a.protocols_.begin(), a.protocols_.end(), b.protocols_.begin(),
                    [](const auto& x, const auto& y) {
                      return x.first == y.first && x.second.description == y.second.description;
                    }) &&
         std::equal(a.schemas_.begin(), a.schemas_.end(), b.schemas_.begin(),
                    [](const auto& x, const auto& y) {
                      return x.first == y.first && x.second.description == y.second.description;
                    }) &&
         std::equal(a.hardware_classes_.begin(), a.hardware_classes_.end(),
                    b.hardware_classes_.begin(), [](const auto& x, const auto& y) {
                      return x.first == y.first && x.second.description == y.second.description;
                    }) &&
         std::equal(a.features_.begin(), a.features_.end(), b.features_.begin(),
                    [](const auto& x, const auto& y) {
                      return x.first == y.first && x.second.description == y.second.description;
                    }) &&
         std::equal(a.capabilities_.begin(), a.capabilities_.end(), b.capabilities_.begin(),
                    [](const auto& x, const auto& y) {
                      return x.first == y.first && x.second.type == y.second.type &&
                             x.second.description == y.second.description;
                    });
}

Result<Taxonomy> ParseTaxonomy(const JsonValue& json, const TaxonomyLimits& limits) {
  if (!json.IsObject()) {
    return MakeError(ErrorCode::InvalidDocument, "taxonomy must be a JSON object");
  }
  Taxonomy taxonomy;
  const auto read_ids = [&](std::string_view key, std::size_t cap, std::string_view label,
                            auto&& add) -> Status {
    const JsonValue* field = json.Find(key);
    if (field == nullptr) return Status{};
    const JsonValue::Array* array = field->AsArray();
    if (array == nullptr) {
      return Fail(ErrorCode::InvalidDocument,
                  "taxonomy." + std::string(key) + " must be an array");
    }
    if (array->size() > cap) {
      return Fail(ErrorCode::LimitExceeded,
                  "taxonomy." + std::string(key) + " exceeds the maximum of " +
                      std::to_string(cap) + " entries");
    }
    for (const JsonValue& entry : *array) {
      if (!entry.IsObject()) {
        return Fail(ErrorCode::InvalidDocument,
                    "taxonomy." + std::string(key) + " entries must be objects");
      }
      auto id = RequireString(entry, "id", label);
      if (!id.has_value()) return Status(id.error());
      auto description = OptionalString(entry, "description", "");
      if (!description.has_value()) return Status(description.error());
      Status status = add(entry, id.value(), description.value());
      if (!status.ok()) return status;
    }
    return Status{};
  };

  Status status = read_ids("families", limits.max_families, "component family",
                           [&](const JsonValue&, const std::string& id, const std::string& text) {
                             auto parsed = ComponentFamilyId::Parse(id);
                             if (!parsed.has_value()) return Status(parsed.error());
                             return taxonomy.AddFamily(FamilyDescriptor{parsed.value(), text});
                           });
  if (!status.ok()) return status.error();

  // Kinds may forward-reference their family; family existence is checked by
  // static validation so that the diagnostic is reported once, consistently.
  status = read_ids("kinds", limits.max_kinds, "component kind",
                    [&](const JsonValue& entry, const std::string& id, const std::string& text) {
                      auto parsed = ComponentKindId::Parse(id);
                      if (!parsed.has_value()) return Status(parsed.error());
                      auto family_text = RequireString(entry, "family", "component kind");
                      if (!family_text.has_value()) return Status(family_text.error());
                      auto family = ComponentFamilyId::Parse(family_text.value());
                      if (!family.has_value()) return Status(family.error());
                      return taxonomy.AddKind(KindDescriptor{parsed.value(), family.value(), text});
                    });
  if (!status.ok()) return status.error();

  status = read_ids("protocols", limits.max_protocols, "protocol",
                    [&](const JsonValue&, const std::string& id, const std::string& text) {
                      auto parsed = ProtocolId::Parse(id);
                      if (!parsed.has_value()) return Status(parsed.error());
                      return taxonomy.AddProtocol(ProtocolDescriptor{parsed.value(), text});
                    });
  if (!status.ok()) return status.error();

  status = read_ids("schemas", limits.max_schemas, "schema",
                    [&](const JsonValue&, const std::string& id, const std::string& text) {
                      auto parsed = SchemaId::Parse(id);
                      if (!parsed.has_value()) return Status(parsed.error());
                      return taxonomy.AddSchema(SchemaDescriptor{parsed.value(), text});
                    });
  if (!status.ok()) return status.error();

  status = read_ids("hardware_classes", limits.max_hardware_classes, "hardware class",
                    [&](const JsonValue&, const std::string& id, const std::string& text) {
                      auto parsed = HardwareClassId::Parse(id);
                      if (!parsed.has_value()) return Status(parsed.error());
                      return taxonomy.AddHardwareClass(HardwareClassDescriptor{parsed.value(), text});
                    });
  if (!status.ok()) return status.error();

  status = read_ids("features", limits.max_features, "feature",
                    [&](const JsonValue&, const std::string& id, const std::string& text) {
                      auto parsed = FeatureId::Parse(id);
                      if (!parsed.has_value()) return Status(parsed.error());
                      return taxonomy.AddFeature(FeatureDescriptor{parsed.value(), text});
                    });
  if (!status.ok()) return status.error();

  status = read_ids("capabilities", limits.max_capabilities, "capability",
                    [&](const JsonValue& entry, const std::string& id, const std::string& text) {
                      auto parsed = CapabilityId::Parse(id);
                      if (!parsed.has_value()) return Status(parsed.error());
                      auto type_text = RequireString(entry, "type", "capability");
                      if (!type_text.has_value()) return Status(type_text.error());
                      auto type = ParseCapabilityType(type_text.value());
                      if (!type.has_value()) return Status(type.error());
                      return taxonomy.AddCapability(
                          CapabilityDeclaration{parsed.value(), type.value(), text});
                    });
  if (!status.ok()) return status.error();
  return taxonomy;
}

JsonValue TaxonomyToJson(const Taxonomy& taxonomy) {
  JsonValue out = JsonValue::Obj();
  {
    JsonValue::Array families;
    for (const auto& [id, descriptor] : taxonomy.families()) {
      JsonValue entry = JsonValue::Obj();
      entry.Set("id", JsonValue::Str(id.value()));
      if (!descriptor.description.empty()) {
        entry.Set("description", JsonValue::Str(descriptor.description));
      }
      families.push_back(std::move(entry));
    }
    out.Set("families", JsonValue::Arr(std::move(families)));
  }
  {
    JsonValue::Array kinds;
    for (const auto& [id, descriptor] : taxonomy.kinds()) {
      JsonValue entry = JsonValue::Obj();
      entry.Set("id", JsonValue::Str(id.value()));
      entry.Set("family", JsonValue::Str(descriptor.family.value()));
      if (!descriptor.description.empty()) {
        entry.Set("description", JsonValue::Str(descriptor.description));
      }
      kinds.push_back(std::move(entry));
    }
    out.Set("kinds", JsonValue::Arr(std::move(kinds)));
  }
  const auto simple = [](const auto& source, std::string_view key) {
    JsonValue::Array array;
    for (const auto& [id, descriptor] : source) {
      JsonValue entry = JsonValue::Obj();
      entry.Set("id", JsonValue::Str(id.value()));
      if (!descriptor.description.empty()) {
        entry.Set("description", JsonValue::Str(descriptor.description));
      }
      array.push_back(std::move(entry));
    }
    (void)key;
    return JsonValue::Arr(std::move(array));
  };
  out.Set("protocols", simple(taxonomy.protocols(), "protocols"));
  out.Set("schemas", simple(taxonomy.schemas(), "schemas"));
  out.Set("hardware_classes", simple(taxonomy.hardware_classes(), "hardware_classes"));
  out.Set("features", simple(taxonomy.features(), "features"));
  {
    JsonValue::Array capabilities;
    for (const auto& [id, declaration] : taxonomy.capabilities()) {
      JsonValue entry = JsonValue::Obj();
      entry.Set("id", JsonValue::Str(id.value()));
      entry.Set("type", JsonValue::Str(std::string(CapabilityTypeName(declaration.type))));
      if (!declaration.description.empty()) {
        entry.Set("description", JsonValue::Str(declaration.description));
      }
      capabilities.push_back(std::move(entry));
    }
    out.Set("capabilities", JsonValue::Arr(std::move(capabilities)));
  }
  return out;
}

}  // namespace fcr
