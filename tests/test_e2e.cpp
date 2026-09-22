#include <memory>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "process_helper.hpp"

using namespace fcr;
using namespace fcr::test;

namespace {

const char* kNicSpec =
    "{\"instance\":\"nic-a\",\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\","
    "\"version\":\"2.4.1\",\"knowledge\":{\"capabilities\":\"closed\",\"protocols\":\"closed\","
    "\"schemas\":\"closed\"},\"capabilities\":{\"rdma.rocev2\":true},"
    "\"protocols\":{\"fabric.rdma\":[\"2.0.0\",\"2.1.0\"]}}";

const char* kLegacyNicSpec =
    "{\"instance\":\"nic-legacy\",\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\","
    "\"version\":\"1.5.0\",\"knowledge\":{\"capabilities\":\"closed\",\"protocols\":\"closed\","
    "\"schemas\":\"closed\"},\"capabilities\":{},\"protocols\":{}}";

const char* kModernNicSpec =
    "{\"instance\":\"nic-modern\",\"family\":\"fabric.transport\",\"kind\":\"rdma.nic\","
    "\"version\":\"3.1.0\",\"knowledge\":{\"capabilities\":\"closed\",\"protocols\":\"closed\","
    "\"schemas\":\"closed\"},\"capabilities\":{},\"protocols\":{}}";

const char* kSwitchSpec =
    "{\"instance\":\"sw-a\",\"family\":\"fabric.transport\",\"kind\":\"fabric.switch\","
    "\"version\":\"1.0.0\",\"knowledge\":{\"capabilities\":\"closed\",\"protocols\":\"closed\","
    "\"schemas\":\"closed\"}}";

const char* kOtherSwitchSpec =
    "{\"instance\":\"sw-b\",\"family\":\"fabric.transport\",\"kind\":\"fabric.switch\","
    "\"version\":\"2.0.0\",\"knowledge\":{\"capabilities\":\"closed\",\"protocols\":\"closed\","
    "\"schemas\":\"closed\"}}";

struct Fixture {
  std::string directory;
  std::string document_path;
  std::string nic_path;
  std::string legacy_path;
  std::string modern_path;
  std::string switch_path;
  std::string other_switch_path;
};

Fixture MakeFixture(const std::string& tag) {
  Fixture fixture;
  fixture.directory = UniqueTempDirectory(tag);
  fixture.document_path = fixture.directory + "\\registry.json";
  fixture.nic_path = fixture.directory + "\\nic.json";
  fixture.legacy_path = fixture.directory + "\\legacy.json";
  fixture.modern_path = fixture.directory + "\\modern.json";
  fixture.switch_path = fixture.directory + "\\switch.json";
  fixture.other_switch_path = fixture.directory + "\\switch2.json";
  CHECK(WriteTextFile(fixture.document_path,
                      RegistryDocumentToJson(MakeStandardDocument()).Dump(true))
            .ok());
  CHECK(WriteTextFile(fixture.nic_path, kNicSpec).ok());
  CHECK(WriteTextFile(fixture.legacy_path, kLegacyNicSpec).ok());
  CHECK(WriteTextFile(fixture.modern_path, kModernNicSpec).ok());
  CHECK(WriteTextFile(fixture.switch_path, kSwitchSpec).ok());
  CHECK(WriteTextFile(fixture.other_switch_path, kOtherSwitchSpec).ok());
  return fixture;
}

void Cleanup(const Fixture& fixture) { RemoveDirectoryQuietly(fixture.directory); }

ProcessResult Cli(const std::vector<std::string>& arguments) {
  return RunProcess(FCR_FCRCTL_PATH, arguments, "");
}

}  // namespace

FCR_TEST(e2e, cli_reports_its_version) {
  const ProcessResult result = Cli({"version"});
  CHECK_EQ(result.exit_code, 0);
  CHECK(result.standard_output.find("fcrctl 1.0.0") != std::string::npos);
}

