#include "fcr/server.hpp"

#include <algorithm>
#include <utility>

namespace fcr {
namespace {

constexpr std::size_t kMaxHelloBytes = 4096;

JsonValue MakeHelloAck(const ServerOptions& options, std::uint32_t effective_max_frame,
                       const IncarnationStamp& stamp, const std::shared_ptr<RegistryAuthority>&
                                                       authority) {
  JsonValue out = JsonValue::Obj();
  out.Set("service", JsonValue::Str(std::string(kServiceName)));
  out.Set("protocol_version", JsonValue::UInt(options.protocol_version));
  out.Set("max_frame_bytes", JsonValue::UInt(effective_max_frame));
  out.Set("incarnation", JsonValue::UInt(stamp.incarnation.value()));
  if (stamp.generation.IsValid()) {
    JsonValue generation = JsonValue::Obj();
    generation.Set("number", JsonValue::UInt(stamp.generation.number.value()));
    generation.Set("digest", JsonValue::Str(stamp.generation.digest.ToHex()));
    generation.Set("id", JsonValue::Str(stamp.generation.ToString()));
    out.Set("generation", std::move(generation));
  }
  auto status = authority->GetStatus();
  if (status.has_value()) {
    out.Set("registry_name", JsonValue::Str(status.value().registry_name));
    out.Set("persistent", JsonValue::Bool(status.value().persistent));
    out.Set("publisher_epoch", JsonValue::UInt(status.value().epoch.value()));
  }
  return out;
}

}  // namespace

Result<std::unique_ptr<RegistryServer>> RegistryServer::Start(
    std::shared_ptr<RegistryAuthority> authority, const ServerOptions& options) {
  if (authority == nullptr) {
    return MakeError(ErrorCode::InvalidArgument, "the registry service needs an authority");
  }
  if (options.worker_threads == 0 || options.worker_threads > 64) {
    return MakeError(ErrorCode::InvalidArgument, "worker thread count must be between 1 and 64");
  }
  if (options.max_connections == 0 || options.max_connections > 4096) {
    return MakeError(ErrorCode::InvalidArgument, "connection limit must be between 1 and 4096");
  }
  if (options.pending_connections == 0 || options.pending_connections > 65536) {
    return MakeError(ErrorCode::InvalidArgument, "pending connection limit is out of range");
  }
  if (options.max_frame_bytes < 1024 || options.max_frame_bytes > 64u * 1024u * 1024u) {
    return MakeError(ErrorCode::InvalidArgument, "maximum frame size is out of range");
  }

  auto listener = TcpListener::Bind(options.bind_address, options.port, options.backlog);
  if (!listener.has_value()) return listener.error();

  auto server = std::unique_ptr<RegistryServer>(new RegistryServer());
  server->options_ = options;
  server->authority_ = std::move(authority);
  server->listener_ = std::move(listener).value();
  server->port_ = server->listener_.port();
  server->context_.authority = server->authority_;
  server->context_.shutdown_token = server->shutdown_token_;
  server->context_.allow_publish = options.allow_publish;
  server->context_.allow_prune = options.allow_prune;
  server->context_.allow_shutdown = options.allow_shutdown;
  server->context_.request_shutdown = [raw = server.get()] { raw->RequestShutdown(); };

  Status wakeups = server->CreateWakeupPairs(options.worker_threads);
  if (!wakeups.ok()) return wakeups.error();

  server->acceptor_ = std::thread([raw = server.get()] { raw->AcceptorLoop(); });
  server->workers_.reserve(options.worker_threads);
  for (std::size_t i = 0; i < options.worker_threads; ++i) {
    server->workers_.emplace_back([raw = server.get(), i] { raw->WorkerLoop(i); });
  }
  return server;
}

Status RegistryServer::CreateWakeupPairs(std::size_t count) {
  wakeups_.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    auto listener = TcpListener::Bind("127.0.0.1", 0, 1);
    if (!listener.has_value()) return listener.error();
    auto write_end = Socket::Connect("127.0.0.1", listener.value().port());
    if (!write_end.has_value()) return write_end.error();
    auto read_end = listener.value().Accept();
    if (!read_end.has_value()) return read_end.error();
    listener.value().Close();
    WakeupPair pair;
    pair.read_end = std::move(read_end).value();
    pair.write_end = std::move(write_end).value();
    wakeups_.push_back(std::move(pair));
  }
  return Status{};
}

