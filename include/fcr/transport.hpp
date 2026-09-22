// Fabric Compatibility Registry - Summon Software Labs
// Framed, length-checked transport over loopback TCP.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "fcr/error.hpp"

namespace fcr {

// Frame layout (little endian):
//   0   4  magic 'F','C','R','F'
//   4   2  frame format version
//   6   2  frame kind
//   8   4  payload length
//   12  8  correlation identifier
//   20  4  CRC-32C over header bytes [0,20)
//   24  4  CRC-32C over the payload
//   28  .. payload
inline constexpr std::size_t kFrameHeaderBytes = 28;
inline constexpr std::uint16_t kFrameFormatVersion = 1;
inline constexpr std::uint32_t kDefaultMaxFrameBytes = 1024u * 1024u;

enum class FrameKind : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  Request = 3,
  Response = 4,
  Error = 5,
  Ping = 6,
  Pong = 7,
  Bye = 8,
};

std::string_view FrameKindName(FrameKind kind);

struct Frame {
  FrameKind kind = FrameKind::Request;
  std::uint64_t correlation = 0;
  std::vector<std::byte> payload;

  void SetPayloadText(std::string_view text);
  std::string PayloadText() const;
};

Result<std::vector<std::byte>> EncodeFrame(const Frame& frame, std::uint32_t max_frame_bytes);

// Outcome of a read that another socket can abandon.
//
// This exists because a thread blocked in recv() is not reliably woken by
// shutdown() on the same socket from another thread, and closing the socket
// from another thread races with the reader. Waiting through select() on a
// private wakeup socket keeps every connection socket owned by one thread.
enum class ReadOutcome : std::uint8_t {
  // The requested bytes, or the whole frame, were read.
  Complete,
  // The peer closed cleanly before the message started.
  Closed,
  // The cancel socket became readable first; nothing was consumed.
  Cancelled,
};

// Owns a connected stream socket.
class Socket {
 public:
  Socket() = default;
  explicit Socket(std::uintptr_t handle) : handle_(handle) {}
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  ~Socket();

  bool valid() const { return handle_ != kInvalid; }
  std::uintptr_t native() const { return handle_; }
  std::uintptr_t Release() {
    const std::uintptr_t handle = handle_;
    handle_ = kInvalid;
    return handle;
  }

  void Close();
  void ShutdownBoth();

  Status SendAll(std::span<const std::byte> data);
  // Returns true when the requested bytes were read, false on a clean EOF that
  // happened exactly at the start of the read.
  Result<bool> RecvAll(std::span<std::byte> data);
  // Reads exactly data.size() bytes, waiting on this socket or on the cancel
  // socket, whichever becomes readable first.
  Result<ReadOutcome> RecvAllCancellable(std::span<std::byte> data, const Socket& cancel);

  static Result<Socket> Connect(const std::string& address, std::uint16_t port);
  static void SetNoDelay(std::uintptr_t handle, bool enable);

 private:
  static constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(~std::uintptr_t{0});
  std::uintptr_t handle_ = kInvalid;
};

class TcpListener {
 public:
  TcpListener() = default;
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  ~TcpListener();

  static Result<TcpListener> Bind(const std::string& address, std::uint16_t port,
                                  std::size_t backlog);

  bool valid() const { return handle_.load() != kInvalid; }
  std::uint16_t port() const { return port_; }
  // Blocks until a connection arrives or the listener is closed.
  Result<Socket> Accept();
  void Close();

 private:
  static constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(~std::uintptr_t{0});
  std::atomic<std::uintptr_t> handle_{kInvalid};
  std::uint16_t port_ = 0;
};

// Reads one frame. Returns false on a clean EOF before any header byte.
Result<bool> ReadFrame(Socket& socket, std::uint32_t max_frame_bytes, Frame& out);

// Reads one frame, abandoning the wait when the cancel socket becomes readable.
Result<ReadOutcome> ReadFrameCancellable(Socket& socket, std::uint32_t max_frame_bytes,
                                         const Socket& cancel, Frame& out);

// Ensures the process-wide socket subsystem is initialised. Safe to call from
// any thread; the first call performs initialisation.
Status EnsureSocketRuntime();

}  // namespace fcr
