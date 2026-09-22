#include "fcr/document.hpp"

#include <algorithm>

#include "fcr/digest.hpp"

namespace fcr {

bool operator==(const RegistryMetadata& a, const RegistryMetadata& b) {
  return a.generation == b.generation && a.parent_digest == b.parent_digest &&
         a.created_at == b.created_at && a.publisher == b.publisher && a.epoch == b.epoch &&
         a.name == b.name && a.description == b.description && a.labels == b.labels;
}

void RegistryDocument::Normalize() {
  for (Rule& rule : rules) NormalizeRule(rule);
  std::sort(rules.begin(), rules.end(), [](const Rule& a, const Rule& b) {
    if (a.id != b.id) return a.id < b.id;
    return a.revision < b.revision;
  });
}

std::size_t RegistryDocument::TotalConstraints() const {
  std::size_t total = 0;
  for (const Rule& rule : rules) total += rule.constraints.size();
  return total;
}

bool operator==(const RegistryDocument& a, const RegistryDocument& b) {
  if (!(a.meta == b.meta)) return false;
  if (!(a.taxonomy == b.taxonomy)) return false;
  if (a.rules.size() != b.rules.size()) return false;
  for (std::size_t i = 0; i < a.rules.size(); ++i) {
    if (RuleContentDigest(a.rules[i]) != RuleContentDigest(b.rules[i])) return false;
  }
  return true;
}

JsonValue RegistryDocumentBodyToJson(const RegistryDocument& document) {
  JsonValue out = JsonValue::Obj();
  out.Set("format", JsonValue::Str(std::string(kRegistryDocumentFormat)));
  out.Set("format_version", JsonValue::UInt(kRegistryDocumentFormatVersion));
  out.Set("generation", JsonValue::UInt(document.meta.generation.value()));
  if (document.meta.parent_digest.has_value()) {
    out.Set("parent_digest", JsonValue::Str(document.meta.parent_digest->ToHex()));
  }
  out.Set("created_at", JsonValue::Str(document.meta.created_at.ToIso8601()));
  out.Set("publisher", JsonValue::Str(document.meta.publisher.value()));
  out.Set("publisher_epoch", JsonValue::UInt(document.meta.epoch.value()));
  out.Set("name", JsonValue::Str(document.meta.name));
  if (!document.meta.description.empty()) {
    out.Set("description", JsonValue::Str(document.meta.description));
  }
  if (!document.meta.labels.empty()) {
    JsonValue labels = JsonValue::Obj();
    for (const auto& [key, value] : document.meta.labels) {
      labels.Set(key, JsonValue::Str(value));
    }
    out.Set("labels", std::move(labels));
  }
  out.Set("taxonomy", TaxonomyToJson(document.taxonomy));
  {
    JsonValue::Array rules;
    rules.reserve(document.rules.size());
    for (const Rule& rule : document.rules) rules.push_back(RuleToJson(rule));
    out.Set("rules", JsonValue::Arr(std::move(rules)));
  }
  return out;
}

JsonValue RegistryDocumentToJson(const RegistryDocument& document) {
  RegistryDocument normalized = document;
  normalized.Normalize();
  return RegistryDocumentBodyToJson(normalized);
}

std::string RegistryDocument::CanonicalJson() const {
  RegistryDocument normalized = *this;
  normalized.Normalize();
  return RegistryDocumentBodyToJson(normalized).Dump(false);
}

ContentDigest RegistryDocument::Digest() const {
  return Sha256::Hash(CanonicalJson());
}

Result<RegistryDocument> ParseRegistryDocument(const JsonValue& json,
                                               const DocumentLimits& limits) {
  if (!json.IsObject()) {
    return MakeError(ErrorCode::InvalidDocument, "registry document must be a JSON object");
  }
  auto format = RequireString(json, "format", "document");
  if (!format.has_value()) return format.error();
  if (format.value() != kRegistryDocumentFormat) {
    return MakeError(ErrorCode::InvalidDocument, "unexpected document format identifier",
                     format.value());
  }
  auto format_version = RequireUInt(json, "format_version", "document");
  if (!format_version.has_value()) return format_version.error();
  if (format_version.value() != kRegistryDocumentFormatVersion) {
    return MakeError(ErrorCode::UnsupportedFormatVersion,
                     "unsupported registry document format version",
                     std::to_string(format_version.value()));
  }

  RegistryDocument document;
  auto generation = RequireUInt(json, "generation", "document");
  if (!generation.has_value()) return generation.error();
  document.meta.generation = GenerationNumber(generation.value());

  const JsonValue* parent = json.Find("parent_digest");
  if (parent != nullptr && !parent->IsNull()) {
    const std::string* text = parent->AsString();
    if (text == nullptr) {
      return MakeError(ErrorCode::InvalidDocument, "parent_digest must be a hex string");
    }
    auto digest = ContentDigest::FromHex(*text);
    if (!digest.has_value()) return digest.error();
    document.meta.parent_digest = digest.value();
  }

  auto created_at = RequireString(json, "created_at", "document");
  if (!created_at.has_value()) return created_at.error();
  auto timestamp = Timestamp::FromIso8601(created_at.value());
  if (!timestamp.has_value()) return timestamp.error();
  document.meta.created_at = timestamp.value();

  auto publisher = RequireString(json, "publisher", "document");
  if (!publisher.has_value()) return publisher.error();
  auto publisher_id = PublisherId::Parse(publisher.value());
  if (!publisher_id.has_value()) return publisher_id.error();
  document.meta.publisher = publisher_id.value();

  auto epoch = RequireUInt(json, "publisher_epoch", "document");
  if (!epoch.has_value()) return epoch.error();
  document.meta.epoch = PublisherEpoch(epoch.value());

  auto name = RequireString(json, "name", "document");
  if (!name.has_value()) return name.error();
  if (name.value().size() > 256) {
    return MakeError(ErrorCode::LimitExceeded, "document name exceeds 256 bytes");
  }
  document.meta.name = name.value();

  auto description = OptionalString(json, "description", "");
  if (!description.has_value()) return description.error();
  document.meta.description = description.value();

  const JsonValue* labels = json.Find("labels");
  if (labels != nullptr && !labels->IsNull()) {
    if (!labels->IsObject()) {
      return MakeError(ErrorCode::InvalidDocument, "labels must be a JSON object");
    }
    if (labels->size() > 64) {
      return MakeError(ErrorCode::LimitExceeded, "too many labels");
    }
    for (std::size_t i = 0; i < labels->keys().size(); ++i) {
      const std::string& key = labels->keys()[i];
      if (key.empty() || key.size() > 64) {
        return MakeError(ErrorCode::InvalidDocument, "label keys must be 1..64 bytes");
      }
      const std::string* value = labels->values()[i].AsString();
      if (value == nullptr) {
        return MakeError(ErrorCode::InvalidDocument, "label values must be strings");
      }
      document.meta.labels[key] = *value;
    }
  }

  auto taxonomy_json = RequireField(json, "taxonomy", "document");
  if (!taxonomy_json.has_value()) return taxonomy_json.error();
  auto taxonomy = ParseTaxonomy(*taxonomy_json.value(), limits.taxonomy);
  if (!taxonomy.has_value()) return taxonomy.error();
  document.taxonomy = std::move(taxonomy).value();

  auto rules = RequireArray(json, "rules", "document");
  if (!rules.has_value()) return rules.error();
  if (rules.value()->size() > limits.max_rules) {
    return MakeError(ErrorCode::LimitExceeded,
                     "document declares more rules than the configured maximum of " +
                         std::to_string(limits.max_rules));
  }
  document.rules.reserve(rules.value()->size());
  for (std::size_t i = 0; i < rules.value()->size(); ++i) {
    auto rule = ParseRule((*rules.value())[i], "rules[" + std::to_string(i) + "]");
    if (!rule.has_value()) return rule.error();
    document.rules.push_back(std::move(rule).value());
  }
  document.Normalize();
  return document;
}

Result<RegistryDocument> ParseRegistryDocumentText(std::string_view text,
                                                   const DocumentLimits& limits) {
  JsonLimits json_limits;
  json_limits.max_bytes = limits.max_document_bytes;
  auto json = ParseJson(text, json_limits);
  if (!json.has_value()) return json.error();
  return ParseRegistryDocument(json.value(), limits);
}

RegistryDocumentBuilder::RegistryDocumentBuilder(std::string name, PublisherId publisher) {
  document_.meta.name = std::move(name);
  document_.meta.publisher = std::move(publisher);
  document_.meta.generation = GenerationNumber(1);
  document_.meta.epoch = PublisherEpoch(1);
}

RegistryDocumentBuilder& RegistryDocumentBuilder::SetGeneration(GenerationNumber generation) {
  document_.meta.generation = generation;
  return *this;
}

RegistryDocumentBuilder& RegistryDocumentBuilder::SetEpoch(PublisherEpoch epoch) {
  document_.meta.epoch = epoch;
  return *this;
}

RegistryDocumentBuilder& RegistryDocumentBuilder::SetCreatedAt(Timestamp timestamp) {
  document_.meta.created_at = timestamp;
  return *this;
}

RegistryDocumentBuilder& RegistryDocumentBuilder::SetDescription(std::string description) {
  document_.meta.description = std::move(description);
  return *this;
}

RegistryDocumentBuilder& RegistryDocumentBuilder::SetParentDigest(ContentDigest digest) {
  document_.meta.parent_digest = digest;
  return *this;
}

RegistryDocumentBuilder& RegistryDocumentBuilder::SetLabel(std::string key, std::string value) {
  document_.meta.labels[std::move(key)] = std::move(value);
  return *this;
}

Status RegistryDocumentBuilder::AddRule(Rule rule) {
  for (const Rule& existing : document_.rules) {
    if (existing.id == rule.id) {
      return Fail(ErrorCode::DuplicateIdentity,
                  "rule '" + rule.id.value() + "' is already present in this generation");
    }
  }
  NormalizeRule(rule);
  document_.rules.push_back(std::move(rule));
  return Status{};
}

bool RegistryDocumentBuilder::HasRule(const RuleId& id) const {
  for (const Rule& rule : document_.rules) {
    if (rule.id == id) return true;
  }
  return false;
}

RegistryDocument RegistryDocumentBuilder::Build() const {
  RegistryDocument out = document_;
  out.Normalize();
  return out;
}

}  // namespace fcr
