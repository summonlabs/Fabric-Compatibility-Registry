// Fabric Compatibility Registry - Summon Software Labs
// fcr-bench: measures completed registry work. Every figure reported here is
// the throughput of operations that finished, never of operations enqueued.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "fcr/fcr.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Summary {
  std::string label;
  std::size_t operations = 0;
  double seconds = 0.0;
  double p50_micros = 0.0;
  double p99_micros = 0.0;

  double PerSecond() const { return seconds > 0.0 ? static_cast<double>(operations) / seconds : 0.0; }
};

double Percentile(std::vector<double> samples, double fraction) {
  if (samples.empty()) return 0.0;
  std::sort(samples.begin(), samples.end());
  const std::size_t index = static_cast<std::size_t>(
      fraction * static_cast<double>(samples.size() - 1));
  return samples[index];
}

void Report(const Summary& summary) {
  std::printf("%-46s %10zu ops %9.3f s %12.1f ops/s  p50 %8.1f us  p99 %8.1f us\n",
              summary.label.c_str(), summary.operations, summary.seconds, summary.PerSecond(),
              summary.p50_micros, summary.p99_micros);
}

fcr::Result<fcr::RegistryDocument> BuildDocument(std::size_t rule_count, std::size_t generation) {
  auto publisher = fcr::PublisherId::Parse("benchmark");
  if (!publisher.has_value()) return publisher.error();
  fcr::RegistryDocumentBuilder builder("fabric-os-benchmark", publisher.value());
  builder.SetGeneration(fcr::GenerationNumber(generation));
  builder.SetEpoch(fcr::PublisherEpoch(1));
  builder.SetCreatedAt(fcr::Timestamp::FromNanos(1767225600000000000LL));

  fcr::Taxonomy& taxonomy = builder.taxonomy();
  const auto family = fcr::ComponentFamilyId::Parse("fabric.transport").value();
  const auto kind = fcr::ComponentKindId::Parse("rdma.nic").value();
  const auto protocol = fcr::ProtocolId::Parse("fabric.rdma").value();
  const auto capability = fcr::CapabilityId::Parse("rdma.rocev2").value();
  fcr::Status status = taxonomy.AddFamily(fcr::FamilyDescriptor{family, "transport"});
  if (!status.ok()) return status.error();
  status = taxonomy.AddKind(fcr::KindDescriptor{kind, family, "adapter"});
  if (!status.ok()) return status.error();
  status = taxonomy.AddProtocol(fcr::ProtocolDescriptor{protocol, "RDMA"});
  if (!status.ok()) return status.error();
  status = taxonomy.AddCapability(
      fcr::CapabilityDeclaration{capability, fcr::CapabilityType::Flag, "RoCEv2"});
  if (!status.ok()) return status.error();

  for (std::size_t i = 0; i < rule_count; ++i) {
    fcr::Rule rule;
    rule.id = fcr::RuleId::Parse("bench-rule-" + std::to_string(i)).value();
    rule.revision = fcr::RuleRevision(1);
    rule.priority = fcr::RulePriority(static_cast<std::int32_t>(i % 7));
    rule.outcome = (i % 11 == 0) ? fcr::RuleOutcome::Incompatible : fcr::RuleOutcome::Compatible;
    rule.left.kind = kind;
    rule.right.kind = kind;
    // Every rule occupies a distinct minor version band so the rule set is
    // consistent by construction.
    const std::string band = ">=" + std::to_string(i) + ".0.0 <" + std::to_string(i + 1) + ".0.0";
    auto range = fcr::VersionRange::Parse(band);
    if (!range.has_value()) return range.error();
    rule.left.version = range.value();
    rule.right.version = range.value();
    if (i % 3 == 0) {
      rule.constraints.push_back(fcr::RequiresCapability{fcr::Side::Left, capability});
      rule.constraints.push_back(fcr::RequiresCapability{fcr::Side::Right, capability});
    }
    rule.provenance.publisher = publisher.value();
    rule.provenance.source = "benchmark";
    rule.provenance.recorded_at = fcr::Timestamp::FromNanos(1767225600000000000LL);
    status = builder.AddRule(rule);
    if (!status.ok()) return status.error();
  }
  return builder.Build();
}

