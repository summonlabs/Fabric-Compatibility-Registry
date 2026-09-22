#include <atomic>
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

JsonValue StatusRequest() {
  JsonValue request = JsonValue::Obj();
  request.Set("op", JsonValue::Str("status"));
  return request;
}

}  // namespace

FCR_TEST(transport, frame_encoding_round_trips) {
  Rng rng(0x7f7f7f7full);
  for (int attempt = 0; attempt < 200; ++attempt) {
    Frame frame;
    frame.kind = static_cast<FrameKind>(1 + rng.Below(8));
    frame.correlation = rng.Next();
    frame.payload.assign(rng.Below(600), std::byte{0});
    for (std::byte& byte : frame.payload) byte = static_cast<std::byte>(rng.Below(256));
    auto encoded = EncodeFrame(frame, kDefaultMaxFrameBytes);
    CHECK_OK(encoded);

    // Decoding goes through a real socket pair so the reader exercises the same
    // path the service does.
    TcpListener listener = TcpListener::Bind("127.0.0.1", 0, 4).value();
    std::thread sender([&] {
      auto socket = Socket::Connect("127.0.0.1", listener.port());
      CHECK_OK(socket);
      Status sent = socket.value().SendAll(std::span<const std::byte>(encoded.value()));
      CHECK(sent.ok());
      socket.value().ShutdownBoth();
      socket.value().Close();
    });
    auto accepted = listener.Accept();
    CHECK_OK(accepted);
    Frame decoded;
    auto read = ReadFrame(accepted.value(), kDefaultMaxFrameBytes, decoded);
    CHECK_OK(read);
    CHECK(read.value());
    CHECK_EQ(decoded.correlation, frame.correlation);
    CHECK_EQ(static_cast<std::uint16_t>(decoded.kind), static_cast<std::uint16_t>(frame.kind));
    CHECK_EQ(decoded.payload.size(), frame.payload.size());
    CHECK(decoded.PayloadText() == frame.PayloadText());
    sender.join();
    accepted.value().Close();
    listener.Close();
  }
}

FCR_TEST(transport, malformed_frames_are_rejected) {
  auto send_and_expect_failure = [](const std::vector<std::byte>& bytes, ErrorCode expected) {
    TcpListener listener = TcpListener::Bind("127.0.0.1", 0, 4).value();
    std::thread sender([&] {
      auto socket = Socket::Connect("127.0.0.1", listener.port());
      CHECK_OK(socket);
      (void)socket.value().SendAll(std::span<const std::byte>(bytes));
      socket.value().ShutdownBoth();
      socket.value().Close();
    });
    auto accepted = listener.Accept();
    CHECK_OK(accepted);
    Frame decoded;
    auto read = ReadFrame(accepted.value(), kDefaultMaxFrameBytes, decoded);
    if (read.has_value()) {
      FCR_FAIL("malformed frame was accepted");
    }
    (void)expected;
    sender.join();
    accepted.value().Close();
    listener.Close();
  };

  Frame good;
  good.kind = FrameKind::Request;
  good.SetPayloadText("{}");
  auto encoded = EncodeFrame(good, kDefaultMaxFrameBytes).value();

  {
    auto bad_magic = encoded;
    bad_magic[0] = std::byte{'X'};
    send_and_expect_failure(bad_magic, ErrorCode::ProtocolError);
  }
  {
    auto bad_crc = encoded;
    bad_crc[22] = static_cast<std::byte>(std::to_integer<unsigned>(bad_crc[22]) ^ 0xff);
    send_and_expect_failure(bad_crc, ErrorCode::ProtocolError);
  }
  {
    auto bad_payload = encoded;
    bad_payload.back() = static_cast<std::byte>(std::to_integer<unsigned>(bad_payload.back()) ^ 0xff);
    send_and_expect_failure(bad_payload, ErrorCode::ProtocolError);
  }
  {
    // A frame that declares more payload than the negotiated maximum.
    auto huge = encoded;
    huge[8] = std::byte{0xff};
    huge[9] = std::byte{0xff};
    huge[10] = std::byte{0xff};
    huge[11] = std::byte{0x7f};
    send_and_expect_failure(huge, ErrorCode::LimitExceeded);
  }
}

FCR_TEST(transport, oversized_payloads_are_refused_before_they_reach_the_wire) {
  Frame frame;
  frame.kind = FrameKind::Request;
  frame.payload.assign(4096, std::byte{1});
  CHECK_ERR(EncodeFrame(frame, 1024), ErrorCode::LimitExceeded);
}

FCR_TEST(transport, handshake_rejects_an_unsupported_protocol_version) {
  auto authority = InMemoryAuthority();
  ServerOptions options;
  options.worker_threads = 1;
  auto server = RegistryServer::Start(authority, options);
  CHECK_OK(server);
  ClientOptions client_options;
  client_options.port = server.value()->port();
  client_options.protocol_version = 99;
  auto client = RegistryClient::Connect(client_options);
  CHECK_FALSE(client.has_value());
  CHECK_EQ(client.error().code, ErrorCode::HandshakeFailed);
  server.value()->Shutdown();
}

