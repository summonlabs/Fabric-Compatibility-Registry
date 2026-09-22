// Fabric Compatibility Registry - Summon Software Labs
// Service protocol: canonical JSON request/response payloads carried by frames.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "fcr/authority.hpp"
#include "fcr/cancel.hpp"
#include "fcr/error.hpp"
#include "fcr/json.hpp"

namespace fcr {

inline constexpr std::uint32_t kServiceProtocolVersion = 1;
inline constexpr std::string_view kServiceName = "fcr-registry";

enum class Operation : std::uint8_t {
  Ping,
  Status,
  Validate,
  Publish,
  QueryPair,
  QuerySet,
  Diff,
  ReplayDiff,
  Provenance,
  Generations,
  Prune,
  Shutdown,
};

std::string_view OperationName(Operation operation);
Result<Operation> ParseOperation(std::string_view text);

// Everything a request handler may touch. The handler never runs while a
// server-internal lock is held.
struct RequestContext {
  std::shared_ptr<RegistryAuthority> authority;
  // Cancelled when the service begins shutting down; long operations observe
  // it before their commit point.
  CancellationToken shutdown_token;
  // Installed by the server; invoked for the shutdown operation.
  std::function<void()> request_shutdown;
  bool allow_shutdown = false;
  bool allow_publish = true;
  bool allow_prune = true;
};

JsonValue ErrorToJson(const Error& error);
JsonValue OkResponse(JsonValue result);
JsonValue ErrorResponse(const Error& error);

// Executes one request. Never throws; every failure path returns a typed error.
Result<JsonValue> ExecuteRequest(const JsonValue& request, RequestContext& context);

// Executes a request and wraps the outcome in the protocol envelope.
JsonValue DispatchEnvelope(const JsonValue& request, RequestContext& context);

JsonValue AuthorityStatusToJson(const AuthorityStatus& status);

}  // namespace fcr
