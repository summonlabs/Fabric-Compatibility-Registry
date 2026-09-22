#include "fcr/component.hpp"

#include <algorithm>
#include <map>

namespace fcr {
namespace {

std::string SelectorText(const ComponentSelector& selector) {
  std::string out;
  const auto append = [&out](std::string_view key, const std::string& value) {
    if (!out.empty()) out.append(", ");
    out.append(key);
    out.push_back('=');
    out.append(value);
  };
  if (selector.family.has_value()) append("family", selector.family->value());
  if (selector.kind.has_value()) append("kind", selector.kind->value());
  if (selector.version.has_value()) append("version", selector.version->ToString());
  if (selector.hardware_class.has_value()) {
    append("hardware_class", selector.hardware_class->value());
  }
  if (out.empty()) out = "*";
  return out;
}

}  // namespace

bool operator==(const ComponentSpec& a, const ComponentSpec& b) {
  return a.instance == b.instance && a.label == b.label && a.family == b.family && a.kind == b.kind &&
         a.version.Precedence(b.version) == std::strong_ordering::equal &&
         a.hardware_class == b.hardware_class && a.vendor == b.vendor && a.knowledge == b.knowledge &&
         a.capabilities == b.capabilities && a.protocols == b.protocols && a.schemas == b.schemas &&
         a.features == b.features;
}

std::size_t ComponentSelector::ConstrainedDimensions() const {
  std::size_t count = 0;
  if (family.has_value()) ++count;
  if (kind.has_value()) ++count;
  if (version.has_value()) ++count;
  if (hardware_class.has_value()) ++count;
  return count;
}

std::string ComponentSelector::ToString() const { return SelectorText(*this); }

bool operator==(const ComponentSelector& a, const ComponentSelector& b) {
  return a.family == b.family && a.kind == b.kind && a.hardware_class == b.hardware_class &&
         a.version == b.version;
}

bool SelectorMatches(const ComponentSelector& selector, const ComponentSpec& component) {
  if (selector.family.has_value() && *selector.family != component.family) return false;
  if (selector.kind.has_value() && *selector.kind != component.kind) return false;
  if (selector.hardware_class.has_value()) {
    if (!component.hardware_class.has_value()) return false;
    if (*selector.hardware_class != *component.hardware_class) return false;
  }
  if (selector.version.has_value() && !selector.version->Contains(component.version)) return false;
  return true;
}

bool SelectorSubsumes(const ComponentSelector& outer, const ComponentSelector& inner) {
  const auto id_subsumes = [](const auto& outer_id, const auto& inner_id) {
    if (!outer_id.has_value()) return true;
    if (!inner_id.has_value()) return false;
    return *outer_id == *inner_id;
  };
  if (!id_subsumes(outer.family, inner.family)) return false;
  if (!id_subsumes(outer.kind, inner.kind)) return false;
  if (!id_subsumes(outer.hardware_class, inner.hardware_class)) return false;
  if (!outer.version.has_value()) return true;
  if (!inner.version.has_value()) return false;
  return outer.version->Subsumes(*inner.version);
}

bool SelectorsOverlap(const ComponentSelector& a, const ComponentSelector& b) {
  const auto id_overlaps = [](const auto& x, const auto& y) {
    if (!x.has_value() || !y.has_value()) return true;
    return *x == *y;
  };
  if (!id_overlaps(a.family, b.family)) return false;
  if (!id_overlaps(a.kind, b.kind)) return false;
  if (!id_overlaps(a.hardware_class, b.hardware_class)) return false;
  if (a.version.has_value() && b.version.has_value()) {
    return VersionRange::Overlaps(*a.version, *b.version);
  }
  return true;
}

Result<ComponentSpec> ParseComponentSpec(const JsonValue& json, const Taxonomy& taxonomy,
                                         std::string_view context,
                                         const ComponentSpecLimits& limits) {
  if (!json.IsObject()) {
    return MakeError(ErrorCode::InvalidDocument, std::string(context) + " must be a JSON object");
  }
  ComponentSpec spec;
  auto family_text = RequireString(json, "family", context);
  if (!family_text.has_value()) return family_text.error();
  auto family = ComponentFamilyId::Parse(family_text.value());
  if (!family.has_value()) return family.error();
  spec.family = family.value();

  auto kind_text = RequireString(json, "kind", context);
  if (!kind_text.has_value()) return kind_text.error();
  auto kind = ComponentKindId::Parse(kind_text.value());
  if (!kind.has_value()) return kind.error();
  spec.kind = kind.value();

  auto version_text = RequireString(json, "version", context);
  if (!version_text.has_value()) return version_text.error();
  auto version = SemVersion::Parse(version_text.value());
  if (!version.has_value()) return version.error();
  spec.version = version.value();

  auto instance_text = OptionalString(json, "instance", "");
  if (!instance_text.has_value()) return instance_text.error();
  if (!instance_text.value().empty()) {
    auto instance = ComponentInstanceId::Parse(instance_text.value());
    if (!instance.has_value()) return instance.error();
    spec.instance = instance.value();
  }
  auto label = OptionalString(json, "label", "");
  if (!label.has_value()) return label.error();
  spec.label = label.value();

  const JsonValue* hardware = json.Find("hardware_class");
  if (hardware != nullptr && !hardware->IsNull()) {
    const std::string* text = hardware->AsString();
    if (text == nullptr) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + ".hardware_class must be a string");
    }
    auto parsed = HardwareClassId::Parse(*text);
    if (!parsed.has_value()) return parsed.error();
    spec.hardware_class = parsed.value();
  }
  const JsonValue* vendor = json.Find("vendor");
  if (vendor != nullptr && !vendor->IsNull()) {
    const std::string* text = vendor->AsString();
    if (text == nullptr) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + ".vendor must be a string");
    }
    auto parsed = VendorId::Parse(*text);
    if (!parsed.has_value()) return parsed.error();
    spec.vendor = parsed.value();
  }

  const JsonValue* knowledge = json.Find("knowledge");
  if (knowledge != nullptr && !knowledge->IsNull()) {
    if (!knowledge->IsObject()) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + ".knowledge must be an object");
    }
    const auto read_closure = [&](std::string_view key, KnowledgeClosure* target) -> Status {
      const JsonValue* field = knowledge->Find(key);
      if (field == nullptr || field->IsNull()) return Status{};
      const std::string* text = field->AsString();
      if (text == nullptr) {
        return Fail(ErrorCode::InvalidDocument,
                    std::string(context) + ".knowledge." + std::string(key) + " must be a string");
      }
      auto parsed = ParseKnowledgeClosure(*text);
      if (!parsed.has_value()) return Status(parsed.error());
      *target = parsed.value();
      return Status{};
    };
    Status status = read_closure("capabilities", &spec.knowledge.capabilities);
    if (!status.ok()) return status.error();
    status = read_closure("protocols", &spec.knowledge.protocols);
    if (!status.ok()) return status.error();
    status = read_closure("schemas", &spec.knowledge.schemas);
    if (!status.ok()) return status.error();
  }

  const JsonValue* features = json.Find("features");
  if (features != nullptr && !features->IsNull()) {
    const JsonValue::Array* array = features->AsArray();
    if (array == nullptr) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + ".features must be an array of feature ids");
    }
    if (array->size() > limits.max_features) {
      return MakeError(ErrorCode::LimitExceeded,
                       std::string(context) + ".features exceeds the size limit");
    }
    for (const JsonValue& entry : *array) {
      const std::string* text = entry.AsString();
      if (text == nullptr) {
        return MakeError(ErrorCode::InvalidDocument,
                         std::string(context) + ".features entries must be strings");
      }
      auto id = FeatureId::Parse(*text);
      if (!id.has_value()) return id.error();
      if (!taxonomy.HasFeature(id.value())) {
        return MakeError(ErrorCode::UndefinedReference,
                         std::string(context) + " declares undeclared feature '" + *text + "'");
      }
      spec.features.Add(id.value());
    }
  }

  const JsonValue* capabilities = json.Find("capabilities");
  if (capabilities != nullptr && !capabilities->IsNull()) {
    if (!capabilities->IsObject()) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + ".capabilities must be an object");
    }
    if (capabilities->size() > limits.max_capabilities) {
      return MakeError(ErrorCode::LimitExceeded,
                       std::string(context) + ".capabilities exceeds the size limit");
    }
    for (std::size_t i = 0; i < capabilities->keys().size(); ++i) {
      const std::string& key = capabilities->keys()[i];
      auto id = CapabilityId::Parse(key);
      if (!id.has_value()) return id.error();
      const CapabilityDeclaration* declaration = taxonomy.FindCapability(id.value());
      if (declaration == nullptr) {
        return MakeError(ErrorCode::UndefinedReference,
                         std::string(context) + " asserts undeclared capability '" + key + "'");
      }
      auto value = CapabilityValue::FromJson(declaration->type, capabilities->values()[i]);
      if (!value.has_value()) return value.error();
      Status status = spec.capabilities.Set(id.value(), value.value());
      if (!status.ok()) return status.error();
    }
  }

  const JsonValue* protocols = json.Find("protocols");
  if (protocols != nullptr && !protocols->IsNull()) {
    if (!protocols->IsObject()) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + ".protocols must be an object");
    }
    if (protocols->size() > limits.max_protocols) {
      return MakeError(ErrorCode::LimitExceeded,
                       std::string(context) + ".protocols exceeds the size limit");
    }
    for (std::size_t i = 0; i < protocols->keys().size(); ++i) {
      const std::string& key = protocols->keys()[i];
      auto id = ProtocolId::Parse(key);
      if (!id.has_value()) return id.error();
      if (!taxonomy.HasProtocol(id.value())) {
        return MakeError(ErrorCode::UndefinedReference,
                         std::string(context) + " asserts undeclared protocol '" + key + "'");
      }
      const JsonValue::Array* array = protocols->values()[i].AsArray();
      if (array == nullptr || array->empty()) {
        return MakeError(ErrorCode::InvalidDocument,
                         std::string(context) + " protocol version list must be a non-empty array");
      }
      if (array->size() > kMaxCapabilityVersionsPerId) {
        return MakeError(ErrorCode::LimitExceeded,
                         std::string(context) + " protocol version list is too long");
      }
      std::vector<SemVersion> versions;
      versions.reserve(array->size());
      for (const JsonValue& entry : *array) {
        const std::string* text = entry.AsString();
        if (text == nullptr) {
          return MakeError(ErrorCode::InvalidDocument,
                           std::string(context) + " protocol versions must be strings");
        }
        auto parsed = SemVersion::Parse(*text);
        if (!parsed.has_value()) return parsed.error();
        versions.push_back(parsed.value());
      }
      Status status = spec.protocols.Set(id.value(), std::move(versions));
      if (!status.ok()) return status.error();
    }
  }

  const JsonValue* schemas = json.Find("schemas");
  if (schemas != nullptr && !schemas->IsNull()) {
    if (!schemas->IsObject()) {
      return MakeError(ErrorCode::InvalidDocument,
                       std::string(context) + ".schemas must be an object");
    }
    if (schemas->size() > limits.max_schemas) {
      return MakeError(ErrorCode::LimitExceeded,
                       std::string(context) + ".schemas exceeds the size limit");
    }
    for (std::size_t i = 0; i < schemas->keys().size(); ++i) {
      const std::string& key = schemas->keys()[i];
      auto id = SchemaId::Parse(key);
      if (!id.has_value()) return id.error();
      if (!taxonomy.HasSchema(id.value())) {
        return MakeError(ErrorCode::UndefinedReference,
                         std::string(context) + " asserts undeclared schema '" + key + "'");
      }
      const JsonValue::Array* array = schemas->values()[i].AsArray();
      if (array == nullptr || array->empty()) {
        return MakeError(ErrorCode::InvalidDocument,
                         std::string(context) + " schema version list must be a non-empty array");
      }
      if (array->size() > kMaxCapabilityVersionsPerId) {
        return MakeError(ErrorCode::LimitExceeded,
                         std::string(context) + " schema version list is too long");
      }
      std::vector<SemVersion> versions;
      versions.reserve(array->size());
      for (const JsonValue& entry : *array) {
        const std::string* text = entry.AsString();
        if (text == nullptr) {
          return MakeError(ErrorCode::InvalidDocument,
                           std::string(context) + " schema versions must be strings");
        }
        auto parsed = SemVersion::Parse(*text);
        if (!parsed.has_value()) return parsed.error();
        versions.push_back(parsed.value());
      }
      Status status = spec.schemas.Set(id.value(), std::move(versions));
      if (!status.ok()) return status.error();
    }
  }
  return spec;
}

