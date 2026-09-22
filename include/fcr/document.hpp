// Fabric Compatibility Registry - Summon Software Labs
// A registry generation document: metadata, taxonomy, rules, canonical form.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fcr/component.hpp"
#include "fcr/error.hpp"
#include "fcr/identity.hpp"
#include "fcr/json.hpp"
#include "fcr/rule.hpp"
#include "fcr/taxonomy.hpp"

namespace fcr {

inline constexpr std::string_view kRegistryDocumentFormat = "fcr.registry.document";
// Bumped whenever the on-disk/on-wire document layout changes incompatibly.
inline constexpr std::uint32_t kRegistryDocumentFormatVersion = 1;

struct RegistryMetadata {
  GenerationNumber generation;
  std::optional<ContentDigest> parent_digest;
  Timestamp created_at;
  PublisherId publisher;
  PublisherEpoch epoch;
  std::string name;
  std::string description;
  std::map<std::string, std::string> labels;

  friend bool operator==(const RegistryMetadata& a, const RegistryMetadata& b);
};

// The complete, immutable description of one registry generation.
//
// Two documents that describe the same generation have byte-identical
// canonical JSON regardless of the order in which their contents were added,
// which is what makes the generation digest a stable identity.
struct RegistryDocument {
  RegistryMetadata meta;
  Taxonomy taxonomy;
  RuleList rules;

  // Sorts rules by rule identity and normalizes every rule's collections.
  void Normalize();

  // Canonical (compact, key-sorted, array-normalized) serialization.
  std::string CanonicalJson() const;
  // SHA-256 over the canonical serialization.
  ContentDigest Digest() const;

  std::size_t TotalConstraints() const;

  friend bool operator==(const RegistryDocument& a, const RegistryDocument& b);
};

struct DocumentLimits {
  std::size_t max_rules = kMaxRulesPerGeneration;
  std::size_t max_document_bytes = 8u * 1024u * 1024u;
  TaxonomyLimits taxonomy;
};

Result<RegistryDocument> ParseRegistryDocument(const JsonValue& json,
                                               const DocumentLimits& limits = DocumentLimits{});
Result<RegistryDocument> ParseRegistryDocumentText(std::string_view text,
                                                   const DocumentLimits& limits = DocumentLimits{});
JsonValue RegistryDocumentToJson(const RegistryDocument& document);

// Convenience builder used by tests, examples and tooling. Rejects duplicate
// rule identities at insert time so a builder can never assemble an ambiguous
// rule set.
class RegistryDocumentBuilder {
 public:
  RegistryDocumentBuilder(std::string name, PublisherId publisher);

  RegistryDocumentBuilder& SetGeneration(GenerationNumber generation);
  RegistryDocumentBuilder& SetEpoch(PublisherEpoch epoch);
  RegistryDocumentBuilder& SetCreatedAt(Timestamp timestamp);
  RegistryDocumentBuilder& SetDescription(std::string description);
  RegistryDocumentBuilder& SetParentDigest(ContentDigest digest);
  RegistryDocumentBuilder& SetLabel(std::string key, std::string value);

  Taxonomy& taxonomy() { return document_.taxonomy; }
  const Taxonomy& taxonomy() const { return document_.taxonomy; }

  Status AddRule(Rule rule);
  bool HasRule(const RuleId& id) const;

  const RegistryDocument& document() const { return document_; }
  RegistryDocument Build() const;

 private:
  RegistryDocument document_;
};

// Canonical document JSON for a document that is already normalized.
JsonValue RegistryDocumentBodyToJson(const RegistryDocument& document);

}  // namespace fcr
