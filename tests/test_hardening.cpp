// Fabric Compatibility Registry - Summon Software Labs
// Second hardening pass: adversarial cases that go beyond the primary suites.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fixtures.hpp"
#include "process_helper.hpp"

using namespace fcr;
using namespace fcr::test;

namespace {

std::filesystem::path GenerationFile(const std::string& directory, GenerationNumber number) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "gen-%020llu.fcr",
                static_cast<unsigned long long>(number.value()));
  return std::filesystem::path(directory) / buffer;
}

std::string ReadRawFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::string text;
  char buffer[4096];
  while (stream) {
    stream.read(buffer, sizeof(buffer));
    text.append(buffer, static_cast<std::size_t>(stream.gcount()));
  }
  return text;
}

void WriteRawFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// A document whose rule set makes a query match many rules without any of them
// being unreachable: every high-precedence rule declines to apply.
RegistryDocument ManyMatchingRules(std::size_t count) {
  RegistryDocumentBuilder builder = MakeStandardBuilder();
  RegistryDocument document = builder.Build();
  document.rules.clear();
  for (std::size_t i = 0; i < count; ++i) {
    Rule rule = MakeRule("skipper-" + std::to_string(i), RuleOutcome::Incompatible);
    rule.priority = RulePriority(static_cast<std::int32_t>(1000 - static_cast<int>(i)));
    rule.on_unmet = OnUnmetRequirement::Skip;
    rule.on_indeterminate = OnIndeterminate::Skip;
    rule.left.kind = NicKind();
    rule.right.kind = NicKind();
    // The capability is absent from the query and its knowledge is closed, so
    // the requirement is definitely unmet and the rule declines to apply.
    rule.constraints.push_back(RequiresCapability{Side::Left, RoceCapability()});
    document.rules.push_back(rule);
  }
  Rule decider = MakeRule("final-decider", RuleOutcome::Compatible);
  decider.priority = RulePriority(-1);
  decider.left.kind = NicKind();
  decider.right.kind = NicKind();
  document.rules.push_back(decider);
  document.Normalize();
  return document;
}

}  // namespace