JsonValue ComponentSpecToJson(const ComponentSpec& spec) {
  JsonValue out = JsonValue::Obj();
  if (!spec.instance.empty()) out.Set("instance", JsonValue::Str(spec.instance.value()));
  if (!spec.label.empty()) out.Set("label", JsonValue::Str(spec.label));
  out.Set("family", JsonValue::Str(spec.family.value()));
  out.Set("kind", JsonValue::Str(spec.kind.value()));
  out.Set("version", JsonValue::Str(spec.version.ToString()));
  if (spec.hardware_class.has_value()) {
    out.Set("hardware_class", JsonValue::Str(spec.hardware_class->value()));
  }
  if (spec.vendor.has_value()) out.Set("vendor", JsonValue::Str(spec.vendor->value()));
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
  if (!spec.features.empty()) {
    JsonValue::Array features;
    for (const FeatureId& id : spec.features.items()) {
      features.push_back(JsonValue::Str(id.value()));
    }
    out.Set("features", JsonValue::Arr(std::move(features)));
  }
  return out;
}

Result<ComponentSpec> ParseComponentSpecText(std::string_view text, const Taxonomy& taxonomy,
                                             std::string_view context) {
  auto json = ParseJson(text);
  if (!json.has_value()) return json.error();
  return ParseComponentSpec(json.value(), taxonomy, context);
}

