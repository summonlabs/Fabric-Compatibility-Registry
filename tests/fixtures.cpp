#include "fixtures.hpp"

#include <chrono>
#include <mutex>
#include <vector>

namespace fcr::test {
namespace {

PublisherId TestPublisher() {
  static const PublisherId id = [] {
    auto parsed = PublisherId::Parse("test-suite");
    return parsed.value();
  }();
  return id;
}

Timestamp FixedTimestamp() { return Timestamp::FromNanos(1767225600000000000LL); }

RuleProvenance Provenance(const std::string& note) {
  RuleProvenance provenance;
  provenance.publisher = TestPublisher();
  provenance.source = "test-suite";
  provenance.reference = "https://example.invalid/registry";
  provenance.recorded_at = FixedTimestamp();
  provenance.change_note = note;
  return provenance;
}

}  // namespace

namespace {

// One static object owns both the registry and its cleanup. Two separate
// function-local statics would be destroyed in the wrong order (the vector
// first), so the cleanup would read a destroyed container.
struct TempRegistry {
  std::mutex mutex;
  std::vector<std::string> directories;

  ~TempRegistry() {
    for (const std::string& path : directories) {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
    directories.clear();
  }
};

TempRegistry& TempDirectories() {
  static TempRegistry registry;
  return registry;
}

}  // namespace

void RemoveDirectoryQuietly(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

std::string UniqueTempDirectory(const std::string& tag) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  static std::uint64_t counter = 0;
  ++counter;
  std::filesystem::path base = std::filesystem::temp_directory_path();
  std::string name = "fcr-test-" + tag + "-" + std::to_string(nanos) + "-" +
                     std::to_string(counter);
  std::filesystem::path directory = base / name;
  std::error_code ec;
  std::filesystem::remove_all(directory, ec);
  std::filesystem::create_directories(directory, ec);
  {
    TempRegistry& registry = TempDirectories();
    std::lock_guard<std::mutex> guard(registry.mutex);
    registry.directories.push_back(directory.string());
  }
  return directory.string();
}

ComponentFamilyId TransportFamily() {
  static const ComponentFamilyId id = ComponentFamilyId::Parse("fabric.transport").value();
  return id;
}

ComponentFamilyId ComputeFamily() {
  static const ComponentFamilyId id = ComponentFamilyId::Parse("fabric.compute").value();
  return id;
}

ComponentKindId NicKind() {
  static const ComponentKindId id = ComponentKindId::Parse("rdma.nic").value();
  return id;
}

ComponentKindId SwitchKind() {
  static const ComponentKindId id = ComponentKindId::Parse("fabric.switch").value();
  return id;
}

ComponentKindId GpuKind() {
  static const ComponentKindId id = ComponentKindId::Parse("gpu.accelerator").value();
  return id;
}

ProtocolId RdmaProtocol() {
  static const ProtocolId id = ProtocolId::Parse("fabric.rdma").value();
  return id;
}

ProtocolId LinkProtocol() {
  static const ProtocolId id = ProtocolId::Parse("fabric.link").value();
  return id;
}

SchemaId ConfigSchema() {
  static const SchemaId id = SchemaId::Parse("fabric.config").value();
  return id;
}

CapabilityId RoceCapability() {
  static const CapabilityId id = CapabilityId::Parse("rdma.rocev2").value();
  return id;
}

CapabilityId LanesCapability() {
  static const CapabilityId id = CapabilityId::Parse("link.lanes").value();
  return id;
}

CapabilityId ChannelCapability() {
  static const CapabilityId id = CapabilityId::Parse("fw.channel").value();
  return id;
}

CapabilityId ApiLevelCapability() {
  static const CapabilityId id = CapabilityId::Parse("api.level").value();
  return id;
}

FeatureId SecureBootFeature() {
  static const FeatureId id = FeatureId::Parse("secure.boot").value();
  return id;
}

HardwareClassId SmartNicGen3() {
  static const HardwareClassId id = HardwareClassId::Parse("smartnic.gen3").value();
  return id;
}

HardwareClassId SmartNicGen1() {
  static const HardwareClassId id = HardwareClassId::Parse("smartnic.gen1").value();
  return id;
}

Rule MakeRule(const std::string& id, RuleOutcome outcome) {
  Rule rule;
  rule.id = RuleId::Parse(id).value();
  rule.revision = RuleRevision(1);
  rule.priority = RulePriority(0);
  rule.origin = RuleOrigin::Standard;
  rule.symmetry = RuleSymmetry::Symmetric;
  rule.arity = RuleArity::Pair;
  rule.outcome = outcome;
  rule.provenance = Provenance("initial");
  return rule;
}

RegistryDocumentBuilder MakeStandardBuilder() {
  RegistryDocumentBuilder builder("fabric-os-standard", TestPublisher());
  builder.SetGeneration(GenerationNumber(1));
  builder.SetEpoch(PublisherEpoch(1));
  builder.SetCreatedAt(FixedTimestamp());
  builder.SetDescription("fixture registry used by the validation suite");

  Taxonomy& taxonomy = builder.taxonomy();
  CHECK_OK(taxonomy.AddFamily(FamilyDescriptor{TransportFamily(), "fabric transport"}));
  CHECK_OK(taxonomy.AddFamily(FamilyDescriptor{ComputeFamily(), "fabric compute"}));
  CHECK_OK(taxonomy.AddKind(KindDescriptor{NicKind(), TransportFamily(), "RDMA network adapter"}));
  CHECK_OK(
      taxonomy.AddKind(KindDescriptor{SwitchKind(), TransportFamily(), "fabric switch"}));
  CHECK_OK(
      taxonomy.AddKind(KindDescriptor{GpuKind(), ComputeFamily(), "accelerator package"}));
  CHECK_OK(taxonomy.AddProtocol(ProtocolDescriptor{RdmaProtocol(), "fabric RDMA"}));
  CHECK_OK(taxonomy.AddProtocol(ProtocolDescriptor{LinkProtocol(), "fabric link layer"}));
  CHECK_OK(taxonomy.AddSchema(SchemaDescriptor{ConfigSchema(), "fabric configuration schema"}));
  CHECK_OK(taxonomy.AddCapability(
      CapabilityDeclaration{RoceCapability(), CapabilityType::Flag, "RoCEv2 support"}));
  CHECK_OK(taxonomy.AddCapability(
      CapabilityDeclaration{LanesCapability(), CapabilityType::Integer, "link lane count"}));
  CHECK_OK(taxonomy.AddCapability(
      CapabilityDeclaration{ChannelCapability(), CapabilityType::Text, "firmware channel"}));
  CHECK_OK(taxonomy.AddCapability(
      CapabilityDeclaration{ApiLevelCapability(), CapabilityType::Version, "API level"}));
  CHECK_OK(taxonomy.AddHardwareClass(HardwareClassDescriptor{SmartNicGen3(), "third gen"}));
  CHECK_OK(taxonomy.AddHardwareClass(HardwareClassDescriptor{SmartNicGen1(), "first gen"}));
  CHECK_OK(taxonomy.AddFeature(FeatureDescriptor{SecureBootFeature(), "measured secure boot"}));

  // A blanket rule: any two NICs at the same major generation interoperate.
  {
    Rule rule = MakeRule("nic-same-major", RuleOutcome::Compatible);
    rule.priority = RulePriority(0);
    rule.left.kind = NicKind();
    rule.right.kind = NicKind();
    CHECK_OK(builder.AddRule(rule));
  }
  // Higher priority rule: NICs below 2.0.0 are incompatible with NICs of 3.x.
  {
    Rule rule = MakeRule("nic-legacy-vs-modern", RuleOutcome::Incompatible);
    rule.priority = RulePriority(10);
    rule.left.kind = NicKind();
    rule.left.version = VersionRange::Parse("<2.0.0").value();
    rule.right.kind = NicKind();
    rule.right.version = VersionRange::Parse("^3.0.0").value();
    rule.rationale = "the 3.x control plane dropped the legacy negotiation handshake";
    CHECK_OK(builder.AddRule(rule));
  }
  // Conditional rule: NIC pairs need the RoCE capability on both sides.
  {
    Rule rule = MakeRule("nic-requires-roce", RuleOutcome::Compatible);
    rule.priority = RulePriority(20);
    rule.left.kind = NicKind();
    rule.left.version = VersionRange::Parse("^2.0.0").value();
    rule.right.kind = NicKind();
    rule.right.version = VersionRange::Parse("^2.0.0").value();
    rule.constraints.push_back(RequiresCapability{Side::Left, RoceCapability()});
    rule.constraints.push_back(RequiresCapability{Side::Right, RoceCapability()});
    rule.constraints.push_back(RequiresProtocol{Side::Left, RdmaProtocol(),
                                                 VersionRange::Parse("^2.0.0").value()});
    rule.constraints.push_back(RequiresProtocol{Side::Right, RdmaProtocol(),
                                                 VersionRange::Parse("^2.0.0").value()});
    CHECK_OK(builder.AddRule(rule));
  }
  // Asymmetric rule: a NIC at 2.x can front a switch at 1.x but not the reverse.
  {
    Rule rule = MakeRule("nic-fronts-legacy-switch", RuleOutcome::Compatible);
    rule.priority = RulePriority(30);
    rule.symmetry = RuleSymmetry::LeftToRight;
    rule.left.kind = NicKind();
    rule.left.version = VersionRange::Parse("^2.0.0").value();
    rule.right.kind = SwitchKind();
    rule.right.version = VersionRange::Parse("^1.0.0").value();
    CHECK_OK(builder.AddRule(rule));
  }
  // Hardware class rule: gen1 smart NICs never pair with gen3 smart NICs.
  {
    Rule rule = MakeRule("smartnic-generation-mix", RuleOutcome::Incompatible);
    rule.priority = RulePriority(40);
    rule.left.kind = NicKind();
    rule.left.hardware_class = SmartNicGen1();
    rule.right.kind = NicKind();
    rule.right.hardware_class = SmartNicGen3();
    CHECK_OK(builder.AddRule(rule));
  }
  // Set rule: every transport member of a set must report secure boot.
  {
    Rule rule = MakeRule("transport-set-secure-boot", RuleOutcome::Compatible);
    rule.arity = RuleArity::Set;
    rule.left.family = TransportFamily();
    rule.constraints.push_back(RequiresFeature{Side::AllMembers, SecureBootFeature()});
    CHECK_OK(builder.AddRule(rule));
  }
  return builder;
}

RegistryDocument MakeStandardDocument() { return MakeStandardBuilder().Build(); }

void CloseKnowledge(ComponentSpec* spec) {
  spec->knowledge.capabilities = KnowledgeClosure::Closed;
  spec->knowledge.protocols = KnowledgeClosure::Closed;
  spec->knowledge.schemas = KnowledgeClosure::Closed;
}

ComponentSpec MakeNic(const std::string& instance, const std::string& version) {
  ComponentSpec spec;
  spec.instance = ComponentInstanceId::Parse(instance).value();
  spec.label = instance;
  spec.family = TransportFamily();
  spec.kind = NicKind();
  spec.version = SemVersion::Parse(version).value();
  CloseKnowledge(&spec);
  return spec;
}

ComponentSpec MakeSwitch(const std::string& instance, const std::string& version) {
  ComponentSpec spec;
  spec.instance = ComponentInstanceId::Parse(instance).value();
  spec.label = instance;
  spec.family = TransportFamily();
  spec.kind = SwitchKind();
  spec.version = SemVersion::Parse(version).value();
  CloseKnowledge(&spec);
  return spec;
}

ComponentSpec MakeGpu(const std::string& instance, const std::string& version) {
  ComponentSpec spec;
  spec.instance = ComponentInstanceId::Parse(instance).value();
  spec.label = instance;
  spec.family = ComputeFamily();
  spec.kind = GpuKind();
  spec.version = SemVersion::Parse(version).value();
  CloseKnowledge(&spec);
  return spec;
}

}  // namespace fcr::test
