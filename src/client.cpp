#include "fcr/client.hpp"

#include "fcr/protocol.hpp"

namespace fcr {
namespace {

Result<JsonValue> UnwrapEnvelope(const JsonValue& envelope) {
  if (!envelope.IsObject()) {
    return MakeError(ErrorCode::ProtocolError, "service reply is not a JSON object");
  }
  const JsonValue* ok = envelope.Find("ok");
  if (ok == nullptr || !ok->IsBool()) {
    return MakeError(ErrorCode::ProtocolError, "service reply has no boolean 'ok' field");
  }
  if (ok->AsBool(false)) {
    const JsonValue* result = envelope.Find("result");
    if (result == nullptr) {
      return MakeError(ErrorCode::ProtocolError, "successful service reply has no result");
    }
    return *result;
  }
  const JsonValue* error = envelope.Find("error");
  if (error == nullptr || !error->IsObject()) {
    return MakeError(ErrorCode::ProtocolError, "failed service reply has no error object");
  }
  Error typed;
  const std::string* code = error->Find("code") != nullptr ? error->Find("code")->AsString() : nullptr;
  typed.code = code != nullptr ? ErrorCodeFromName(*code) : ErrorCode::Internal;
  const std::string* message =
      error->Find("message") != nullptr ? error->Find("message")->AsString() : nullptr;
  typed.message = message != nullptr ? *message : "service reported an unnamed failure";
  const std::string* detail =
      error->Find("detail") != nullptr ? error->Find("detail")->AsString() : nullptr;
  if (detail != nullptr) typed.detail = *detail;
  return typed;
}

}  // namespace

Result<std::unique_ptr<RegistryClient>> RegistryClient::Connect(const ClientOptions& options) {
  if (options.port == 0) {
    return MakeError(ErrorCode::InvalidArgument, "a registry service port is required");
  }
  auto socket = Socket::Connect(options.address, options.port);
  if (!socket.has_value()) return socket.error();

  auto client = std::unique_ptr<RegistryClient>(new RegistryClient());
  client->options_ = options;
  client->socket_ = std::move(socket).value();

  Frame hello;
  hello.kind = FrameKind::Hello;
  hello.correlation = 0;
  JsonValue payload = JsonValue::Obj();
  payload.Set("client_id", JsonValue::Str(options.client_id));
  payload.Set("protocol_version", JsonValue::UInt(options.protocol_version));
  payload.Set("max_frame_bytes", JsonValue::UInt(options.max_frame_bytes));
  hello.SetPayloadText(payload.Dump(false));

  auto encoded = EncodeFrame(hello, options.max_frame_bytes);
  if (!encoded.has_value()) return encoded.error();
  Status sent = client->socket_.SendAll(std::span<const std::byte>(encoded.value()));
  if (!sent.ok()) return sent.error();

  Frame reply;
  auto read = ReadFrame(client->socket_, options.max_frame_bytes, reply);
  if (!read.has_value()) return read.error();
  if (!read.value()) {
    return MakeError(ErrorCode::HandshakeFailed, "the registry service closed during handshake");
  }
  if (reply.kind == FrameKind::Error) {
    auto envelope = ParseJson(reply.PayloadText(), JsonLimits{});
    if (envelope.has_value()) {
      auto unwrapped = UnwrapEnvelope(envelope.value());
      if (!unwrapped.has_value()) return unwrapped.error();
    }
    return MakeError(ErrorCode::HandshakeFailed, "the registry service refused the handshake",
                     reply.PayloadText());
  }
  if (reply.kind != FrameKind::HelloAck) {
    return MakeError(ErrorCode::HandshakeFailed, "expected a handshake acknowledgement frame");
  }
  auto ack = ParseJson(reply.PayloadText(), JsonLimits{});
  if (!ack.has_value()) return ack.error();
  auto version = RequireUInt(ack.value(), "protocol_version", "handshake");
  if (!version.has_value()) return version.error();
  auto max_frame = RequireUInt(ack.value(), "max_frame_bytes", "handshake");
  if (!max_frame.has_value()) return max_frame.error();
  auto incarnation = RequireUInt(ack.value(), "incarnation", "handshake");
  if (!incarnation.has_value()) return incarnation.error();

  client->handshake_.protocol_version = static_cast<std::uint32_t>(version.value());
  client->handshake_.max_frame_bytes = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(max_frame.value(), options.max_frame_bytes));
  client->handshake_.incarnation = Incarnation(incarnation.value());
  if (client->handshake_.max_frame_bytes < 1024) {
    return MakeError(ErrorCode::HandshakeFailed,
                     "the registry service negotiated an unusable frame size");
  }
  const JsonValue* generation = ack.value().Find("generation");
  if (generation != nullptr && generation->IsObject()) {
    auto number = RequireUInt(*generation, "number", "handshake generation");
    if (!number.has_value()) return number.error();
    auto digest_text = RequireString(*generation, "digest", "handshake generation");
    if (!digest_text.has_value()) return digest_text.error();
    auto digest = ContentDigest::FromHex(digest_text.value());
    if (!digest.has_value()) return digest.error();
    GenerationId id;
    id.number = GenerationNumber(number.value());
    id.digest = digest.value();
    client->handshake_.generation = id;
  }
  auto name = OptionalString(ack.value(), "registry_name", "");
  if (!name.has_value()) return name.error();
  client->handshake_.registry_name = name.value();
  const JsonValue* persistent = ack.value().Find("persistent");
  if (persistent != nullptr) client->handshake_.persistent = persistent->AsBool(false);
  const JsonValue* epoch = ack.value().Find("publisher_epoch");
  if (epoch != nullptr && epoch->AsUInt().has_value()) {
    client->handshake_.epoch = PublisherEpoch(epoch->AsUInt().value());
  }
  return client;
}

