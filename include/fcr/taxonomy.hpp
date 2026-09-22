// Fabric Compatibility Registry - Summon Software Labs
// Capability, protocol, schema and component taxonomy: the typed vocabulary
// that compatibility rules are allowed to speak.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "fcr/error.hpp"
#include "fcr/identity.hpp"
#include "fcr/json.hpp"
#include "fcr/version.hpp"

namespace fcr {

// ---------------------------------------------------------------------------
// Capability values
// ---------------------------------------------------------------------------
enum class CapabilityType : std::uint8_t { Flag, Integer, Text, Version };

std::string_view CapabilityTypeName(CapabilityType type);
Result<CapabilityType> ParseCapabilityType(std::string_view text);

// A typed capability value. Comparisons are only defined between values of the
// same capability type; the registry rejects mismatched declarations at
// validation time rather than guessing at query time.
class CapabilityValue {
 public:
  CapabilityValue() = default;
  static CapabilityValue Flag(bool v);
  static CapabilityValue Integer(std::int64_t v);
  static CapabilityValue Text(std::string v);
  static CapabilityValue Version(SemVersion v);

  CapabilityType type() const noexcept { return type_; }
  bool AsFlag(bool fallback) const noexcept;
  std::int64_t AsInteger(std::int64_t fallback) const noexcept;
  const std::string& AsText() const noexcept { return text_; }
  const SemVersion& AsVersion() const noexcept { return version_; }

  std::string ToString() const;
  JsonValue ToJson() const;
  static Result<CapabilityValue> FromJson(CapabilityType expected, const JsonValue& json);

  friend bool operator==(const CapabilityValue& a, const CapabilityValue& b);
  friend bool operator!=(const CapabilityValue& a, const CapabilityValue& b) { return !(a == b); }

 private:
  CapabilityType type_ = CapabilityType::Flag;
  bool flag_ = false;
  std::int64_t integer_ = 0;
  std::string text_;
  SemVersion version_;
};

enum class ComparisonOp : std::uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

std::string_view ComparisonOpName(ComparisonOp op);
Result<ComparisonOp> ParseComparisonOp(std::string_view text);
// True when the operator has defined semantics for the capability type.
bool ComparisonOpSupported(CapabilityType type, ComparisonOp op);
// Applies the operator. Requires equal types; returns false when undefined.
bool ApplyComparison(ComparisonOp op, const CapabilityValue& lhs, const CapabilityValue& rhs);

constexpr std::size_t kMaxCapabilityTextBytes = 256;
constexpr std::size_t kMaxCapabilityVersionsPerId = 64;

// ---------------------------------------------------------------------------
// Declarations and assertions
// ---------------------------------------------------------------------------
struct CapabilityDeclaration {
  CapabilityId id;
  CapabilityType type = CapabilityType::Flag;
  std::string description;
};

struct CapabilityAssertion {
  CapabilityId id;
  CapabilityValue value;

  friend bool operator<(const CapabilityAssertion& a, const CapabilityAssertion& b) {
    return a.id < b.id;
  }
};

// Sorted, duplicate-free set of capability assertions.
class CapabilitySet {
 public:
  Status Set(const CapabilityId& id, CapabilityValue value);
  bool Contains(const CapabilityId& id) const;
  const CapabilityValue* Get(const CapabilityId& id) const;
  std::size_t size() const noexcept { return items_.size(); }
  bool empty() const noexcept { return items_.empty(); }
  const std::map<CapabilityId, CapabilityValue>& items() const noexcept { return items_; }

  friend bool operator==(const CapabilitySet& a, const CapabilitySet& b) { return a.items_ == b.items_; }

 private:
  std::map<CapabilityId, CapabilityValue> items_;
};

// Knowledge closure: does the observation enumerate *everything* the component
// has, or only a part? Absence of a fact is only evidence of absence when the
// closure is Closed. Open closure turns "not listed" into UNKNOWN.
enum class KnowledgeClosure : std::uint8_t { Open, Closed };

std::string_view KnowledgeClosureName(KnowledgeClosure closure);
Result<KnowledgeClosure> ParseKnowledgeClosure(std::string_view text);

struct KnowledgeProfile {
  KnowledgeClosure capabilities = KnowledgeClosure::Open;
  KnowledgeClosure protocols = KnowledgeClosure::Open;
  KnowledgeClosure schemas = KnowledgeClosure::Open;

  bool AllClosed() const {
    return capabilities == KnowledgeClosure::Closed && protocols == KnowledgeClosure::Closed &&
           schemas == KnowledgeClosure::Closed;
  }
  friend bool operator==(const KnowledgeProfile& a, const KnowledgeProfile& b) {
    return a.capabilities == b.capabilities && a.protocols == b.protocols && a.schemas == b.schemas;
  }
};

// ---------------------------------------------------------------------------
// Protocol / schema support
// ---------------------------------------------------------------------------
// Support is expressed as concrete versions. Negotiation picks a concrete
// version from the intersection, which keeps "negotiated version" a total,
// reproducible value rather than an unbounded interval.
struct ProtocolSupport {
  ProtocolId protocol;
  std::vector<SemVersion> versions;
};

struct SchemaSupport {
  SchemaId schema;
  std::vector<SemVersion> versions;
};

class ProtocolSupportSet {
 public:
  Status Set(const ProtocolId& id, std::vector<SemVersion> versions);
  bool Contains(const ProtocolId& id) const;
  const std::vector<SemVersion>* Get(const ProtocolId& id) const;
  std::size_t size() const noexcept { return items_.size(); }
  bool empty() const noexcept { return items_.empty(); }
  const std::map<ProtocolId, std::vector<SemVersion>>& items() const noexcept { return items_; }

