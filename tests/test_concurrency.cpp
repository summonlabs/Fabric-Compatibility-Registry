#include <atomic>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fixtures.hpp"

using namespace fcr;
using namespace fcr::test;
using fcr::test::Rng;

namespace {

std::shared_ptr<RegistryAuthority> InMemoryAuthority() {
  auto authority = RegistryAuthority::OpenInMemory(MakeStandardDocument());
  CHECK_OK(authority);
  return authority.value();
}

ComponentSpec NicWithRoce(const std::string& instance, const std::string& version) {
  ComponentSpec spec = MakeNic(instance, version);
  CHECK_OK(spec.capabilities.Set(RoceCapability(), CapabilityValue::Flag(true)));
  CHECK_OK(spec.protocols.Set(RdmaProtocol(), {SemVersion(2, 0, 0), SemVersion(2, 1, 0)}));
  return spec;
}

}  // namespace

FCR_TEST(concurrency, concurrent_queries_are_consistent) {
  auto authority = InMemoryAuthority();
  std::atomic<int> mismatches{0};
  std::atomic<int> completed{0};
  const ComponentSpec left = NicWithRoce("nic-a", "2.4.1");
  const ComponentSpec right = NicWithRoce("nic-b", "2.4.1");
  const Decision reference = authority->QueryPair(left, right).value();
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 200; ++i) {
        auto decision = authority->QueryPair(left, right);
        if (!decision.has_value()) {
          mismatches.fetch_add(1);
          continue;
        }
        if (!(decision.value().id == reference.id) ||
            decision.value().outcome != reference.outcome) {
          mismatches.fetch_add(1);
        }
        completed.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  CHECK_EQ(mismatches.load(), 0);
  CHECK_EQ(completed.load(), 1600);
}

FCR_TEST(concurrency, readers_observe_only_whole_generations) {
  auto authority = InMemoryAuthority();
  std::atomic<bool> stop{false};
  std::atomic<int> observations{0};
  std::atomic<int> torn{0};
  constexpr int kReaders = 4;
  // A latch, not a spin or a timeout: publication starts only once every
  // reader has completed one full observation.
  std::latch first_observation(kReaders);
  std::vector<std::thread> readers;
  for (int t = 0; t < kReaders; ++t) {
    readers.emplace_back([&] {
      bool announced = false;
      while (!stop.load()) {
        auto snapshot = authority->Snapshot();
        if (!snapshot.has_value()) {
          torn.fetch_add(1);
          continue;
        }
        // Every snapshot is internally consistent: the id digest is the digest
        // of the document it carries.
        if (snapshot.value()->id().digest != snapshot.value()->document().Digest()) {
          torn.fetch_add(1);
        }
        observations.fetch_add(1);
        if (!announced) {
          announced = true;
          first_observation.count_down();
        }
      }
    });
  }
  first_observation.wait();
  // The readers are always released, including on the failure path, so a
  // broken expectation can never leave joined resources behind.
  bool published_all = true;
  std::string publish_error;
  try {
    for (int i = 0; i < 30; ++i) {
      RegistryDocumentBuilder builder = MakeStandardBuilder();
      builder.SetLabel("publication", std::to_string(i));
      PublishRequest request;
      request.expected_generation = GenerationNumber(static_cast<std::uint64_t>(i + 1));
      auto published = authority->Publish(builder.Build(), request);
      if (!published.has_value()) {
        published_all = false;
        publish_error = published.error().ToString();
        break;
      }
    }
  } catch (...) {
    stop.store(true);
    for (std::thread& thread : readers) thread.join();
    throw;
  }
  stop.store(true);
  for (std::thread& thread : readers) thread.join();
  if (!published_all) FCR_FAIL("publication failed: " << publish_error);
  CHECK_EQ(torn.load(), 0);
  CHECK(observations.load() > 0);
}

