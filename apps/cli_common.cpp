#include "cli_common.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace fcr::cli {
namespace {

bool AllDigits(const std::string& text) {
  if (text.empty()) return false;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

}  // namespace

Status ParseEndpoint(const std::string& text, std::string* host, std::uint16_t* port) {
  if (text.empty()) {
    return Fail(ErrorCode::InvalidArgument, "endpoint must not be empty");
  }
  std::string address = text;
  std::string port_text;
  const std::size_t colon = text.rfind(':');
  if (colon != std::string::npos) {
    address = text.substr(0, colon);
    port_text = text.substr(colon + 1);
  } else {
    port_text = text;
    address = "127.0.0.1";
  }
  if (address.empty()) address = "127.0.0.1";
  if (!AllDigits(port_text)) {
    return Fail(ErrorCode::InvalidArgument, "endpoint port must be numeric", text);
  }
  const unsigned long value = std::strtoul(port_text.c_str(), nullptr, 10);
  if (value == 0 || value > 65535) {
    return Fail(ErrorCode::InvalidArgument, "endpoint port is out of range", text);
  }
  *host = address;
  *port = static_cast<std::uint16_t>(value);
  return Status{};
}

std::string ReadEnvironment(const char* name) {
  char* buffer = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) {
    std::string value(buffer);
    free(buffer);
    return value;
  }
  return std::string();
}

std::string DefaultDataDirectory() {
  const std::string from_environment = ReadEnvironment("FCR_DATA_DIR");
  if (!from_environment.empty()) return from_environment;
  return "fcr-data";
}

Result<std::string> ReadTextFile(const std::filesystem::path& path, std::uint64_t max_bytes) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return MakeError(ErrorCode::NotFound, "cannot stat input file", path.string());
  }
  if (size > max_bytes) {
    return MakeError(ErrorCode::LimitExceeded, "input file exceeds the configured size limit",
                     path.string());
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return MakeError(ErrorCode::IoError, "cannot open input file", path.string());
  }
  std::string text(static_cast<std::size_t>(size), '\0');
  if (size != 0) {
    stream.read(text.data(), static_cast<std::streamsize>(size));
    if (!stream) {
      return MakeError(ErrorCode::IoError, "cannot read input file", path.string());
    }
  }
  return text;
}

Result<JsonValue> LoadJsonFile(const std::filesystem::path& path, std::uint64_t max_bytes) {
  auto text = ReadTextFile(path, max_bytes);
  if (!text.has_value()) return text.error();
  JsonLimits limits;
  limits.max_bytes = static_cast<std::size_t>(max_bytes);
  auto json = ParseJson(text.value(), limits);
  if (!json.has_value()) {
    Error error = json.error();
    error.detail = path.string();
    return error;
  }
  return json;
}

Result<std::unique_ptr<Session>> Session::Open(const Options& options) {
  auto session = std::unique_ptr<Session>(new Session());
  if (!options.endpoint.empty()) {
    std::string host;
    std::uint16_t port = 0;
    Status parsed = ParseEndpoint(options.endpoint, &host, &port);
    if (!parsed.ok()) return parsed.error();
    ClientOptions client_options;
    client_options.address = host;
    client_options.port = port;
    client_options.client_id = options.client_id;
    auto client = RegistryClient::Connect(client_options);
    if (!client.has_value()) return client.error();
    session->handshake_ = client.value()->handshake();
    session->client_ = std::move(client).value();
    return session;
  }
  AuthorityOptions authority_options;
  authority_options.store.directory = options.data_dir;
  authority_options.store.mode = options.read_write ? OpenMode::ReadWrite : OpenMode::ReadOnly;
  authority_options.allow_empty = true;
  auto authority = RegistryAuthority::Open(authority_options);
  if (!authority.has_value()) return authority.error();
  session->authority_ = authority.value();
  session->context_.authority = authority.value();
  session->context_.allow_publish = options.read_write;
  session->context_.allow_prune = options.read_write;
  session->context_.allow_shutdown = false;
  return session;
}

Session::~Session() {
  if (client_ != nullptr) client_->Close();
}

Result<JsonValue> Session::Call(const JsonValue& request) {
  if (client_ != nullptr) return client_->CallResult(request);
  JsonValue envelope = DispatchEnvelope(request, context_);
  const JsonValue* ok = envelope.Find("ok");
  if (ok != nullptr && ok->AsBool(false)) {
    const JsonValue* result = envelope.Find("result");
    if (result == nullptr) {
      return MakeError(ErrorCode::Internal, "local dispatch produced no result");
    }
    return *result;
  }
  const JsonValue* error = envelope.Find("error");
  if (error == nullptr) {
    return MakeError(ErrorCode::Internal, "local dispatch produced no error object");
  }
  Error typed;
  const JsonValue* code = error->Find("code");
  if (code != nullptr && code->AsString() != nullptr) {
    typed.code = ErrorCodeFromName(*code->AsString());
  }
  const JsonValue* message = error->Find("message");
  typed.message = message != nullptr && message->AsString() != nullptr ? *message->AsString()
                                                                       : "local dispatch failed";
  const JsonValue* detail = error->Find("detail");
  if (detail != nullptr && detail->AsString() != nullptr) typed.detail = *detail->AsString();
  return typed;
}

void PrintError(const Error& error) {
  std::cerr << "error: " << error.ToString() << "\n";
}

void PrintJson(const JsonValue& value, bool pretty) {
  std::cout << value.Dump(pretty) << "\n";
}

}  // namespace fcr::cli
