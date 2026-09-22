// Fabric Compatibility Registry - Summon Software Labs
// Embedding example: how Fabric Upgrade Manager uses the registry in process.
//
// This example builds a registry generation, asks whether a candidate upgrade
// set may interoperate, and prints the decision, its evidence and its
// authority. Every value it prints is derived from the runtime, not hard coded.
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "fcr/fcr.hpp"

namespace {

fcr::Result<fcr::ComponentSpec> MakeNic(const std::string& instance, const std::string& version,
                                        bool roce, const std::vector<std::string>& rdma) {
  fcr::ComponentSpec spec;
  spec.instance = fcr::ComponentInstanceId::Parse(instance).value();
  spec.label = instance;
  spec.family = fcr::ComponentFamilyId::Parse("fabric.transport").value();
  spec.kind = fcr::ComponentKindId::Parse("rdma.nic").value();
  auto parsed = fcr::SemVersion::Parse(version);
  if (!parsed.has_value()) return parsed.error();
  spec.version = parsed.value();
  spec.knowledge.capabilities = fcr::KnowledgeClosure::Closed;
  spec.knowledge.protocols = fcr::KnowledgeClosure::Closed;
  spec.knowledge.schemas = fcr::KnowledgeClosure::Closed;
  if (roce) {
    fcr::Status status = spec.capabilities.Set(
        fcr::CapabilityId::Parse("rdma.rocev2").value(), fcr::CapabilityValue::Flag(true));
    if (!status.ok()) return status.error();
  }
  std::vector<fcr::SemVersion> versions;
  for (const std::string& text : rdma) {
    auto version_value = fcr::SemVersion::Parse(text);
    if (!version_value.has_value()) return version_value.error();
    versions.push_back(version_value.value());
  }
  fcr::Status status =
      spec.protocols.Set(fcr::ProtocolId::Parse("fabric.rdma").value(), std::move(versions));
  if (!status.ok()) return status.error();
  return spec;
}

fcr::Result<fcr::RegistryDocument> BuildRegistry() {
  auto publisher = fcr::PublisherId::Parse("platform-engineering");
  if (!publisher.has_value()) return publisher.error();
  fcr::RegistryDocumentBuilder builder("fabric-os-standard", publisher.value());
  builder.SetGeneration(fcr::GenerationNumber(1));
  builder.SetEpoch(fcr::PublisherEpoch(1));
  builder.SetCreatedAt(fcr::Timestamp::FromIso8601("2026-03-01T00:00:00Z").value());
  builder.SetDescription("example registry");

  fcr::Taxonomy& taxonomy = builder.taxonomy();
  fcr::Status status = taxonomy.AddFamily(fcr::FamilyDescriptor{
      fcr::ComponentFamilyId::Parse("fabric.transport").value(), "fabric transport"});
  if (!status.ok()) return status.error();
  status = taxonomy.AddKind(fcr::KindDescriptor{
      fcr::ComponentKindId::Parse("rdma.nic").value(),
      fcr::ComponentFamilyId::Parse("fabric.transport").value(), "RDMA adapter"});
  if (!status.ok()) return status.error();
  status = taxonomy.AddProtocol(fcr::ProtocolDescriptor{
      fcr::ProtocolId::Parse("fabric.rdma").value(), "fabric RDMA"});
  if (!status.ok()) return status.error();
  status = taxonomy.AddCapability(fcr::CapabilityDeclaration{
      fcr::CapabilityId::Parse("rdma.rocev2").value(), fcr::CapabilityType::Flag, "RoCEv2"});
  if (!status.ok()) return status.error();

  fcr::Rule rule;
  rule.id = fcr::RuleId::Parse("rdma-generation-window").value();
  rule.revision = fcr::RuleRevision(1);
  rule.priority = fcr::RulePriority(10);
  rule.outcome = fcr::RuleOutcome::Compatible;
  rule.left.kind = fcr::ComponentKindId::Parse("rdma.nic").value();
  rule.right.kind = fcr::ComponentKindId::Parse("rdma.nic").value();
  auto window = fcr::VersionRange::Parse("^2.0.0");
  if (!window.has_value()) return window.error();
  rule.left.version = window.value();
  rule.right.version = window.value();
  rule.constraints.push_back(fcr::RequiresCapability{
      fcr::Side::Left, fcr::CapabilityId::Parse("rdma.rocev2").value()});
  rule.constraints.push_back(fcr::RequiresCapability{
      fcr::Side::Right, fcr::CapabilityId::Parse("rdma.rocev2").value()});
  rule.provenance.publisher = publisher.value();
  rule.provenance.source = "example";
  rule.provenance.recorded_at = fcr::Timestamp::FromIso8601("2026-03-01T00:00:00Z").value();
  rule.rationale = "2.x adapters interoperate when both sides speak RoCEv2";
  status = builder.AddRule(rule);
  if (!status.ok()) return status.error();

  fcr::Rule legacy;
  legacy.id = fcr::RuleId::Parse("no-mixed-major").value();
  legacy.revision = fcr::RuleRevision(1);
  legacy.priority = fcr::RulePriority(20);
  legacy.outcome = fcr::RuleOutcome::Incompatible;
  legacy.left.kind = fcr::ComponentKindId::Parse("rdma.nic").value();
  auto old_range = fcr::VersionRange::Parse("<2.0.0");
  if (!old_range.has_value()) return old_range.error();
  legacy.left.version = old_range.value();
  legacy.right.kind = fcr::ComponentKindId::Parse("rdma.nic").value();
  auto new_range = fcr::VersionRange::Parse("^3.0.0");
  if (!new_range.has_value()) return new_range.error();
  legacy.right.version = new_range.value();
  legacy.provenance.publisher = publisher.value();
  legacy.provenance.recorded_at = fcr::Timestamp::FromIso8601("2026-03-01T00:00:00Z").value();
  legacy.rationale = "3.x dropped the legacy negotiation handshake";
  status = builder.AddRule(legacy);
  if (!status.ok()) return status.error();
  return builder.Build();
}

void PrintDecision(const char* label, const fcr::Decision& decision) {
  std::cout << "--- " << label << " ---\n";
  std::cout << "outcome:    " << fcr::DecisionOutcomeName(decision.outcome) << "\n";
  std::cout << "reason:     " << fcr::DecisionReasonName(decision.reason) << "\n";
  std::cout << "generation: " << decision.generation.ToString() << "\n";
  std::cout << "decision:   " << decision.id.ToHex() << "\n";
  if (decision.deciding_rule.has_value()) {
    std::cout << "decided by: " << decision.deciding_rule->ToString() << "\n";
  }
  for (const fcr::NegotiatedSubject& subject : decision.negotiation.protocols) {
    std::cout << "negotiated: " << subject.subject << " -> "
              << fcr::NegotiationStatusName(subject.status);
    if (subject.selected.has_value()) std::cout << " " << subject.selected->ToString();
    std::cout << "\n";
  }
  for (const fcr::UnmetRequirement& requirement : decision.unmet_requirements) {
    std::cout << "unmet:      " << requirement.requirement << " (" << requirement.detail << ")\n";
  }
  std::cout << "\n";
}

}  // namespace