FCR_TEST(e2e, cli_validates_documents_with_meaningful_exit_codes) {
  const Fixture fixture = MakeFixture("e2e-validate");
  {
    const ProcessResult result = Cli({"validate", fixture.document_path});
    CHECK_EQ(result.exit_code, 0);
    CHECK(result.standard_output.find("errors:   0") != std::string::npos);
  }
  {
    // A rule referencing an undefined capability cannot be published.
    RegistryDocument broken = MakeStandardDocument();
    broken.rules.clear();
    Rule rule = MakeRule("broken", RuleOutcome::Compatible);
    rule.left.kind = NicKind();
    rule.right.kind = NicKind();
    JsonValue document = RegistryDocumentToJson(broken);
    JsonValue rules = JsonValue::Arr();
    JsonValue entry = JsonValue::Obj();
    entry.Set("id", JsonValue::Str("broken"));
    entry.Set("outcome", JsonValue::Str("compatible"));
    JsonValue selector = JsonValue::Obj();
    selector.Set("kind", JsonValue::Str("rdma.nic"));
    entry.Set("left", selector);
    entry.Set("right", selector);
    JsonValue constraint = JsonValue::Obj();
    constraint.Set("type", JsonValue::Str("requires_capability"));
    constraint.Set("side", JsonValue::Str("left"));
    constraint.Set("capability", JsonValue::Str("ghost.capability"));
    JsonValue::Array constraints;
    constraints.push_back(constraint);
    entry.Set("constraints", JsonValue::Arr(std::move(constraints)));
    JsonValue provenance = JsonValue::Obj();
    provenance.Set("publisher", JsonValue::Str("ops"));
    entry.Set("provenance", std::move(provenance));
    rules.Push(std::move(entry));
    document.Set("rules", std::move(rules));
    const std::string path = fixture.directory + "\\broken.json";
    CHECK(WriteTextFile(path, document.Dump(false)).ok());
    const ProcessResult result = Cli({"validate", path});
    CHECK_EQ(result.exit_code, 2);
    CHECK(result.standard_output.find("undefined_capability") != std::string::npos);
  }
  {
    const ProcessResult result = Cli({"validate", fixture.directory + "\\missing.json"});
    CHECK_EQ(result.exit_code, 1);
  }
  Cleanup(fixture);
}

FCR_TEST(e2e, cli_publishes_and_reports_generations) {
  const Fixture fixture = MakeFixture("e2e-publish");
  {
    const ProcessResult result = Cli({"publish", fixture.document_path, "--data-dir",
                                      fixture.directory, "--expected-generation", "0"});
    CHECK_EQ(result.exit_code, 0);
    CHECK(result.standard_output.find("published gen-1:") != std::string::npos);
  }
  {
    const ProcessResult result = Cli({"generations", "--data-dir", fixture.directory});
    CHECK_EQ(result.exit_code, 0);
    CHECK_EQ(result.standard_output, std::string("1\n"));
  }
  {
    const ProcessResult result = Cli({"status", "--data-dir", fixture.directory});
    CHECK_EQ(result.exit_code, 0);
    CHECK(result.standard_output.find("rules:") != std::string::npos);
    CHECK(result.standard_output.find("gen-1:") != std::string::npos);
  }
  {
    const ProcessResult result = Cli({"status", "--data-dir", fixture.directory, "--json"});
    CHECK_EQ(result.exit_code, 0);
    auto json = ParseJson(result.standard_output);
    CHECK_OK(json);
    CHECK(json.value().Find("generation") != nullptr);
  }
  {
    // A stale expected generation is refused.
    const ProcessResult result = Cli({"publish", fixture.document_path, "--data-dir",
                                      fixture.directory, "--expected-generation", "0", "--quiet"});
    CHECK_EQ(result.exit_code, 2);
  }
  Cleanup(fixture);
}

FCR_TEST(e2e, cli_query_exit_codes_distinguish_unknown_from_compatible) {
  const Fixture fixture = MakeFixture("e2e-query");
  CHECK_EQ(Cli({"publish", fixture.document_path, "--data-dir", fixture.directory,
                "--expected-generation", "0", "--quiet"})
               .exit_code,
           0);
  {
    const ProcessResult result = Cli({"query", "pair", "--left", fixture.nic_path, "--right",
                                      fixture.nic_path, "--data-dir", fixture.directory});
    CHECK_EQ(result.exit_code, 0);
    CHECK(result.standard_output.find("outcome: compatible") != std::string::npos);
    CHECK(result.standard_output.find("negotiation:") != std::string::npos);
    CHECK(result.standard_output.find("fabric.rdma") != std::string::npos);
  }
  {
    const ProcessResult result =
        Cli({"query", "pair", "--left", fixture.legacy_path, "--right", fixture.modern_path,
             "--data-dir", fixture.directory});
    CHECK_EQ(result.exit_code, 2);
    CHECK(result.standard_output.find("outcome: incompatible") != std::string::npos);
    CHECK(result.standard_output.find("deciding_rule: nic-legacy-vs-modern@r1") !=
          std::string::npos);
  }
  {
    const ProcessResult result =
        Cli({"query", "pair", "--left", fixture.switch_path, "--right",
             fixture.other_switch_path, "--data-dir", fixture.directory});
    CHECK_EQ(result.exit_code, 3);
    CHECK(result.standard_output.find("outcome: unknown") != std::string::npos);
    CHECK(result.standard_output.find("no_matching_rule") != std::string::npos);
  }
  {
    const ProcessResult result =
        Cli({"query", "pair", "--left", fixture.nic_path, "--right", fixture.nic_path,
             "--data-dir", fixture.directory, "--json"});
    CHECK_EQ(result.exit_code, 0);
    auto json = ParseJson(result.standard_output);
    CHECK_OK(json);
    CHECK(json.value().Find("decision_id") != nullptr);
    CHECK(json.value().Find("matched_rules") != nullptr);
  }
  Cleanup(fixture);
}

