#include "fcr/transport.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "fcr/digest.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#error "Fabric Compatibility Registry currently builds only for Windows x64"
#endif

namespace fcr {
namespace {

constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(~std::uintptr_t{0});

void StoreLe16(std::byte* out, std::uint16_t value) {
  out[0] = static_cast<std::byte>(value & 0xffu);
  out[1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void StoreLe32(std::byte* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
}

void StoreLe64(std::byte* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
}

std::uint16_t LoadLe16(const std::byte* in) {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(in[0]) |
                                    (std::to_integer<unsigned>(in[1]) << 8));
}

std::uint32_t LoadLe32(const std::byte* in) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(in[i])) << (8 * i);
  }
  return value;
}

std::uint64_t LoadLe64(const std::byte* in) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(in[i])) << (8 * i);
  }
  return value;
}

}  // namespace

std::string_view FrameKindName(FrameKind kind) {
  switch (kind) {
    case FrameKind::Hello: return "hello";
    case FrameKind::HelloAck: return "hello_ack";
    case FrameKind::Request: return "request";
    case FrameKind::Response: return "response";
    case FrameKind::Error: return "error";
    case FrameKind::Ping: return "ping";
    case FrameKind::Pong: return "pong";
    case FrameKind::Bye: return "bye";
  }
  return "unknown";
}

Status EnsureSocketRuntime() {
  static std::once_flag once;
  static Status result;
  std::call_once(once, [] {
    WSADATA data{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
      result = Fail(ErrorCode::TransportError, "WSAStartup failed");
    }
  });
  return result;
}

void Frame::SetPayloadText(std::string_view text) {
  payload.assign(reinterpret_cast<const std::byte*>(text.data()),
                 reinterpret_cast<const std::byte*>(text.data()) + text.size());
}

std::string Frame::PayloadText() const {
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

Result<std::vector<std::byte>> EncodeFrame(const Frame& frame, std::uint32_t max_frame_bytes) {
  if (frame.payload.size() > max_frame_bytes) {
    return MakeError(ErrorCode::LimitExceeded,
                     "frame payload exceeds the negotiated maximum frame size",
                     "payload " + std::to_string(frame.payload.size()) + " limit " +
                         std::to_string(max_frame_bytes));
  }
  std::vector<std::byte> out(kFrameHeaderBytes + frame.payload.size(), std::byte{0});
  out[0] = std::byte{'F'};
  out[1] = std::byte{'C'};
  out[2] = std::byte{'R'};
  out[3] = std::byte{'F'};
  StoreLe16(out.data() + 4, kFrameFormatVersion);
  StoreLe16(out.data() + 6, static_cast<std::uint16_t>(frame.kind));
  StoreLe32(out.data() + 8, static_cast<std::uint32_t>(frame.payload.size()));
  StoreLe64(out.data() + 12, frame.correlation);
  StoreLe32(out.data() + 20,
            Crc32c(std::span<const std::byte>(out.data(), static_cast<std::size_t>(20))));
  StoreLe32(out.data() + 24, Crc32c(std::span<const std::byte>(frame.payload)));
  if (!frame.payload.empty()) {
    std::memcpy(out.data() + kFrameHeaderBytes, frame.payload.data(), frame.payload.size());
  }
  return out;
}

Result<ReadOutcome> Socket::RecvAllCancellable(std::span<std::byte> data, const Socket& cancel) {
  if (handle_ == kInvalid) {
    return MakeError(ErrorCode::TransportError, "receive on an invalid socket");
  }
  if (data.empty()) return ReadOutcome::Complete;
  const SOCKET socket = static_cast<SOCKET>(handle_);
  const SOCKET cancel_socket =
      cancel.valid() ? static_cast<SOCKET>(cancel.native()) : INVALID_SOCKET;
  std::size_t offset = 0;
  while (offset < data.size()) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    if (cancel_socket != INVALID_SOCKET) FD_SET(cancel_socket, &readable);
    // No timeout: the wait ends when data arrives or the service is stopping.
    const int ready = select(0, &readable, nullptr, nullptr, nullptr);
    if (ready == SOCKET_ERROR) {
      return MakeError(ErrorCode::TransportError,
                       "socket select failed with error " + std::to_string(WSAGetLastError()));
    }
    if (cancel_socket != INVALID_SOCKET && FD_ISSET(cancel_socket, &readable)) {
      return ReadOutcome::Cancelled;
    }
    if (!FD_ISSET(socket, &readable)) continue;
    const int want = static_cast<int>(
        std::min<std::size_t>(data.size() - offset, static_cast<std::size_t>(1) << 20));
    const int received = recv(socket, reinterpret_cast<char*>(data.data() + offset), want, 0);
    if (received == SOCKET_ERROR) {
      return MakeError(ErrorCode::TransportError,
                       "socket receive failed with error " + std::to_string(WSAGetLastError()));
    }
    if (received == 0) {
      if (offset == 0) return ReadOutcome::Closed;
      return MakeError(ErrorCode::TransportError, "connection closed mid-message");
    }
    offset += static_cast<std::size_t>(received);
  }
  return ReadOutcome::Complete;
}