int main() {
  auto document = BuildRegistry();
  if (!document.has_value()) {
    std::cerr << "cannot build the example registry: " << document.error().ToString() << "\n";
    return 1;
  }
  auto authority = fcr::RegistryAuthority::OpenInMemory(document.value());
  if (!authority.has_value()) {
    std::cerr << "cannot open the authority: " << authority.error().ToString() << "\n";
    return 1;
  }

  const auto left = MakeNic("node-a-nic0", "2.4.1", true, {"2.0.0", "2.1.0"});
  const auto right = MakeNic("node-b-nic0", "2.3.0", true, {"2.1.0", "2.2.0"});
  const auto legacy = MakeNic("node-c-nic0", "1.9.0", false, {"1.0.0"});
  if (!left.has_value() || !right.has_value() || !legacy.has_value()) {
    std::cerr << "cannot build the example components\n";
    return 1;
  }

  auto first = authority.value()->QueryPair(left.value(), right.value());
  if (!first.has_value()) {
    std::cerr << first.error().ToString() << "\n";
    return 1;
  }
  PrintDecision("two 2.x adapters", first.value());

  auto second = authority.value()->QueryPair(left.value(), legacy.value());
  if (!second.has_value()) {
    std::cerr << second.error().ToString() << "\n";
    return 1;
  }
  PrintDecision("2.x adapter against a 1.x adapter", second.value());

  std::cout << "full explanation of the first decision:\n\n" << first.value().narrative << "\n";

  // Publication is fenced against the generation the caller read.
  fcr::RegistryDocumentBuilder builder("fabric-os-standard",
                                       fcr::PublisherId::Parse("platform-engineering").value());
  builder.SetCreatedAt(fcr::Timestamp::FromIso8601("2026-03-02T00:00:00Z").value());
  fcr::PublishRequest request;
  request.expected_generation = fcr::GenerationNumber(2);
  auto stale = authority.value()->Publish(builder.Build(), request);
  std::cout << "stale publication refused: "
            << (stale.has_value() ? "no" : std::string(fcr::ErrorCodeName(stale.error().code)))
            << "\n";
  if (!stale.has_value()) {
    std::cout << "  " << stale.error().message << "\n";
  }
  return 0;
}