RegistryClient::~RegistryClient() { Close(); }

void RegistryClient::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) return;
  closed_ = true;
  Frame bye;
  bye.kind = FrameKind::Bye;
  bye.correlation = correlation_;
  auto encoded = EncodeFrame(bye, handshake_.max_frame_bytes);
  if (encoded.has_value()) {
    (void)socket_.SendAll(std::span<const std::byte>(encoded.value()));
  }
  socket_.ShutdownBoth();
  socket_.Close();
}

Result<JsonValue> RegistryClient::Call(const JsonValue& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) {
    return MakeError(ErrorCode::TransportError, "the registry client is closed");
  }
  Frame frame;
  frame.kind = FrameKind::Request;
  frame.correlation = ++correlation_;
  frame.SetPayloadText(request.Dump(false));
  auto encoded = EncodeFrame(frame, handshake_.max_frame_bytes);
  if (!encoded.has_value()) return encoded.error();
  Status sent = socket_.SendAll(std::span<const std::byte>(encoded.value()));
  if (!sent.ok()) return sent.error();

  Frame reply;
  auto read = ReadFrame(socket_, handshake_.max_frame_bytes, reply);
  if (!read.has_value()) return read.error();
  if (!read.value()) {
    return MakeError(ErrorCode::TransportError, "the registry service closed the connection");
  }
  if (reply.correlation != frame.correlation) {
    return MakeError(ErrorCode::ProtocolError,
                     "service reply correlation does not match the request",
                     "expected " + std::to_string(frame.correlation) + " got " +
                         std::to_string(reply.correlation));
  }
  if (reply.kind != FrameKind::Response && reply.kind != FrameKind::Error) {
    return MakeError(ErrorCode::ProtocolError, "unexpected reply frame kind",
                     std::string(FrameKindName(reply.kind)));
  }
  return ParseJson(reply.PayloadText(), JsonLimits{});
}

Result<JsonValue> RegistryClient::CallResult(const JsonValue& request) {
  auto envelope = Call(request);
  if (!envelope.has_value()) return envelope.error();
  return UnwrapEnvelope(envelope.value());
}

Result<JsonValue> RegistryClient::Ping() {
  JsonValue request = JsonValue::Obj();
  request.Set("op", JsonValue::Str("ping"));
  return CallResult(request);
}

}  // namespace fcr