Result<ReadOutcome> ReadFrameCancellable(Socket& socket, std::uint32_t max_frame_bytes,
                                         const Socket& cancel, Frame& out) {
  std::byte header[kFrameHeaderBytes];
  auto header_read =
      socket.RecvAllCancellable(std::span<std::byte>(header, kFrameHeaderBytes), cancel);
  if (!header_read.has_value()) return header_read.error();
  if (header_read.value() == ReadOutcome::Cancelled) return ReadOutcome::Cancelled;
  if (header_read.value() == ReadOutcome::Closed) return ReadOutcome::Closed;
  if (std::to_integer<char>(header[0]) != 'F' || std::to_integer<char>(header[1]) != 'C' ||
      std::to_integer<char>(header[2]) != 'R' || std::to_integer<char>(header[3]) != 'F') {
    return MakeError(ErrorCode::ProtocolError, "frame magic mismatch");
  }
  const std::uint16_t version = LoadLe16(header + 4);
  if (version != kFrameFormatVersion) {
    return MakeError(ErrorCode::ProtocolError, "unsupported frame format version",
                     std::to_string(version));
  }
  const std::uint32_t header_crc = LoadLe32(header + 20);
  const std::uint32_t actual_header_crc = Crc32c(std::span<const std::byte>(header, 20));
  if (header_crc != actual_header_crc) {
    return MakeError(ErrorCode::ProtocolError, "frame header checksum mismatch");
  }
  const std::uint32_t payload_length = LoadLe32(header + 8);
  if (payload_length > max_frame_bytes) {
    return MakeError(ErrorCode::LimitExceeded, "incoming frame exceeds the maximum frame size",
                     std::to_string(payload_length));
  }
  out.kind = static_cast<FrameKind>(LoadLe16(header + 6));
  out.correlation = LoadLe64(header + 12);
  out.payload.assign(payload_length, std::byte{0});
  if (payload_length != 0) {
    auto payload_read =
        socket.RecvAllCancellable(std::span<std::byte>(out.payload.data(), payload_length), cancel);
    if (!payload_read.has_value()) return payload_read.error();
    if (payload_read.value() != ReadOutcome::Complete) return payload_read.value();
  }
  const std::uint32_t payload_crc = LoadLe32(header + 24);
  if (payload_crc != Crc32c(std::span<const std::byte>(out.payload))) {
    return MakeError(ErrorCode::ProtocolError, "frame payload checksum mismatch");
  }
  return ReadOutcome::Complete;
}