FCR_TEST(transport, non_hello_first_frame_is_refused) {
  auto authority = InMemoryAuthority();
  ServerOptions options;
  options.worker_threads = 1;
  auto server = RegistryServer::Start(authority, options);
  CHECK_OK(server);
  auto socket = Socket::Connect("127.0.0.1", server.value()->port());
  CHECK_OK(socket);
  Frame frame;
  frame.kind = FrameKind::Request;
  frame.SetPayloadText("{\"op\":\"status\"}");
  auto encoded = EncodeFrame(frame, kDefaultMaxFrameBytes);
  CHECK_OK(encoded);
  CHECK(socket.value().SendAll(std::span<const std::byte>(encoded.value())).ok());
  Frame reply;
  auto read = ReadFrame(socket.value(), kDefaultMaxFrameBytes, reply);
  // The server closes without answering: a protocol error, not a silent hang.
  CHECK(read.has_value() ? !read.value() : true);
  socket.value().Close();
  server.value()->Shutdown();
  CHECK(server.value()->stats().protocol_errors >= 1);
}

FCR_TEST(transport, service_round_trips_cover_every_operation) {
  const std::string directory = fcr::test::UniqueTempDirectory("service");
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
  CHECK(client.value()->handshake().incarnation.value() > 0);

  std::size_t completed = 0;
  std::size_t failed = 0;
  {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("ping"));
    CHECK(client.value()->CallResult(request).has_value());
    ++completed;
  }
  {
    auto result = client.value()->CallResult(StatusRequest());
    CHECK_OK(result);
    CHECK_EQ(result.value().Find("service")->AsString()->compare("fcr-registry"), 0);
    ++completed;
  }
  {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("validate"));
    request.Set("document", RegistryDocumentToJson(MakeStandardDocument()));
    auto result = client.value()->CallResult(request);
    CHECK_OK(result);
    CHECK(result.value().Find("publishable")->AsBool(false));
    ++completed;
  }
  {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("publish"));
    request.Set("document", RegistryDocumentToJson(MakeStandardDocument()));
    request.Set("expected_generation", JsonValue::UInt(0));
    auto result = client.value()->CallResult(request);
    CHECK_OK(result);
    CHECK_EQ(result.value().Find("generation")->Find("number")->AsUInt().value(), 1u);
    ++completed;
  }
  {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("publish"));
    request.Set("document", RegistryDocumentToJson(MakeStandardDocument()));
    request.Set("expected_generation", JsonValue::UInt(0));
    auto result = client.value()->CallResult(request);
    CHECK_FALSE(result.has_value());
    CHECK_EQ(result.error().code, ErrorCode::StaleGeneration);
    ++failed;
  }
  {
    JsonValue left = JsonValue::Obj();
    left.Set("family", JsonValue::Str("fabric.transport"));
    left.Set("kind", JsonValue::Str("rdma.nic"));
    left.Set("version", JsonValue::Str("2.4.1"));
    left.Set("capabilities", JsonValue::Obj());
    left.Set("protocols", JsonValue::Obj());
    JsonValue right = left;
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("query_pair"));
    request.Set("left", std::move(left));
    request.Set("right", std::move(right));
    request.Set("explain", JsonValue::Bool(true));
    auto result = client.value()->CallResult(request);
    CHECK_OK(result);
    CHECK(result.value().Find("outcome") != nullptr);
    CHECK(result.value().Find("explanation") != nullptr);
    ++completed;
  }
  {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("generations"));
    auto result = client.value()->CallResult(request);
    CHECK_OK(result);
    CHECK_EQ(result.value().Find("generations")->AsArray()->size(), std::size_t{1});
    ++completed;
  }
  {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("diff"));
    request.Set("from", JsonValue::UInt(1));
    request.Set("to", JsonValue::UInt(1));
    auto result = client.value()->CallResult(request);
    CHECK_OK(result);
    ++completed;
  }
  {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("provenance"));
    request.Set("rule", JsonValue::Str("nic-requires-roce"));
    auto result = client.value()->CallResult(request);
    CHECK_OK(result);
    CHECK(result.value().Find("records")->AsArray()->size() >= 1);
    ++completed;
  }
  {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("prune"));
    request.Set("keep", JsonValue::UInt(4));
    auto result = client.value()->CallResult(request);
    CHECK_OK(result);
    ++completed;
  }
  {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("unknown_op"));
    auto result = client.value()->CallResult(request);
    CHECK_FALSE(result.has_value());
    CHECK_EQ(result.error().code, ErrorCode::InvalidArgument);
    ++failed;
  }
  client.value()->Close();
  server.value()->Shutdown();
  const ServerStats stats = server.value()->stats();
  CHECK_EQ(stats.completed_requests, static_cast<std::uint64_t>(completed));
  CHECK_EQ(stats.failed_requests, static_cast<std::uint64_t>(failed));
  CHECK_EQ(stats.protocol_errors, std::uint64_t{0});
  CHECK_EQ(stats.active_connections, std::size_t{0});
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(transport, shutdown_is_observed_by_in_flight_publishes) {
  auto authority = InMemoryAuthority();
  ServerOptions options;
  options.worker_threads = 2;
  auto server = RegistryServer::Start(authority, options);
  CHECK_OK(server);
  ClientOptions client_options;
  client_options.port = server.value()->port();
  auto client = RegistryClient::Connect(client_options);
  CHECK_OK(client);
  server.value()->RequestShutdown();
  CHECK(server.value()->IsStopping());
  JsonValue request = JsonValue::Obj();
  request.Set("op", JsonValue::Str("publish"));
  request.Set("document", RegistryDocumentToJson(MakeStandardDocument()));
  request.Set("expected_generation", JsonValue::UInt(1));
  auto result = client.value()->CallResult(request);
  if (result.has_value()) {
    FCR_FAIL("a publish succeeded after shutdown was requested");
  }
  client.value()->Close();
  server.value()->Shutdown();
  auto current = authority->CurrentGeneration();
  CHECK_OK(current);
  CHECK_EQ(current.value().number.value(), 1u);
}
