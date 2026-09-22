// Independent downstream consumer of the installed Fabric Compatibility
// Registry package. This project lives outside the runtime repository and only
// sees the exported CMake package and installed headers.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <fcr/fcr.hpp>

namespace {

fcr::Result<fcr::ComponentSpec> MakeNic(const std::string& instance, const std::string& version,
                                        bool roce) {
  fcr::ComponentSpec spec;
  spec.instance = fcr::ComponentInstanceId::Parse(instance).value();
  spec.family = fcr::ComponentFamilyId::Parse("fabric.transport").value();
  spec.kind = fcr::ComponentKindId::Parse("rdma.nic").value();
  auto parsed = fcr::SemVersion::Parse(version);
  if (!parsed.has_value()) return parsed.error();
  spec.version = parsed.value();
  spec.knowledge.capabilities = fcr::KnowledgeClosure::Closed;
  spec.knowledge.protocols = fcr::KnowledgeClosure::Closed;
  spec.knowledge.schemas = fcr::KnowledgeClosure::Closed;
  if (roce) {
    fcr::Status status = spec.capabilities.Set(fcr::CapabilityId::Parse("rdma.rocev2").value(),
                                               fcr::CapabilityValue::Flag(true));
    if (!status.ok()) return status.error();
  }
  fcr::Status protocols = spec.protocols.Set(fcr::ProtocolId::Parse("fabric.rdma").value(),
                                             {fcr::SemVersion(2, 0, 0)});
  if (!protocols.ok()) return protocols.error();
  return spec;
}

}  // namespace

int main() {
  fcr::RegistryDocumentBuilder builder("consumer-check",
                                       fcr::PublisherId::Parse("downstream").value());
  builder.SetCreatedAt(fcr::Timestamp::FromIso8601("2026-04-01T00:00:00Z").value());
  fcr::Taxonomy& taxonomy = builder.taxonomy();
  const auto family = fcr::ComponentFamilyId::Parse("fabric.transport").value();
  const auto kind = fcr::ComponentKindId::Parse("rdma.nic").value();
  const auto protocol = fcr::ProtocolId::Parse("fabric.rdma").value();
  const auto capability = fcr::CapabilityId::Parse("rdma.rocev2").value();
  fcr::Status status = taxonomy.AddFamily(fcr::FamilyDescriptor{family, "transport"});
  if (!status.ok()) return 1;
  status = taxonomy.AddKind(fcr::KindDescriptor{kind, family, "adapter"});
  if (!status.ok()) return 1;
  status = taxonomy.AddProtocol(fcr::ProtocolDescriptor{protocol, "RDMA"});
  if (!status.ok()) return 1;
  status = taxonomy.AddCapability(
      fcr::CapabilityDeclaration{capability, fcr::CapabilityType::Flag, "RoCEv2"});
  if (!status.ok()) return 1;

  fcr::Rule rule;
  rule.id = fcr::RuleId::Parse("consumer-rule").value();
  rule.revision = fcr::RuleRevision(1);
  rule.priority = fcr::RulePriority(5);
  rule.outcome = fcr::RuleOutcome::Compatible;
  rule.left.kind = kind;
  rule.right.kind = kind;
  rule.left.version = fcr::VersionRange::Parse("^2.0.0").value();
  rule.right.version = fcr::VersionRange::Parse("^2.0.0").value();
  rule.constraints.push_back(fcr::RequiresCapability{fcr::Side::Left, capability});
  rule.constraints.push_back(fcr::RequiresCapability{fcr::Side::Right, capability});
  rule.provenance.publisher = fcr::PublisherId::Parse("downstream").value();
  rule.provenance.recorded_at = fcr::Timestamp::FromIso8601("2026-04-01T00:00:00Z").value();
  status = builder.AddRule(rule);
  if (!status.ok()) return 1;

  auto authority = fcr::RegistryAuthority::OpenInMemory(builder.Build());
  if (!authority.has_value()) {
    std::fprintf(stderr, "open failed: %s\n", authority.error().ToString().c_str());
    return 1;
  }
  auto left = MakeNic("node-a", "2.4.1", true);
  auto right = MakeNic("node-b", "2.2.0", true);
  if (!left.has_value() || !right.has_value()) return 1;
  auto decision = authority.value()->QueryPair(left.value(), right.value());
  if (!decision.has_value()) {
    std::fprintf(stderr, "query failed: %s\n", decision.error().ToString().c_str());
    return 1;
  }
  std::printf("runtime %s decided %s via %s under generation %s\n", fcr::kRuntimeVersion,
              std::string(fcr::DecisionOutcomeName(decision.value().outcome)).c_str(),
              decision.value().deciding_rule.has_value()
                  ? decision.value().deciding_rule->ToString().c_str()
                  : "(no rule)",
              decision.value().generation.ToString().c_str());
  const bool ok = decision.value().IsCompatible() &&
                  decision.value().generation.digest ==
                      fcr::Sha256::Hash(authority.value()->Snapshot().value()->document()
                                            .CanonicalJson());
  return ok ? 0 : 1;
}