RegistryServer::~RegistryServer() { Shutdown(); }

void RegistryServer::BeginShutdown() {
  if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
  shutdown_token_.Cancel();
  // Waking a blocked accept() by closing the listening socket from another
  // thread is not a supported Winsock operation. A real connection to our own
  // listener is: accept() returns it, the acceptor observes the stop flag and
  // leaves. The fallback path only runs if the wake connection cannot be made.
  WakeAcceptor();
  // Wake every worker that is blocked waiting for client bytes. The worker
  // owns its connection socket exclusively, so shutdown never closes a socket
  // that another thread may be using.
  const std::byte token{1};
  for (WakeupPair& pair : wakeups_) {
    if (!pair.write_end.valid()) continue;
    (void)pair.write_end.SendAll(std::span<const std::byte>(&token, 1));
  }
  queue_cv_.notify_all();
  if (options_.on_shutdown_request) options_.on_shutdown_request();
}

void RegistryServer::WakeAcceptor() {
  if (port_ == 0) {
    listener_.Close();
    return;
  }
  const std::string address =
      options_.bind_address.empty() ? std::string("127.0.0.1") : options_.bind_address;
  auto wake = Socket::Connect(address, port_);
  if (!wake.has_value()) {
    listener_.Close();
    return;
  }
  wake.value().Close();
}

void RegistryServer::Shutdown() {
  BeginShutdown();
  if (acceptor_.joinable()) acceptor_.join();
  listener_.Close();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
  wakeups_.clear();
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!queue_.empty()) {
      queue_.front().Close();
      queue_.pop_front();
    }
  }
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.clear();
    active_connections_ = 0;
  }
}

ServerStats RegistryServer::stats() const {
  ServerStats out;
  out.accepted_connections = accepted_.load(std::memory_order_relaxed);
  out.rejected_connections = rejected_.load(std::memory_order_relaxed);
  out.closed_connections = closed_.load(std::memory_order_relaxed);
  out.completed_requests = completed_.load(std::memory_order_relaxed);
  out.failed_requests = failed_.load(std::memory_order_relaxed);
  out.protocol_errors = protocol_errors_.load(std::memory_order_relaxed);
  out.handshakes = handshakes_.load(std::memory_order_relaxed);
  out.bytes_received = bytes_received_.load(std::memory_order_relaxed);
  out.bytes_sent = bytes_sent_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    out.active_connections = active_connections_;
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    out.queued_connections = queue_.size();
  }
  return out;
}