fcr::ComponentSpec MakeSpec(const std::string& instance, const std::string& version) {
  fcr::ComponentSpec spec;
  spec.instance = fcr::ComponentInstanceId::Parse(instance).value();
  spec.family = fcr::ComponentFamilyId::Parse("fabric.transport").value();
  spec.kind = fcr::ComponentKindId::Parse("rdma.nic").value();
  spec.version = fcr::SemVersion::Parse(version).value();
  spec.knowledge.capabilities = fcr::KnowledgeClosure::Closed;
  spec.knowledge.protocols = fcr::KnowledgeClosure::Closed;
  spec.knowledge.schemas = fcr::KnowledgeClosure::Closed;
  fcr::Status status = spec.capabilities.Set(fcr::CapabilityId::Parse("rdma.rocev2").value(),
                                             fcr::CapabilityValue::Flag(true));
  (void)status;
  return spec;
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t iterations = 20000;
  std::size_t rule_count = 512;
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    if (token == "--iterations" && i + 1 < argc) {
      iterations = static_cast<std::size_t>(std::stoul(argv[++i]));
    } else if (token == "--rules" && i + 1 < argc) {
      rule_count = static_cast<std::size_t>(std::stoul(argv[++i]));
    }
  }

  std::printf("fcr-bench %s - completed-work measurements\n\n", fcr::kRuntimeVersion);

  auto document = BuildDocument(rule_count, 1);
  if (!document.has_value()) {
    std::cerr << "cannot build the benchmark document: " << document.error().ToString() << "\n";
    return 1;
  }

  {
    constexpr std::size_t kRounds = 20;
    std::vector<double> samples;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < kRounds; ++i) {
      const auto round_start = Clock::now();
      auto compiled = fcr::RegistryGeneration::Compile(document.value());
      if (!compiled.has_value()) {
        std::cerr << compiled.error().ToString() << "\n";
        return 1;
      }
      samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - round_start).count());
    }
    Summary summary;
    summary.label = "registry validate+compile (" + std::to_string(rule_count) + " rules)";
    summary.operations = kRounds;
    summary.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    summary.p50_micros = Percentile(samples, 0.50);
    summary.p99_micros = Percentile(samples, 0.99);
    Report(summary);
  }

  auto compiled = fcr::RegistryGeneration::Compile(document.value());
  if (!compiled.has_value()) {
    std::cerr << compiled.error().ToString() << "\n";
    return 1;
  }
  const auto registry = compiled.value();

  {
    std::vector<double> samples;
    samples.reserve(iterations);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
      const std::string band = std::to_string(i % rule_count);
      fcr::ComponentSpec left = MakeSpec("a", band + ".1.0");
      fcr::ComponentSpec right = MakeSpec("b", band + ".1.0");
      const auto op_start = Clock::now();
      const fcr::Decision decision = registry->EvaluatePair(left, right);
      samples.push_back(
          std::chrono::duration<double, std::micro>(Clock::now() - op_start).count());
      if (decision.id.ToHex().empty()) return 1;
    }
    Summary summary;
    summary.label = "pair compatibility decision";
    summary.operations = iterations;
    summary.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    summary.p50_micros = Percentile(samples, 0.50);
    summary.p99_micros = Percentile(samples, 0.99);
    Report(summary);
  }

  {
    std::vector<double> samples;
    const std::size_t rounds = std::max<std::size_t>(200, iterations / 20);
    samples.reserve(rounds);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < rounds; ++i) {
      const auto op_start = Clock::now();
      const std::string canonical = registry->document().CanonicalJson();
      const fcr::ContentDigest digest = fcr::Sha256::Hash(canonical);
      samples.push_back(
          std::chrono::duration<double, std::micro>(Clock::now() - op_start).count());
      if (digest.IsZero()) return 1;
    }
    Summary summary;
    summary.label = "canonical serialization + SHA-256";
    summary.operations = rounds;
    summary.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    summary.p50_micros = Percentile(samples, 0.50);
    summary.p99_micros = Percentile(samples, 0.99);
    Report(summary);
  }

  {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "fcr-bench-publish";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    fcr::AuthorityOptions options;
    options.store.directory = directory;
    options.store.mode = fcr::OpenMode::ReadWrite;
    auto authority = fcr::RegistryAuthority::Open(options);
    if (!authority.has_value()) {
      std::cerr << authority.error().ToString() << "\n";
      return 1;
    }
    const std::size_t rounds = std::max<std::size_t>(20, iterations / 200);
    std::vector<double> samples;
    samples.reserve(rounds);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < rounds; ++i) {
      auto next = BuildDocument(std::min<std::size_t>(rule_count, 64), i + 1);
      if (!next.has_value()) {
        std::cerr << next.error().ToString() << "\n";
        return 1;
      }
      fcr::PublishRequest request;
      request.expected_generation = fcr::GenerationNumber(i);
      const auto op_start = Clock::now();
      auto published = authority.value()->Publish(next.value(), request);
      samples.push_back(
          std::chrono::duration<double, std::micro>(Clock::now() - op_start).count());
      if (!published.has_value()) {
        std::cerr << published.error().ToString() << "\n";
        return 1;
      }
    }
    Summary summary;
    summary.label = "atomic persistent generation publication";
    summary.operations = rounds;
    summary.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    summary.p50_micros = Percentile(samples, 0.50);
    summary.p99_micros = Percentile(samples, 0.99);
    Report(summary);
    authority.value().reset();
    std::filesystem::remove_all(directory, ec);
  }

  {
    fcr::AuthorityOptions options;
    options.allow_empty = false;
    auto authority = fcr::RegistryAuthority::OpenInMemory(document.value());
    if (!authority.has_value()) {
      std::cerr << authority.error().ToString() << "\n";
      return 1;
    }
    fcr::ServerOptions server_options;
    server_options.worker_threads = 2;
    auto server = fcr::RegistryServer::Start(authority.value(), server_options);
    if (!server.has_value()) {
      std::cerr << server.error().ToString() << "\n";
      return 1;
    }
    fcr::ClientOptions client_options;
    client_options.port = server.value()->port();
    auto client = fcr::RegistryClient::Connect(client_options);
    if (!client.has_value()) {
      std::cerr << client.error().ToString() << "\n";
      return 1;
    }
    fcr::JsonValue ping = fcr::JsonValue::Obj();
    ping.Set("op", fcr::JsonValue::Str("ping"));
    const std::size_t rounds = std::max<std::size_t>(200, iterations / 20);
    std::vector<double> samples;
    samples.reserve(rounds);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < rounds; ++i) {
      const auto op_start = Clock::now();
      auto response = client.value()->CallResult(ping);
      samples.push_back(
          std::chrono::duration<double, std::micro>(Clock::now() - op_start).count());
      if (!response.has_value()) {
        std::cerr << response.error().ToString() << "\n";
        return 1;
      }
    }
    Summary summary;
    summary.label = "loopback TCP request/response round trip";
    summary.operations = rounds;
    summary.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    summary.p50_micros = Percentile(samples, 0.50);
    summary.p99_micros = Percentile(samples, 0.99);
    Report(summary);

    fcr::JsonValue left = fcr::JsonValue::Obj();
    left.Set("family", fcr::JsonValue::Str("fabric.transport"));
    left.Set("kind", fcr::JsonValue::Str("rdma.nic"));
    left.Set("version", fcr::JsonValue::Str("1.1.0"));
    fcr::JsonValue query = fcr::JsonValue::Obj();
    query.Set("op", fcr::JsonValue::Str("query_pair"));
    query.Set("left", left);
    query.Set("right", left);
    std::vector<double> query_samples;
    query_samples.reserve(rounds);
    const auto query_start = Clock::now();
    for (std::size_t i = 0; i < rounds; ++i) {
      const auto op_start = Clock::now();
      auto response = client.value()->CallResult(query);
      query_samples.push_back(
          std::chrono::duration<double, std::micro>(Clock::now() - op_start).count());
      if (!response.has_value()) {
        std::cerr << response.error().ToString() << "\n";
        return 1;
      }
    }
    Summary query_summary;
    query_summary.label = "remote pair decision over loopback TCP";
    query_summary.operations = rounds;
    query_summary.seconds = std::chrono::duration<double>(Clock::now() - query_start).count();
    query_summary.p50_micros = Percentile(query_samples, 0.50);
    query_summary.p99_micros = Percentile(query_samples, 0.99);
    Report(query_summary);

    client.value()->Close();
    server.value()->Shutdown();
    const fcr::ServerStats stats = server.value()->stats();
    std::printf("\nservice accounting: accepted=%llu completed=%llu failed=%llu active=%zu\n",
                static_cast<unsigned long long>(stats.accepted_connections),
                static_cast<unsigned long long>(stats.completed_requests),
                static_cast<unsigned long long>(stats.failed_requests), stats.active_connections);
  }
  return 0;
}