Result<bool> ReadFrame(Socket& socket, std::uint32_t max_frame_bytes, Frame& out) {
  std::byte header[kFrameHeaderBytes];
  auto header_read = socket.RecvAll(std::span<std::byte>(header, kFrameHeaderBytes));
  if (!header_read.has_value()) return header_read.error();
  if (!header_read.value()) return false;
  if (std::to_integer<char>(header[0]) != 'F' || std::to_integer<char>(header[1]) != 'C' ||
      std::to_integer<char>(header[2]) != 'R' || std::to_integer<char>(header[3]) != 'F') {
    return MakeError(ErrorCode::ProtocolError, "frame magic mismatch");
  }
  const std::uint16_t version = LoadLe16(header + 4);
  if (version != kFrameFormatVersion) {
    return MakeError(ErrorCode::ProtocolError, "unsupported frame format version",
                     std::to_string(version));
  }
  const std::uint32_t header_crc = LoadLe32(header + 20);
  const std::uint32_t actual_header_crc = Crc32c(std::span<const std::byte>(header, 20));
  if (header_crc != actual_header_crc) {
    return MakeError(ErrorCode::ProtocolError, "frame header checksum mismatch");
  }
  const std::uint32_t payload_length = LoadLe32(header + 8);
  if (payload_length > max_frame_bytes) {
    return MakeError(ErrorCode::LimitExceeded, "incoming frame exceeds the maximum frame size",
                     std::to_string(payload_length));
  }
  out.kind = static_cast<FrameKind>(LoadLe16(header + 6));
  out.correlation = LoadLe64(header + 12);
  out.payload.assign(payload_length, std::byte{0});
  if (payload_length != 0) {
    auto payload_read = socket.RecvAll(std::span<std::byte>(out.payload.data(), payload_length));
    if (!payload_read.has_value()) return payload_read.error();
    if (!payload_read.value()) {
      return MakeError(ErrorCode::ProtocolError, "connection closed inside a frame payload");
    }
  }
  const std::uint32_t payload_crc = LoadLe32(header + 24);
  if (payload_crc != Crc32c(std::span<const std::byte>(out.payload))) {
    return MakeError(ErrorCode::ProtocolError, "frame payload checksum mismatch");
  }
  return true;
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalid; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = kInvalid;
  }
  return *this;
}

Socket::~Socket() { Close(); }

void Socket::Close() {
  if (handle_ == kInvalid) return;
  SOCKET socket = static_cast<SOCKET>(handle_);
  closesocket(socket);
  handle_ = kInvalid;
}

void Socket::ShutdownBoth() {
  if (handle_ == kInvalid) return;
  SOCKET socket = static_cast<SOCKET>(handle_);
  shutdown(socket, SD_BOTH);
}

Status Socket::SendAll(std::span<const std::byte> data) {
  if (handle_ == kInvalid) {
    return Fail(ErrorCode::TransportError, "send on an invalid socket");
  }
  SOCKET socket = static_cast<SOCKET>(handle_);
  std::size_t offset = 0;
  while (offset < data.size()) {
    const int want = static_cast<int>(
        std::min<std::size_t>(data.size() - offset, static_cast<std::size_t>(1) << 20));
    const int sent = send(socket, reinterpret_cast<const char*>(data.data() + offset), want, 0);
    if (sent == SOCKET_ERROR) {
      return Fail(ErrorCode::TransportError,
                  "socket send failed with error " + std::to_string(WSAGetLastError()));
    }
    if (sent == 0) {
      return Fail(ErrorCode::TransportError, "socket send returned zero bytes");
    }
    offset += static_cast<std::size_t>(sent);
  }
  return Status{};
}

Result<bool> Socket::RecvAll(std::span<std::byte> data) {
  if (handle_ == kInvalid) {
    return MakeError(ErrorCode::TransportError, "receive on an invalid socket");
  }
  SOCKET socket = static_cast<SOCKET>(handle_);
  std::size_t offset = 0;
  while (offset < data.size()) {
    const int want = static_cast<int>(
        std::min<std::size_t>(data.size() - offset, static_cast<std::size_t>(1) << 20));
    const int received =
        recv(socket, reinterpret_cast<char*>(data.data() + offset), want, 0);
    if (received == SOCKET_ERROR) {
      return MakeError(ErrorCode::TransportError,
                       "socket receive failed with error " + std::to_string(WSAGetLastError()));
    }
    if (received == 0) {
      if (offset == 0) return false;
      return MakeError(ErrorCode::TransportError, "connection closed mid-message");
    }
    offset += static_cast<std::size_t>(received);
  }
  return true;
}