Result<ComponentSelector> ParseComponentSelector(const JsonValue& json,
                                                 const ComponentSelectorLimits& limits) {
  if (!json.IsObject()) {
    return MakeError(ErrorCode::InvalidDocument, "selector must be a JSON object");
  }
  (void)limits;
  ComponentSelector selector;
  const auto read_id = [&](std::string_view key, auto* target, auto parse) -> Status {
    const JsonValue* field = json.Find(key);
    if (field == nullptr || field->IsNull()) return Status{};
    const std::string* text = field->AsString();
    if (text == nullptr) {
      return Fail(ErrorCode::InvalidDocument, "selector." + std::string(key) + " must be a string");
    }
    auto parsed = parse(*text);
    if (!parsed.has_value()) return Status(parsed.error());
    *target = parsed.value();
    return Status{};
  };
  Status status = read_id("family", &selector.family,
                          [](std::string_view t) { return ComponentFamilyId::Parse(t); });
  if (!status.ok()) return status.error();
  status = read_id("kind", &selector.kind,
                   [](std::string_view t) { return ComponentKindId::Parse(t); });
  if (!status.ok()) return status.error();
  status = read_id("hardware_class", &selector.hardware_class,
                   [](std::string_view t) { return HardwareClassId::Parse(t); });
  if (!status.ok()) return status.error();

  const JsonValue* version = json.Find("version");
  if (version != nullptr && !version->IsNull()) {
    const std::string* text = version->AsString();
    if (text == nullptr) {
      return MakeError(ErrorCode::InvalidDocument, "selector.version must be a string");
    }
    auto parsed = VersionRange::Parse(*text);
    if (!parsed.has_value()) return parsed.error();
    selector.version = parsed.value();
  }
  return selector;
}

JsonValue ComponentSelectorToJson(const ComponentSelector& selector) {
  JsonValue out = JsonValue::Obj();
  if (selector.family.has_value()) out.Set("family", JsonValue::Str(selector.family->value()));
  if (selector.kind.has_value()) out.Set("kind", JsonValue::Str(selector.kind->value()));
  if (selector.hardware_class.has_value()) {
    out.Set("hardware_class", JsonValue::Str(selector.hardware_class->value()));
  }
  if (selector.version.has_value()) out.Set("version", JsonValue::Str(selector.version->ToString()));
  return out;
}

}  // namespace fcr
