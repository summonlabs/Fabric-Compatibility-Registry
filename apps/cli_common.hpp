// Fabric Compatibility Registry - Summon Software Labs
// Shared plumbing for the command line tools.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fcr/fcr.hpp"

namespace fcr::cli {

// Exit codes are part of the tool contract.
//   0  the command completed and answered affirmatively
//   1  the command failed (usage, IO, typed runtime error)
//   2  the command completed and the answer is negative (not publishable,
//      incompatible)
//   3  the command completed and the answer is UNKNOWN
inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;
inline constexpr int kExitNegative = 2;
inline constexpr int kExitUnknown = 3;

struct Options {
  std::filesystem::path data_dir;
  std::string endpoint;
  bool json = false;
  bool quiet = false;
  bool read_write = false;
  std::string client_id = "fcrctl";
};

Status ParseEndpoint(const std::string& text, std::string* host, std::uint16_t* port);
std::string DefaultDataDirectory();

Result<std::string> ReadTextFile(const std::filesystem::path& path, std::uint64_t max_bytes);
Result<JsonValue> LoadJsonFile(const std::filesystem::path& path, std::uint64_t max_bytes);

// A session is either an in-process authority (loaded from a data directory) or
// a connection to a running fcr-registryd. Both speak the same protocol layer,
// so the command implementations are identical.
class Session {
 public:
  static Result<std::unique_ptr<Session>> Open(const Options& options);
  ~Session();

  bool remote() const { return client_ != nullptr; }
  Result<JsonValue> Call(const JsonValue& request);
  const std::optional<HandshakeInfo>& handshake() const { return handshake_; }

 private:
  Session() = default;
  std::unique_ptr<RegistryClient> client_;
  std::shared_ptr<RegistryAuthority> authority_;
  RequestContext context_;
  std::optional<HandshakeInfo> handshake_;
};

void PrintError(const Error& error);
void PrintJson(const JsonValue& value, bool pretty);

}  // namespace fcr::cli
