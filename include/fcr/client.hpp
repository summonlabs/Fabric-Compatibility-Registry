// Fabric Compatibility Registry - Summon Software Labs
// Client for the registry service.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "fcr/error.hpp"
#include "fcr/identity.hpp"
#include "fcr/json.hpp"
#include "fcr/transport.hpp"

namespace fcr {

inline constexpr std::uint32_t kClientProtocolVersion = 1;

struct ClientOptions {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;
  std::uint32_t max_frame_bytes = kDefaultMaxFrameBytes;
  std::uint32_t protocol_version = kClientProtocolVersion;
  std::string client_id = "fcr-client";
};

struct HandshakeInfo {
  std::uint32_t protocol_version = 0;
  std::uint32_t max_frame_bytes = 0;
  Incarnation incarnation;
  std::optional<GenerationId> generation;
  std::string registry_name;
  bool persistent = false;
  PublisherEpoch epoch;
};

// One request at a time per connection. Calls are serialised internally, so a
// client may be shared between threads; the lock is never held while a caller
// inspects the result.
class RegistryClient {
 public:
  static Result<std::unique_ptr<RegistryClient>> Connect(const ClientOptions& options);

  RegistryClient(const RegistryClient&) = delete;
  RegistryClient& operator=(const RegistryClient&) = delete;
  ~RegistryClient();

  const HandshakeInfo& handshake() const { return handshake_; }

  // Returns the raw protocol envelope.
  Result<JsonValue> Call(const JsonValue& request);
  // Returns the unwrapped result, or the server-side typed error.
  Result<JsonValue> CallResult(const JsonValue& request);

  Result<JsonValue> Ping();
  void Close();

 private:
  RegistryClient() = default;

  ClientOptions options_;
  HandshakeInfo handshake_;
  Socket socket_;
  std::uint64_t correlation_ = 0;
  std::mutex mutex_;
  bool closed_ = false;
};

}  // namespace fcr