void RegistryServer::AcceptorLoop() {
  while (true) {
    auto accepted = listener_.Accept();
    if (stopping_.load(std::memory_order_acquire)) {
      if (accepted.has_value()) accepted.value().Close();
      break;
    }
    if (!accepted.has_value()) {
      protocol_errors_.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    Socket socket = std::move(accepted).value();
    accepted_.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      if (active_connections_ >= options_.max_connections) {
        // Refuse politely rather than queueing without bound.
        Frame refusal;
        refusal.kind = FrameKind::Error;
        refusal.SetPayloadText(
            ErrorResponse(MakeError(ErrorCode::ConnectionLimitReached,
                                    "registry service connection limit reached"))
                .Dump(false));
        auto encoded = EncodeFrame(refusal, options_.max_frame_bytes);
        if (encoded.has_value()) {
          (void)socket.SendAll(std::span<const std::byte>(encoded.value()));
        }
        socket.Close();
        rejected_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      connections_.insert(socket.native());
      ++active_connections_;
    }
    if (stopping_.load(std::memory_order_acquire)) {
      UnregisterConnection(socket.native());
      socket.Close();
      closed_.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    if (!EnqueueConnection(std::move(socket))) {
      // Queue is full: the connection was already closed by EnqueueConnection.
      rejected_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

bool RegistryServer::EnqueueConnection(Socket socket) {
  const std::uintptr_t handle = socket.native();
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= options_.pending_connections) {
      // Bounded queue: refuse instead of growing without limit.
      UnregisterConnection(handle);
      socket.Close();
      closed_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    queue_.push_back(std::move(socket));
  }
  queue_cv_.notify_one();
  return true;
}

void RegistryServer::UnregisterConnection(std::uintptr_t handle) {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  if (connections_.erase(handle) != 0 && active_connections_ > 0) {
    --active_connections_;
  }
}

void RegistryServer::WorkerLoop(std::size_t index) {
  const Socket& cancel = index < wakeups_.size() ? wakeups_[index].read_end : wakeups_.front().read_end;
  while (true) {
    Socket socket;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] {
        return stopping_.load(std::memory_order_acquire) || !queue_.empty();
      });
      if (queue_.empty()) {
        if (stopping_.load(std::memory_order_acquire)) return;
        continue;
      }
      socket = std::move(queue_.front());
      queue_.pop_front();
    }
    if (stopping_.load(std::memory_order_acquire)) {
      UnregisterConnection(socket.native());
      socket.Close();
      closed_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    ServeConnection(std::move(socket), cancel);
  }
}

void RegistryServer::ServeConnection(Socket socket, const Socket& cancel) {
  const std::uintptr_t handle = socket.native();
  const auto finish = [this, handle, &socket] {
    UnregisterConnection(handle);
    socket.Close();
    closed_.fetch_add(1, std::memory_order_relaxed);
  };

  Frame hello;
  auto hello_read = ReadFrameCancellable(socket, kMaxHelloBytes, cancel, hello);
  if (!hello_read.has_value()) {
    protocol_errors_.fetch_add(1, std::memory_order_relaxed);
    finish();
    return;
  }
  if (hello_read.value() != ReadOutcome::Complete) {
    finish();
    return;
  }
  if (hello.kind != FrameKind::Hello) {
    protocol_errors_.fetch_add(1, std::memory_order_relaxed);
    finish();
    return;
  }
  bytes_received_.fetch_add(hello.payload.size() + kFrameHeaderBytes, std::memory_order_relaxed);
  {
    auto json = ParseJson(hello.PayloadText(), JsonLimits{});
    if (!json.has_value()) {
      protocol_errors_.fetch_add(1, std::memory_order_relaxed);
      finish();
      return;
    }
    auto protocol_version = RequireUInt(json.value(), "protocol_version", "handshake");
    if (!protocol_version.has_value() ||
        protocol_version.value() != options_.protocol_version) {
      Frame refusal;
      refusal.kind = FrameKind::Error;
      refusal.correlation = hello.correlation;
      refusal.SetPayloadText(
          ErrorResponse(MakeError(ErrorCode::HandshakeFailed,
                                  "client protocol version is not supported",
                                  "server " + std::to_string(options_.protocol_version)))
              .Dump(false));
      auto encoded = EncodeFrame(refusal, options_.max_frame_bytes);
      if (encoded.has_value()) {
        (void)socket.SendAll(std::span<const std::byte>(encoded.value()));
      }
      handshakes_.fetch_add(1, std::memory_order_relaxed);
      finish();
      return;
    }
  }
  std::uint32_t effective_max = options_.max_frame_bytes;
  {
    auto json = ParseJson(hello.PayloadText(), JsonLimits{});
    if (json.has_value()) {
      const JsonValue* requested = json.value().Find("max_frame_bytes");
      if (requested != nullptr) {
        auto value = requested->AsUInt();
        if (value.has_value() && value.value() >= 1024) {
          effective_max = static_cast<std::uint32_t>(
              std::min<std::uint64_t>(value.value(), options_.max_frame_bytes));
        }
      }
    }
  }
  {
    const IncarnationStamp stamp = authority_->Stamp();
    Frame ack;
    ack.kind = FrameKind::HelloAck;
    ack.correlation = hello.correlation;
    ack.SetPayloadText(
        MakeHelloAck(options_, effective_max, stamp, authority_).Dump(false));
    auto encoded = EncodeFrame(ack, options_.max_frame_bytes);
    if (!encoded.has_value()) {
      finish();
      return;
    }
    Status sent = socket.SendAll(std::span<const std::byte>(encoded.value()));
    if (!sent.ok()) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      finish();
      return;
    }
    bytes_sent_.fetch_add(encoded.value().size(), std::memory_order_relaxed);
    handshakes_.fetch_add(1, std::memory_order_relaxed);
  }

  while (!stopping_.load(std::memory_order_acquire)) {
    Frame frame;
    auto read = ReadFrameCancellable(socket, effective_max, cancel, frame);
    if (!read.has_value()) {
      protocol_errors_.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    if (read.value() == ReadOutcome::Cancelled) break;
    if (read.value() == ReadOutcome::Closed) break;
    bytes_received_.fetch_add(frame.payload.size() + kFrameHeaderBytes,
                              std::memory_order_relaxed);
    if (frame.kind == FrameKind::Bye) break;
    if (frame.kind == FrameKind::Ping) {
      Frame pong;
      pong.kind = FrameKind::Pong;
      pong.correlation = frame.correlation;
      pong.payload = frame.payload;
      auto encoded = EncodeFrame(pong, effective_max);
      if (!encoded.has_value()) break;
      Status sent = socket.SendAll(std::span<const std::byte>(encoded.value()));
      if (!sent.ok()) break;
      bytes_sent_.fetch_add(encoded.value().size(), std::memory_order_relaxed);
      continue;
    }
    if (frame.kind != FrameKind::Request) {
      protocol_errors_.fetch_add(1, std::memory_order_relaxed);
      Frame refusal;
      refusal.kind = FrameKind::Error;
      refusal.correlation = frame.correlation;
      refusal.SetPayloadText(ErrorResponse(MakeError(ErrorCode::ProtocolError,
                                                     "only request frames are accepted here"))
                                 .Dump(false));
      auto encoded = EncodeFrame(refusal, effective_max);
      if (encoded.has_value()) (void)socket.SendAll(std::span<const std::byte>(encoded.value()));
      break;
    }

    JsonValue response;
    {
      auto request = ParseJson(frame.PayloadText(), JsonLimits{});
      if (!request.has_value()) {
        response = ErrorResponse(request.error());
        failed_.fetch_add(1, std::memory_order_relaxed);
      } else {
        response = DispatchEnvelope(request.value(), context_);
        if (response.Find("ok") != nullptr && response.Find("ok")->AsBool(false)) {
          completed_.fetch_add(1, std::memory_order_relaxed);
        } else {
          failed_.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    const std::string text = response.Dump(false);
    if (text.size() > effective_max) {
      Frame too_large;
      too_large.kind = FrameKind::Error;
      too_large.correlation = frame.correlation;
      too_large.SetPayloadText(
          ErrorResponse(MakeError(ErrorCode::LimitExceeded,
                                  "response exceeds the negotiated maximum frame size"))
              .Dump(false));
      auto encoded = EncodeFrame(too_large, effective_max);
      if (!encoded.has_value()) break;
      Status sent = socket.SendAll(std::span<const std::byte>(encoded.value()));
      if (!sent.ok()) break;
      bytes_sent_.fetch_add(encoded.value().size(), std::memory_order_relaxed);
      continue;
    }
    Frame reply;
    reply.kind = FrameKind::Response;
    reply.correlation = frame.correlation;
    reply.SetPayloadText(text);
    auto encoded = EncodeFrame(reply, effective_max);
    if (!encoded.has_value()) break;
    Status sent = socket.SendAll(std::span<const std::byte>(encoded.value()));
    if (!sent.ok()) break;
    bytes_sent_.fetch_add(encoded.value().size(), std::memory_order_relaxed);
  }
  finish();
}

}  // namespace fcr