  friend bool operator==(const ProtocolSupportSet& a, const ProtocolSupportSet& b) {
    return a.items_ == b.items_;
  }

 private:
  std::map<ProtocolId, std::vector<SemVersion>> items_;
};

class SchemaSupportSet {
 public:
  Status Set(const SchemaId& id, std::vector<SemVersion> versions);
  bool Contains(const SchemaId& id) const;
  const std::vector<SemVersion>* Get(const SchemaId& id) const;
  std::size_t size() const noexcept { return items_.size(); }
  bool empty() const noexcept { return items_.empty(); }
  const std::map<SchemaId, std::vector<SemVersion>>& items() const noexcept { return items_; }

  friend bool operator==(const SchemaSupportSet& a, const SchemaSupportSet& b) {
    return a.items_ == b.items_;
  }

 private:
  std::map<SchemaId, std::vector<SemVersion>> items_;
};

// Declared features are flags a component either carries or does not. They are
// a distinct typed identity from capabilities because features are boolean by
// definition and are recorded verbatim by discovery tooling.
class FeatureSet {
 public:
  void Add(const FeatureId& id) { items_.insert(id); }
  void Remove(const FeatureId& id) { items_.erase(id); }
  bool Contains(const FeatureId& id) const { return items_.count(id) != 0; }
  std::size_t size() const noexcept { return items_.size(); }
  bool empty() const noexcept { return items_.empty(); }
  const std::set<FeatureId>& items() const noexcept { return items_; }

  friend bool operator==(const FeatureSet& a, const FeatureSet& b) { return a.items_ == b.items_; }

 private:
  std::set<FeatureId> items_;
};

// ---------------------------------------------------------------------------
// Taxonomy
// ---------------------------------------------------------------------------
struct FamilyDescriptor {
  ComponentFamilyId id;
  std::string description;
};

struct KindDescriptor {
  ComponentKindId id;
  ComponentFamilyId family;
  std::string description;
};

struct ProtocolDescriptor {
  ProtocolId id;
  std::string description;
};

struct SchemaDescriptor {
  SchemaId id;
  std::string description;
};

struct HardwareClassDescriptor {
  HardwareClassId id;
  std::string description;
};

struct FeatureDescriptor {
  FeatureId id;
  std::string description;
};

// The complete, finite vocabulary of a registry generation. Every identifier a
// rule mentions must be declared here or the generation cannot be published.
class Taxonomy {
 public:
  Status AddFamily(FamilyDescriptor descriptor);
  Status AddKind(KindDescriptor descriptor);
  Status AddProtocol(ProtocolDescriptor descriptor);
  Status AddSchema(SchemaDescriptor descriptor);
  Status AddHardwareClass(HardwareClassDescriptor descriptor);
  Status AddFeature(FeatureDescriptor descriptor);
  Status AddCapability(CapabilityDeclaration declaration);

  bool HasFamily(const ComponentFamilyId& id) const { return families_.count(id) != 0; }
  bool HasKind(const ComponentKindId& id) const { return kinds_.count(id) != 0; }
  bool HasProtocol(const ProtocolId& id) const { return protocols_.count(id) != 0; }
  bool HasSchema(const SchemaId& id) const { return schemas_.count(id) != 0; }
  bool HasHardwareClass(const HardwareClassId& id) const { return hardware_classes_.count(id) != 0; }
  bool HasFeature(const FeatureId& id) const { return features_.count(id) != 0; }
  bool HasCapability(const CapabilityId& id) const { return capabilities_.count(id) != 0; }

  const KindDescriptor* FindKind(const ComponentKindId& id) const;
  const CapabilityDeclaration* FindCapability(const CapabilityId& id) const;

  const std::map<ComponentFamilyId, FamilyDescriptor>& families() const noexcept { return families_; }
  const std::map<ComponentKindId, KindDescriptor>& kinds() const noexcept { return kinds_; }
  const std::map<ProtocolId, ProtocolDescriptor>& protocols() const noexcept { return protocols_; }
  const std::map<SchemaId, SchemaDescriptor>& schemas() const noexcept { return schemas_; }
  const std::map<HardwareClassId, HardwareClassDescriptor>& hardware_classes() const noexcept {
    return hardware_classes_;
  }
  const std::map<FeatureId, FeatureDescriptor>& features() const noexcept { return features_; }
  const std::map<CapabilityId, CapabilityDeclaration>& capabilities() const noexcept {
    return capabilities_;
  }

  std::size_t TotalEntries() const noexcept;

  friend bool operator==(const Taxonomy& a, const Taxonomy& b);

 private:
  std::map<ComponentFamilyId, FamilyDescriptor> families_;
  std::map<ComponentKindId, KindDescriptor> kinds_;
  std::map<ProtocolId, ProtocolDescriptor> protocols_;
  std::map<SchemaId, SchemaDescriptor> schemas_;
  std::map<HardwareClassId, HardwareClassDescriptor> hardware_classes_;
  std::map<FeatureId, FeatureDescriptor> features_;
  std::map<CapabilityId, CapabilityDeclaration> capabilities_;
};

struct TaxonomyLimits {
  std::size_t max_families = 256;
  std::size_t max_kinds = 1024;
  std::size_t max_protocols = 512;
  std::size_t max_schemas = 512;
  std::size_t max_hardware_classes = 256;
  std::size_t max_features = 1024;
  std::size_t max_capabilities = 4096;
};

Result<Taxonomy> ParseTaxonomy(const JsonValue& json, const TaxonomyLimits& limits = TaxonomyLimits{});
JsonValue TaxonomyToJson(const Taxonomy& taxonomy);

}  // namespace fcr
