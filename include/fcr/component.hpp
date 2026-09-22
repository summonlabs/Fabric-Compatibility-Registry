// Fabric Compatibility Registry - Summon Software Labs
// Component observations (the subject of a compatibility query) and the
// selectors that rules use to pick them.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "fcr/error.hpp"
#include "fcr/identity.hpp"
#include "fcr/json.hpp"
#include "fcr/taxonomy.hpp"
#include "fcr/version.hpp"

namespace fcr {

// A concrete, observed component: what it is, what it has, and how complete
// that knowledge is. Vendor identity is carried for reporting only and never
// participates in a decision.
struct ComponentSpec {
  ComponentInstanceId instance;
  std::string label;
  ComponentFamilyId family;
  ComponentKindId kind;
  SemVersion version;
  std::optional<HardwareClassId> hardware_class;
  std::optional<VendorId> vendor;
  KnowledgeProfile knowledge;
  CapabilitySet capabilities;
  ProtocolSupportSet protocols;
  SchemaSupportSet schemas;
  FeatureSet features;

  bool CapabilitiesClosed() const { return knowledge.capabilities == KnowledgeClosure::Closed; }
  bool ProtocolsClosed() const { return knowledge.protocols == KnowledgeClosure::Closed; }
  bool SchemasClosed() const { return knowledge.schemas == KnowledgeClosure::Closed; }

  friend bool operator==(const ComponentSpec& a, const ComponentSpec& b);
};

// Selects components within a rule. Unset dimensions are wildcards. An
// all-wildcard selector matches every registered component.
struct ComponentSelector {
  std::optional<ComponentFamilyId> family;
  std::optional<ComponentKindId> kind;
  std::optional<VersionRange> version;
  std::optional<HardwareClassId> hardware_class;

  bool IsWildcard() const {
    return !family.has_value() && !kind.has_value() && !version.has_value() &&
           !hardware_class.has_value();
  }
  std::size_t ConstrainedDimensions() const;
  bool VersionIsExact() const { return version.has_value() && version->IsExact(); }
  bool VersionIsBounded() const { return version.has_value() && !version->IsAny(); }
  std::string ToString() const;

  friend bool operator==(const ComponentSelector& a, const ComponentSelector& b);
};

bool SelectorMatches(const ComponentSelector& selector, const ComponentSpec& component);

// Structural region containment: every component matched by inner is also
// matched by outer. Used by static reachability analysis.
bool SelectorSubsumes(const ComponentSelector& outer, const ComponentSelector& inner);
// True when at least one component could satisfy both selectors at once.
bool SelectorsOverlap(const ComponentSelector& a, const ComponentSelector& b);

struct ComponentSpecLimits {
  std::size_t max_capabilities = 512;
  std::size_t max_protocols = 128;
  std::size_t max_schemas = 128;
  std::size_t max_features = 256;
};

struct ComponentSelectorLimits {
  std::size_t max_text_bytes = 512;
};

// Component specs are typed against a taxonomy: a spec may only assert
// capabilities, protocols and schemas the registry declares.
Result<ComponentSpec> ParseComponentSpec(const JsonValue& json, const Taxonomy& taxonomy,
                                         std::string_view context,
                                         const ComponentSpecLimits& limits = ComponentSpecLimits{});
JsonValue ComponentSpecToJson(const ComponentSpec& spec);

Result<ComponentSelector> ParseComponentSelector(const JsonValue& json,
                                                 const ComponentSelectorLimits& limits =
                                                     ComponentSelectorLimits{});
JsonValue ComponentSelectorToJson(const ComponentSelector& selector);

Result<ComponentSpec> ParseComponentSpecText(std::string_view text, const Taxonomy& taxonomy,
                                             std::string_view context);

}  // namespace fcr
