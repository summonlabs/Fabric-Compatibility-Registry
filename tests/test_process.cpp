#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fixtures.hpp"
#include "process_helper.hpp"

using namespace fcr;
using namespace fcr::test;

namespace {

struct Daemon {
  std::unique_ptr<ChildProcess> process;
  std::string port;
  std::string directory;
};

Daemon StartDaemon(const std::string& directory) {
  Daemon daemon;
  daemon.directory = directory;
  auto child = ChildProcess::Spawn(FCR_REGISTRYD_PATH,
                                   {"--data-dir", directory, "--workers", "2",
                                    "--max-connections", "16"},
                                   "", true);
  CHECK_OK(child);
  daemon.process = std::move(child).value();
  auto line = daemon.process->ReadLine();
  if (!line.has_value()) {
    FCR_FAIL("registry daemon did not announce readiness: " << line.error().ToString());
  }
  const std::string text = line.value();
  if (text.compare(0, 6, "READY ") != 0) {
    FCR_FAIL("unexpected daemon greeting: " << text);
  }
  const std::size_t colon = text.rfind(':');
  CHECK(colon != std::string::npos);
  daemon.port = text.substr(colon + 1);
  return daemon;
}

std::unique_ptr<RegistryClient> ConnectTo(const std::string& port) {
  ClientOptions options;
  options.port = static_cast<std::uint16_t>(std::stoi(port));
  options.client_id = "process-test";
  auto client = RegistryClient::Connect(options);
  CHECK_OK(client);
  return std::move(client).value();
}

JsonValue StatusRequest() {
  JsonValue request = JsonValue::Obj();
  request.Set("op", JsonValue::Str("status"));
  return request;
}

JsonValue MakePublishJson(std::uint64_t expected) {
  JsonValue request = JsonValue::Obj();
  request.Set("op", JsonValue::Str("publish"));
  request.Set("document", RegistryDocumentToJson(MakeStandardDocument()));
  request.Set("expected_generation", JsonValue::UInt(expected));
  return request;
}

// Asks the daemon to stop through the protocol and waits for it. The service
// keeps running until it is told to stop, so a caller must not simply wait.
ProcessResult StopDaemon(Daemon* daemon) {
  {
    auto client = ConnectTo(daemon->port);
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("shutdown"));
    (void)client->CallResult(request);
    client->Close();
  }
  return daemon->process->Wait();
}

std::uint64_t HeadNumber(RegistryClient* client) {
  auto status = client->CallResult(StatusRequest());
  CHECK_OK(status);
  const JsonValue* generation = status.value().Find("generation");
  if (generation == nullptr || !generation->IsObject()) return 0;
  return generation->Find("number")->AsUInt().value();
}

}  // namespace