Result<Socket> Socket::Connect(const std::string& address, std::uint16_t port) {
  Status runtime = EnsureSocketRuntime();
  if (!runtime.ok()) return runtime.error();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  const int rc = getaddrinfo(address.c_str(), service.c_str(), &hints, &result);
  if (rc != 0 || result == nullptr) {
    return MakeError(ErrorCode::TransportError, "cannot resolve registry service address",
                     address + ":" + service);
  }
  SOCKET socket = INVALID_SOCKET;
  for (addrinfo* candidate = result; candidate != nullptr; candidate = candidate->ai_next) {
    socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (socket == INVALID_SOCKET) continue;
    if (connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) break;
    closesocket(socket);
    socket = INVALID_SOCKET;
  }
  freeaddrinfo(result);
  if (socket == INVALID_SOCKET) {
    return MakeError(ErrorCode::TransportError, "cannot connect to the registry service",
                     address + ":" + service);
  }
  SetNoDelay(static_cast<std::uintptr_t>(socket), true);
  return Socket(static_cast<std::uintptr_t>(socket));
}

void Socket::SetNoDelay(std::uintptr_t handle, bool enable) {
  if (handle == kInvalidSocket) return;
  BOOL value = enable ? TRUE : FALSE;
  setsockopt(static_cast<SOCKET>(handle), IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char*>(&value), sizeof(value));
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_.load()), port_(other.port_) {
  other.handle_.store(kInvalid);
  other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    Close();
    handle_.store(other.handle_.load());
    port_ = other.port_;
    other.handle_.store(kInvalid);
    other.port_ = 0;
  }
  return *this;
}

TcpListener::~TcpListener() { Close(); }

Result<TcpListener> TcpListener::Bind(const std::string& address, std::uint16_t port,
                                      std::size_t backlog) {
  Status runtime = EnsureSocketRuntime();
  if (!runtime.ok()) return runtime.error();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  const int rc = getaddrinfo(address.empty() ? nullptr : address.c_str(), service.c_str(), &hints,
                             &result);
  if (rc != 0 || result == nullptr) {
    return MakeError(ErrorCode::TransportError, "cannot resolve bind address",
                     address + ":" + service);
  }
  SOCKET socket = INVALID_SOCKET;
  for (addrinfo* candidate = result; candidate != nullptr; candidate = candidate->ai_next) {
    socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (socket == INVALID_SOCKET) continue;
    const BOOL exclusive = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    if (bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) break;
    closesocket(socket);
    socket = INVALID_SOCKET;
  }
  freeaddrinfo(result);
  if (socket == INVALID_SOCKET) {
    return MakeError(ErrorCode::TransportError, "cannot bind the registry listener",
                     address + ":" + service);
  }
  if (listen(socket, static_cast<int>(std::min<std::size_t>(backlog, 1024))) != 0) {
    closesocket(socket);
    return MakeError(ErrorCode::TransportError, "cannot listen on the registry socket");
  }
  sockaddr_in bound{};
  int bound_length = sizeof(bound);
  TcpListener listener;
  listener.handle_.store(static_cast<std::uintptr_t>(socket));
  if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
    listener.port_ = ntohs(bound.sin_port);
  } else {
    listener.port_ = port;
  }
  return listener;
}

void TcpListener::Close() {
  const std::uintptr_t handle = handle_.exchange(kInvalid);
  if (handle == kInvalid) return;
  closesocket(static_cast<SOCKET>(handle));
}

Result<Socket> TcpListener::Accept() {
  // The handle is loaded once and used; Close() exchanges it away first, so a
  // concurrently closed listener can never hand a recycled handle to accept().
  const std::uintptr_t handle = handle_.load();
  if (handle == kInvalid) {
    return MakeError(ErrorCode::TransportError, "accept on an invalid listener");
  }
  SOCKET accepted = accept(static_cast<SOCKET>(handle), nullptr, nullptr);
  if (accepted == INVALID_SOCKET) {
    return MakeError(ErrorCode::TransportError,
                     "accept failed with error " + std::to_string(WSAGetLastError()));
  }
  Socket::SetNoDelay(static_cast<std::uintptr_t>(accepted), true);
  return Socket(static_cast<std::uintptr_t>(accepted));
}

}  // namespace fcr