FCR_TEST(e2e, cli_reports_diffs_and_provenance) {
  const Fixture fixture = MakeFixture("e2e-diff");
  CHECK_EQ(Cli({"publish", fixture.document_path, "--data-dir", fixture.directory,
                "--expected-generation", "0", "--quiet"})
               .exit_code,
           0);
  {
    RegistryDocument second = MakeStandardDocument();
    for (Rule& rule : second.rules) {
      if (rule.id.value() == "nic-requires-roce") {
        rule.revision = RuleRevision(2);
        rule.provenance.change_note = "tightened";
      }
    }
    Rule extra = MakeRule("gpu-pair", RuleOutcome::Compatible);
    extra.left.kind = GpuKind();
    extra.right.kind = GpuKind();
    second.rules.push_back(extra);
    second.Normalize();
    const std::string path = fixture.directory + "\\second.json";
    CHECK(WriteTextFile(path, RegistryDocumentToJson(second).Dump(true)).ok());
    CHECK_EQ(Cli({"publish", path, "--data-dir", fixture.directory, "--expected-generation", "1",
                  "--quiet"})
                 .exit_code,
             0);
  }
  {
    const ProcessResult result =
        Cli({"diff", "--from", "1", "--to", "2", "--data-dir", fixture.directory});
    CHECK_EQ(result.exit_code, 0);
    CHECK(result.standard_output.find("added:     1") != std::string::npos);
    CHECK(result.standard_output.find("modified:  1") != std::string::npos);
  }
  {
    const ProcessResult result = Cli({"provenance", "--rule", "nic-requires-roce", "--data-dir",
                                      fixture.directory});
    CHECK_EQ(result.exit_code, 0);
    CHECK(result.standard_output.find("nic-requires-roce@r2") != std::string::npos);
    CHECK(result.standard_output.find("gen-2") != std::string::npos);
  }
  Cleanup(fixture);
}

FCR_TEST(e2e, cli_talks_to_a_running_daemon) {
  const Fixture fixture = MakeFixture("e2e-daemon");
  auto child = ChildProcess::Spawn(FCR_REGISTRYD_PATH,
                                   {"--data-dir", fixture.directory, "--workers", "2"}, "", true);
  CHECK_OK(child);
  auto line = child.value()->ReadLine();
  CHECK_OK(line);
  const std::size_t colon = line.value().rfind(':');
  CHECK(colon != std::string::npos);
  const std::string endpoint = "127.0.0.1:" + line.value().substr(colon + 1);

  const ProcessResult publish = Cli({"publish", fixture.document_path, "--endpoint", endpoint,
                                     "--expected-generation", "0"});
  CHECK_EQ(publish.exit_code, 0);
  const ProcessResult query = Cli({"query", "pair", "--left", fixture.nic_path, "--right",
                                   fixture.nic_path, "--endpoint", endpoint});
  CHECK_EQ(query.exit_code, 0);
  CHECK(query.standard_output.find("outcome: compatible") != std::string::npos);
  const ProcessResult status = Cli({"status", "--endpoint", endpoint});
  CHECK_EQ(status.exit_code, 0);
  CHECK(status.standard_output.find("persistent") != std::string::npos);
  const ProcessResult stale = Cli({"publish", fixture.document_path, "--endpoint", endpoint,
                                   "--expected-generation", "0", "--quiet"});
  CHECK_EQ(stale.exit_code, 2);

  // The service runs until it is asked to stop through the protocol.
  {
    ClientOptions client_options;
    client_options.port = static_cast<std::uint16_t>(std::stoi(line.value().substr(colon + 1)));
    auto client = RegistryClient::Connect(client_options);
    CHECK_OK(client);
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("shutdown"));
    (void)client.value()->CallResult(request);
    client.value()->Close();
  }
  const ProcessResult stopped = child.value()->Wait();
  CHECK_EQ(stopped.exit_code, 0);
  CHECK(stopped.standard_output.find("STOPPED") != std::string::npos);
  Cleanup(fixture);
}