FCR_TEST(concurrency, racing_publishers_produce_exactly_one_winner) {
  auto authority = InMemoryAuthority();
  constexpr int kRacers = 8;
  std::atomic<int> successes{0};
  std::atomic<int> stale{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kRacers; ++i) {
    threads.emplace_back([&, i] {
      RegistryDocumentBuilder builder = MakeStandardBuilder();
      builder.SetLabel("racer", std::to_string(i));
      PublishRequest request;
      request.expected_generation = GenerationNumber(1);
      auto published = authority->Publish(builder.Build(), request);
      if (published.has_value()) {
        successes.fetch_add(1);
      } else if (published.error().code == ErrorCode::StaleGeneration) {
        stale.fetch_add(1);
      } else {
        other.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  CHECK_EQ(successes.load(), 1);
  CHECK_EQ(stale.load(), kRacers - 1);
  CHECK_EQ(other.load(), 0);
  auto status = authority->GetStatus();
  CHECK_OK(status);
  CHECK_EQ(status.value().generation.number.value(), 2u);
}

FCR_TEST(concurrency, cancellation_never_publishes) {
  auto authority = InMemoryAuthority();
  CancellationToken token;
  token.Cancel();
  RegistryDocumentBuilder builder = MakeStandardBuilder();
  builder.SetLabel("cancelled", "true");
  PublishRequest request;
  request.expected_generation = GenerationNumber(1);
  request.token = token;
  auto published = authority->Publish(builder.Build(), request);
  CHECK_FALSE(published.has_value());
  CHECK_EQ(published.error().code, ErrorCode::Cancelled);
  auto current = authority->CurrentGeneration();
  CHECK_OK(current);
  CHECK_EQ(current.value().number.value(), 1u);
}

FCR_TEST(concurrency, server_lifecycle_repeats_cleanly) {
  auto authority = InMemoryAuthority();
  for (int round = 0; round < 6; ++round) {
    ServerOptions options;
    options.worker_threads = 2;
    options.max_connections = 8;
    auto server = RegistryServer::Start(authority, options);
    CHECK_OK(server);
    CHECK(server.value()->port() != 0);
    ClientOptions client_options;
    client_options.port = server.value()->port();
    auto client = RegistryClient::Connect(client_options);
    CHECK_OK(client);
    CHECK_OK(client.value()->Ping());
    CHECK_OK(client.value()->Ping());
    client.value()->Close();
    server.value()->Shutdown();
    const ServerStats stats = server.value()->stats();
    CHECK_EQ(stats.handshakes, std::uint64_t{1});
    CHECK_EQ(stats.completed_requests, std::uint64_t{2});
    CHECK_EQ(stats.active_connections, std::size_t{0});
    CHECK_EQ(stats.queued_connections, std::size_t{0});
  }
}

FCR_TEST(concurrency, shutdown_wakes_idle_connections_and_workers) {
  auto authority = InMemoryAuthority();
  ServerOptions options;
  options.worker_threads = 3;
  auto server = RegistryServer::Start(authority, options);
  CHECK_OK(server);
  std::vector<std::unique_ptr<RegistryClient>> clients;
  for (int i = 0; i < 3; ++i) {
    ClientOptions client_options;
    client_options.port = server.value()->port();
    auto client = RegistryClient::Connect(client_options);
    CHECK_OK(client);
    clients.push_back(std::move(client).value());
  }
  // No further traffic: the workers are blocked in receive. Shutdown must still
  // return because it shuts the live sockets down.
  server.value()->Shutdown();
  const ServerStats stats = server.value()->stats();
  CHECK_EQ(stats.active_connections, std::size_t{0});
  CHECK_EQ(stats.closed_connections, std::uint64_t{3});
  for (auto& client : clients) client->Close();
}

FCR_TEST(concurrency, concurrent_clients_share_one_authority) {
  auto authority = InMemoryAuthority();
  ServerOptions options;
  options.worker_threads = 4;
  auto server = RegistryServer::Start(authority, options);
  CHECK_OK(server);
  std::atomic<int> ok{0};
  std::atomic<int> failed{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t) {
    threads.emplace_back([&] {
      ClientOptions client_options;
      client_options.port = server.value()->port();
      auto client = RegistryClient::Connect(client_options);
      if (!client.has_value()) {
        failed.fetch_add(1);
        return;
      }
      for (int i = 0; i < 20; ++i) {
        JsonValue request = JsonValue::Obj();
        request.Set("op", JsonValue::Str("status"));
        auto result = client.value()->CallResult(request);
        if (result.has_value()) {
          ok.fetch_add(1);
        } else {
          failed.fetch_add(1);
        }
      }
      client.value()->Close();
    });
  }
  for (std::thread& thread : threads) thread.join();
  CHECK_EQ(failed.load(), 0);
  CHECK_EQ(ok.load(), 120);
  server.value()->Shutdown();
}

FCR_TEST(concurrency, connection_limit_is_enforced_without_hanging) {
  auto authority = InMemoryAuthority();
  ServerOptions options;
  // One worker serves one connection at a time, so the admission limit is only
  // reachable without queueing when it does not exceed the worker count.
  options.worker_threads = 2;
  options.max_connections = 2;
  auto server = RegistryServer::Start(authority, options);
  CHECK_OK(server);
  std::vector<std::unique_ptr<RegistryClient>> held;
  for (int i = 0; i < 2; ++i) {
    ClientOptions client_options;
    client_options.port = server.value()->port();
    auto client = RegistryClient::Connect(client_options);
    CHECK_OK(client);
    held.push_back(std::move(client).value());
  }
  ClientOptions extra_options;
  extra_options.port = server.value()->port();
  auto extra = RegistryClient::Connect(extra_options);
  CHECK_FALSE(extra.has_value());
  server.value()->Shutdown();
  for (auto& client : held) client->Close();
}

FCR_TEST(concurrency, repeated_authority_open_close_cycles) {
  const std::string directory = fcr::test::UniqueTempDirectory("cycles");
  for (int round = 0; round < 8; ++round) {
    AuthorityOptions options;
    options.store.directory = directory;
    options.store.mode = OpenMode::ReadWrite;
    auto authority = RegistryAuthority::Open(options);
    CHECK_OK(authority);
    PublishRequest request;
    request.expected_generation = GenerationNumber(static_cast<std::uint64_t>(round));
    RegistryDocumentBuilder builder = MakeStandardBuilder();
    builder.SetLabel("cycle", std::to_string(round));
    auto published = authority.value()->Publish(builder.Build(), request);
    CHECK_OK(published);
    auto read_only = RegistryAuthority::Open([&] {
      AuthorityOptions read_options = options;
      read_options.store.mode = OpenMode::ReadOnly;
      return read_options;
    }());
    CHECK_OK(read_only);
    auto current = read_only.value()->CurrentGeneration();
    CHECK_OK(current);
    CHECK_EQ(current.value().number.value(), static_cast<std::uint64_t>(round + 1));
  }
  RemoveDirectoryQuietly(directory);
}