FCR_TEST(hardening, corrupt_head_metadata_is_rejected) {
  const std::string directory = UniqueTempDirectory("meta-corrupt");
  {
    auto store = RegistryStore::Open([&] {
      StoreOptions options;
      options.directory = directory;
      options.mode = OpenMode::ReadWrite;
      return options;
    }());
    CHECK_OK(store);
    CHECK_OK(store.value().Publish(MakeStandardDocument(), store.value().CurrentFence().value()));
  }
  const std::filesystem::path meta_path = std::filesystem::path(directory) / "registry.meta";
  const std::string original = ReadRawFile(meta_path);
  CHECK(original.size() > 100);
  // Flip a character inside the recorded generation digest.
  std::string corrupted = original;
  const std::size_t position = corrupted.find("generation_digest") + 30;
  CHECK(position < corrupted.size());
  corrupted[position] = corrupted[position] == 'a' ? 'b' : 'a';
  WriteRawFile(meta_path, corrupted);

  StoreOptions options;
  options.directory = directory;
  options.mode = OpenMode::ReadOnly;
  auto store = RegistryStore::Open(options);
  // Either the metadata fails its integrity check outright, or it parses and
  // the mismatch is caught when the generation is read.
  if (store.has_value()) {
    auto loaded = store.value().LoadLatest();
    CHECK_FALSE(loaded.has_value());
  } else {
    CHECK(store.error().code == ErrorCode::DigestMismatch ||
          store.error().code == ErrorCode::CorruptPersistence ||
          store.error().code == ErrorCode::InvalidJson);
  }
  // Restoring the original metadata makes the directory usable again.
  WriteRawFile(meta_path, original);
  auto restored = RegistryStore::Open(options);
  CHECK_OK(restored);
  CHECK_OK(restored.value().LoadLatest());
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(hardening, head_pointing_at_a_missing_generation_recovers) {
  const std::string directory = UniqueTempDirectory("missing-head");
  {
    auto store = RegistryStore::Open([&] {
      StoreOptions options;
      options.directory = directory;
      options.mode = OpenMode::ReadWrite;
      return options;
    }());
    CHECK_OK(store);
    CHECK_OK(store.value().Publish(MakeStandardDocument(), store.value().CurrentFence().value()));
    CHECK_OK(store.value().Publish(MakeStandardDocument(), store.value().CurrentFence().value()));
  }
  std::filesystem::remove(GenerationFile(directory, GenerationNumber(2)));
  StoreOptions options;
  options.directory = directory;
  options.mode = OpenMode::ReadWrite;
  auto store = RegistryStore::Open(options);
  CHECK_OK(store);
  auto loaded = store.value().LoadLatest();
  CHECK_OK(loaded);
  CHECK_EQ(loaded.value().id.number.value(), 1u);
  CHECK(loaded.value().recovery.recovered);
  CHECK_EQ(loaded.value().recovery.skipped_corrupt.size(), std::size_t{1});
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(hardening, pruning_never_removes_the_authoritative_generation) {
  const std::string directory = UniqueTempDirectory("prune-head");
  StoreOptions options;
  options.directory = directory;
  options.mode = OpenMode::ReadWrite;
  auto store = RegistryStore::Open(options);
  CHECK_OK(store);
  for (int i = 0; i < 5; ++i) {
    CHECK_OK(store.value().Publish(MakeStandardDocument(), store.value().CurrentFence().value()));
  }
  const StoreMeta head = store.value().ReadMeta().value();
  auto removed = store.value().Prune(0);
  CHECK_OK(removed);
  auto generations = store.value().ListGenerations();
  CHECK_OK(generations);
  CHECK_EQ(generations.value().size(), std::size_t{1});
  CHECK(generations.value().front() == head.generation);
  auto loaded = store.value().LoadLatest();
  CHECK_OK(loaded);
  CHECK_EQ(loaded.value().id.number.value(), head.generation.value());
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(hardening, publish_races_with_cancellation_leave_a_consistent_store) {
  const std::string directory = UniqueTempDirectory("cancel-race");
  StoreOptions options;
  options.directory = directory;
  options.mode = OpenMode::ReadWrite;
  auto store = RegistryStore::Open(options);
  CHECK_OK(store);
  std::size_t succeeded = 0;
  std::size_t cancelled = 0;
  // Ten uncontended publications, ten cancelled before they start, and ten
  // genuine races in which either outcome is legal.
  for (int round = 0; round < 30; ++round) {
    CancellationToken token;
    std::thread canceller;
    if (round >= 10) canceller = std::thread([&token] { token.Cancel(); });
    auto fence = store.value().CurrentFence();
    CHECK_OK(fence);
    auto published = store.value().Publish(MakeStandardDocument(), fence.value(), token);
    if (canceller.joinable()) canceller.join();
    if (published.has_value()) {
      ++succeeded;
    } else if (published.error().code == ErrorCode::Cancelled) {
      ++cancelled;
    } else {
      FCR_FAIL("unexpected failure: " << published.error().ToString());
    }
  }
  CHECK_EQ(succeeded + cancelled, std::size_t{30});
  CHECK(succeeded >= 10);
  CHECK(cancelled >= 10);
  // Whatever interleaving happened, the store holds exactly the generations
  // that were committed and no temporary debris.
  auto generations = store.value().ListGenerations();
  CHECK_OK(generations);
  CHECK_EQ(generations.value().size(), succeeded);
  std::size_t temporary_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().find(".tmp-") != std::string::npos) ++temporary_files;
  }
  CHECK_EQ(temporary_files, std::size_t{0});
  auto loaded = store.value().LoadLatest();
  CHECK_OK(loaded);
  CHECK_EQ(loaded.value().id.number.value(), succeeded);
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(hardening, evidence_truncation_keeps_the_deciding_rule) {
  RegistryDocument document = ManyMatchingRules(kMaxEvidenceRules + 40);
  auto registry = RegistryGeneration::Compile(document);
  CHECK_OK(registry);
  const ComponentSpec left = MakeNic("nic-a", "1.0.0");
  const ComponentSpec right = MakeNic("nic-b", "1.0.0");
  const Decision decision = registry.value()->EvaluatePair(left, right);
  CHECK(decision.IsCompatible());
  CHECK_EQ(decision.deciding_rule->id.value(), std::string("final-decider"));
  CHECK(decision.evidence_truncated);
  CHECK_EQ(decision.matched_rules.size(), kMaxEvidenceRules);
  bool decider_present = false;
  for (const RuleEvaluation& evaluation : decision.matched_rules) {
    if (evaluation.decided) decider_present = true;
  }
  CHECK(decider_present);
  CHECK_EQ(decision.total_matched_rules, kMaxEvidenceRules + 41);
  // The decision identifier remains stable under repeated evaluation.
  const Decision again = registry.value()->EvaluatePair(left, right);
  CHECK(again.id == decision.id);
}

FCR_TEST(hardening, reachability_budget_exhaustion_is_reported) {
  RegistryDocument document = MakeStandardDocument();
  document.rules.clear();
  // Three windows that between them cover every version on the left side.
  const char* windows[] = {"<1.0.0", ">=1.0.0 <2.0.0", ">=2.0.0"};
  for (int i = 0; i < 3; ++i) {
    Rule cover = MakeRule("cover-" + std::to_string(i), RuleOutcome::Compatible);
    cover.priority = RulePriority(10);
    cover.left.kind = NicKind();
    cover.left.version = VersionRange::Parse(windows[i]).value();
    cover.right.kind = NicKind();
    document.rules.push_back(cover);
  }
  Rule covered = MakeRule("covered", RuleOutcome::Compatible);
  covered.priority = RulePriority(0);
  covered.left.kind = NicKind();
  covered.right.kind = NicKind();
  document.rules.push_back(covered);
  document.Normalize();

  ValidationOptions options;
  options.max_reachability_candidates = 1;
  const ValidationReport report = ValidateRegistryDocument(document, options);
  CHECK_FALSE(report.reachability_exhaustive);
  bool skipped = false;
  for (const Diagnostic& diagnostic : report.diagnostics) {
    if (diagnostic.code == DiagnosticCode::ReachabilityAnalysisSkipped) skipped = true;
  }
  CHECK(skipped);
  // With an adequate budget the same rule set is proven to have a dead rule.
  ValidationOptions full;
  const ValidationReport full_report = ValidateRegistryDocument(document, full);
  CHECK(full_report.reachability_exhaustive);
  bool unreachable = false;
  for (const Diagnostic& diagnostic : full_report.diagnostics) {
    if (diagnostic.code == DiagnosticCode::UnreachableRule) unreachable = true;
  }
  CHECK(unreachable);
}

FCR_TEST(hardening, a_response_larger_than_the_frame_budget_is_refused) {
  auto authority = RegistryAuthority::OpenInMemory(ManyMatchingRules(200));
  CHECK_OK(authority);
  ServerOptions options;
  options.worker_threads = 1;
  options.max_frame_bytes = 4096;
  auto server = RegistryServer::Start(authority.value(), options);
  CHECK_OK(server);
  ClientOptions client_options;
  client_options.port = server.value()->port();
  client_options.max_frame_bytes = 4096;
  auto client = RegistryClient::Connect(client_options);
  CHECK_OK(client);
  JsonValue request = JsonValue::Obj();
  JsonValue nic = JsonValue::Obj();
  nic.Set("family", JsonValue::Str("fabric.transport"));
  nic.Set("kind", JsonValue::Str("rdma.nic"));
  nic.Set("version", JsonValue::Str("1.0.0"));
  request.Set("op", JsonValue::Str("query_pair"));
  request.Set("left", nic);
  request.Set("right", nic);
  auto result = client.value()->CallResult(request);
  CHECK_FALSE(result.has_value());
  CHECK_EQ(result.error().code, ErrorCode::LimitExceeded);
  // The connection remains usable afterwards.
  CHECK_OK(client.value()->Ping());
  client.value()->Close();
  server.value()->Shutdown();
}

FCR_TEST(hardening, cli_reports_a_missing_service_clearly) {
  const ProcessResult result =
      RunProcess(FCR_FCRCTL_PATH, {"status", "--endpoint", "127.0.0.1:1"}, "");
  CHECK(result.exit_code != 0);
  CHECK(result.standard_output.find("transport_error") != std::string::npos ||
        result.standard_output.find("cannot connect") != std::string::npos);
}

FCR_TEST(hardening, repeated_publish_and_query_over_one_connection) {
  const std::string directory = UniqueTempDirectory("repeat-publish");
  AuthorityOptions authority_options;
  authority_options.store.directory = directory;
  authority_options.store.mode = OpenMode::ReadWrite;
  auto authority = RegistryAuthority::Open(authority_options);
  CHECK_OK(authority);
  ServerOptions options;
  options.worker_threads = 2;
  auto server = RegistryServer::Start(authority.value(), options);
  CHECK_OK(server);
  ClientOptions client_options;
  client_options.port = server.value()->port();
  auto client = RegistryClient::Connect(client_options);
  CHECK_OK(client);
  JsonValue document = RegistryDocumentToJson(MakeStandardDocument());
  for (std::uint64_t generation = 0; generation < 25; ++generation) {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("publish"));
    request.Set("document", document);
    request.Set("expected_generation", JsonValue::UInt(generation));
    auto published = client.value()->CallResult(request);
    CHECK_OK(published);
    CHECK_EQ(published.value().Find("generation")->Find("number")->AsUInt().value(),
             generation + 1);
  }
  // The stale fence is still refused after a long session.
  JsonValue stale = JsonValue::Obj();
  stale.Set("op", JsonValue::Str("publish"));
  stale.Set("document", document);
  stale.Set("expected_generation", JsonValue::UInt(0));
  CHECK_ERR(client.value()->CallResult(stale), ErrorCode::StaleGeneration);
  client.value()->Close();
  server.value()->Shutdown();
  const ServerStats stats = server.value()->stats();
  CHECK_EQ(stats.completed_requests, std::uint64_t{25});
  CHECK_EQ(stats.failed_requests, std::uint64_t{1});
  CHECK_EQ(stats.active_connections, std::size_t{0});
  RemoveDirectoryQuietly(directory);
}