FCR_TEST(process, independent_daemon_serves_publish_and_query) {
  const std::string directory = UniqueTempDirectory("daemon");
  Daemon daemon = StartDaemon(directory);
  {
    auto client = ConnectTo(daemon.port);
    CHECK_EQ(HeadNumber(client.get()), 0u);
    auto published = client->CallResult(MakePublishJson(0));
    CHECK_OK(published);
    CHECK_EQ(published.value().Find("generation")->Find("number")->AsUInt().value(), 1u);
    JsonValue query = JsonValue::Obj();
    JsonValue nic = JsonValue::Obj();
    nic.Set("family", JsonValue::Str("fabric.transport"));
    nic.Set("kind", JsonValue::Str("rdma.nic"));
    nic.Set("version", JsonValue::Str("2.4.1"));
    nic.Set("capabilities", JsonValue::Obj());
    nic.Set("protocols", JsonValue::Obj());
    query.Set("op", JsonValue::Str("query_pair"));
    query.Set("left", nic);
    query.Set("right", nic);
    auto decision = client->CallResult(query);
    CHECK_OK(decision);
    CHECK(decision.value().Find("outcome") != nullptr);
    client->Close();
  }
  const ProcessResult result = StopDaemon(&daemon);
  CHECK_EQ(result.exit_code, 0);
  CHECK(result.standard_output.find("STOPPED") != std::string::npos);
  CHECK(std::filesystem::exists(std::filesystem::path(directory) / "registry.meta"));
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(process, hard_kill_and_restart_changes_the_incarnation_and_fences_clients) {
  const std::string directory = UniqueTempDirectory("kill-restart");
  Incarnation first_incarnation;
  {
    Daemon daemon = StartDaemon(directory);
    auto client = ConnectTo(daemon.port);
    first_incarnation = client->handshake().incarnation;
    CHECK(first_incarnation.value() > 0);
    CHECK_OK(client->CallResult(MakePublishJson(0)));
    CHECK_OK(client->CallResult(MakePublishJson(1)));
    client->Close();
    const ProcessResult killed = daemon.process->Kill();
    CHECK_EQ(killed.exit_code, 137);
  }
  {
    Daemon daemon = StartDaemon(directory);
    auto client = ConnectTo(daemon.port);
    const Incarnation second_incarnation = client->handshake().incarnation;
    CHECK(second_incarnation.value() > first_incarnation.value());
    // The data survived the hard kill.
    CHECK_EQ(HeadNumber(client.get()), 2u);
    // A caller that presents the dead incarnation is fenced.
    JsonValue request = StatusRequest();
    JsonValue stamp = JsonValue::Obj();
    stamp.Set("incarnation", JsonValue::UInt(first_incarnation.value()));
    if (client->handshake().generation.has_value()) {
      stamp.Set("generation", JsonValue::Str(client->handshake().generation->ToString()));
    }
    request.Set("stamp", std::move(stamp));
    auto fenced = client->CallResult(request);
    CHECK_FALSE(fenced.has_value());
    CHECK_EQ(fenced.error().code, ErrorCode::StaleIncarnation);
    // The fresh stamp is accepted.
    JsonValue fresh_request = StatusRequest();
    JsonValue fresh_stamp = JsonValue::Obj();
    fresh_stamp.Set("incarnation", JsonValue::UInt(second_incarnation.value()));
    if (client->handshake().generation.has_value()) {
      fresh_stamp.Set("generation", JsonValue::Str(client->handshake().generation->ToString()));
    }
    fresh_request.Set("stamp", std::move(fresh_stamp));
    CHECK_OK(client->CallResult(fresh_request));
    client->Close();
    daemon.process->Kill();
  }
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(process, a_second_writer_process_cannot_take_the_directory) {
  const std::string directory = UniqueTempDirectory("second-writer");
  Daemon daemon = StartDaemon(directory);
  {
    const ProcessResult result =
        RunProcess(FCR_FCRCTL_PATH, {"publish", "nonexistent.json", "--data-dir", directory}, "");
    CHECK(result.exit_code != 0);
  }
  {
    // Write mode from a second process is refused by the directory lock.
    AuthorityOptions options;
    options.store.directory = directory;
    options.store.mode = OpenMode::ReadWrite;
    auto authority = RegistryAuthority::Open(options);
    CHECK_FALSE(authority.has_value());
    CHECK_EQ(authority.error().code, ErrorCode::LockContention);
    AuthorityOptions read_options = options;
    read_options.store.mode = OpenMode::ReadOnly;
    auto reader = RegistryAuthority::Open(read_options);
    CHECK_OK(reader);
  }
  daemon.process->Kill();
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(process, racing_publishers_from_separate_processes_produce_one_winner) {
  const std::string directory = UniqueTempDirectory("race");
  Daemon daemon = StartDaemon(directory);
  {
    auto client = ConnectTo(daemon.port);
    CHECK_OK(client->CallResult(MakePublishJson(0)));
    client->Close();
  }
  const std::string document_path = directory + "\\document.json";
  CHECK(WriteTextFile(document_path, RegistryDocumentToJson(MakeStandardDocument()).Dump(false))
            .ok());
  const std::vector<std::string> arguments = {"publish", document_path, "--endpoint",
                                              "127.0.0.1:" + daemon.port, "--expected-generation",
                                              "1"};
  auto first = ChildProcess::Spawn(FCR_FCRCTL_PATH, arguments, "", true);
  CHECK_OK(first);
  auto second = ChildProcess::Spawn(FCR_FCRCTL_PATH, arguments, "", true);
  CHECK_OK(second);
  const ProcessResult first_result = first.value()->Wait();
  const ProcessResult second_result = second.value()->Wait();
  const int successes = (first_result.exit_code == 0 ? 1 : 0) + (second_result.exit_code == 0 ? 1 : 0);
  CHECK_EQ(successes, 1);
  const ProcessResult loser = first_result.exit_code == 0 ? second_result : first_result;
  CHECK_EQ(loser.exit_code, 2);
  CHECK(loser.standard_output.find("stale_generation") != std::string::npos);
  {
    auto client = ConnectTo(daemon.port);
    CHECK_EQ(HeadNumber(client.get()), 2u);
    auto generations = client->CallResult([] {
      JsonValue request = JsonValue::Obj();
      request.Set("op", JsonValue::Str("generations"));
      return request;
    }());
    CHECK_OK(generations);
    CHECK_EQ(generations.value().Find("generations")->AsArray()->size(), std::size_t{2});
    client->Close();
  }
  daemon.process->Kill();
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(process, daemon_survives_repeated_client_churn) {
  const std::string directory = UniqueTempDirectory("churn");
  Daemon daemon = StartDaemon(directory);
  for (int round = 0; round < 12; ++round) {
    auto client = ConnectTo(daemon.port);
    CHECK_OK(client->Ping());
    CHECK_OK(client->CallResult(StatusRequest()));
    client->Close();
  }
  const ProcessResult result = StopDaemon(&daemon);
  CHECK_EQ(result.exit_code, 0);
  CHECK(result.standard_output.find("accepted=13") != std::string::npos);
  RemoveDirectoryQuietly(directory);
}
