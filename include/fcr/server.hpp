// Fabric Compatibility Registry - Summon Software Labs
// The registry service: bounded worker pool over real loopback TCP sockets.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "fcr/authority.hpp"
#include "fcr/cancel.hpp"
#include "fcr/error.hpp"
#include "fcr/protocol.hpp"
#include "fcr/transport.hpp"

namespace fcr {

struct ServerOptions {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 0;
  // Each worker serves one connection at a time. worker_threads therefore
  // bounds the number of connections that can complete a handshake
  // concurrently; further admitted connections wait in the bounded queue.
  std::size_t worker_threads = 4;
  std::size_t max_connections = 32;
  std::size_t pending_connections = 256;
  std::uint32_t max_frame_bytes = kDefaultMaxFrameBytes;
  std::uint32_t protocol_version = kServiceProtocolVersion;
  std::size_t backlog = 128;
  bool allow_publish = true;
  bool allow_prune = true;
  bool allow_shutdown = true;
  // Invoked once when shutdown begins, on the requesting thread and never
  // while a server lock is held. The daemon uses it to leave its wait loop.
  std::function<void()> on_shutdown_request;
};

struct ServerStats {
  std::uint64_t accepted_connections = 0;
  std::uint64_t rejected_connections = 0;
  std::uint64_t closed_connections = 0;
  std::uint64_t completed_requests = 0;
  std::uint64_t failed_requests = 0;
  std::uint64_t protocol_errors = 0;
  std::uint64_t handshakes = 0;
  std::uint64_t bytes_received = 0;
  std::uint64_t bytes_sent = 0;
  std::size_t active_connections = 0;
  std::size_t queued_connections = 0;
};

// Shutdown sequence:
//   1. stop admission and cancel the shutdown token (in-flight work observes it
//      before its commit point);
//   2. close the listening socket so the acceptor leaves accept();
//   3. shut every live connection down so blocked receivers return;
//   4. wake idle workers and join the acceptor and the workers.
// The join is unconditional; there is no polling and no timeout.
class RegistryServer {
 public:
  static Result<std::unique_ptr<RegistryServer>> Start(std::shared_ptr<RegistryAuthority> authority,
                                                       const ServerOptions& options);

  RegistryServer(const RegistryServer&) = delete;
  RegistryServer& operator=(const RegistryServer&) = delete;
  ~RegistryServer();

  std::uint16_t port() const { return port_; }
  bool IsStopping() const { return stopping_.load(std::memory_order_acquire); }
  void RequestShutdown() { BeginShutdown(); }
  // Blocking and idempotent.
  void Shutdown();
  ServerStats stats() const;
  const CancellationToken& shutdown_token() const { return shutdown_token_; }

 private:
  RegistryServer() = default;

  // One private loopback pair per worker. Writing a byte to the write end
  // makes the worker's blocking select() return so it can observe shutdown
  // without another thread touching its connection socket.
  struct WakeupPair {
    Socket read_end;
    Socket write_end;
  };

  void BeginShutdown();
  void WakeAcceptor();
  Status CreateWakeupPairs(std::size_t count);
  void AcceptorLoop();
  void WorkerLoop(std::size_t index);
  void ServeConnection(Socket socket, const Socket& cancel);
  bool EnqueueConnection(Socket socket);
  void UnregisterConnection(std::uintptr_t handle);

  ServerOptions options_;
  std::shared_ptr<RegistryAuthority> authority_;
  RequestContext context_;
  TcpListener listener_;
  std::uint16_t port_ = 0;

  std::atomic<bool> stopping_{false};
  CancellationToken shutdown_token_;

  std::thread acceptor_;
  std::vector<std::thread> workers_;
  std::vector<WakeupPair> wakeups_;

  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<Socket> queue_;

  mutable std::mutex connections_mutex_;
  std::set<std::uintptr_t> connections_;
  std::size_t active_connections_ = 0;

  std::atomic<std::uint64_t> accepted_{0};
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<std::uint64_t> closed_{0};
  std::atomic<std::uint64_t> completed_{0};
  std::atomic<std::uint64_t> failed_{0};
  std::atomic<std::uint64_t> protocol_errors_{0};
  std::atomic<std::uint64_t> handshakes_{0};
  std::atomic<std::uint64_t> bytes_received_{0};
  std::atomic<std::uint64_t> bytes_sent_{0};
};

}  // namespace fcr
