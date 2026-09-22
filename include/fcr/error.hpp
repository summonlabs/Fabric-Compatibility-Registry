// Fabric Compatibility Registry - Summon Software Labs
// Strongly typed error and result plumbing.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace fcr {

// Every failure mode of the runtime is named. Callers never parse strings to
// branch on failure: they branch on ErrorCode.
enum class ErrorCode : int {
  Ok = 0,
  InvalidArgument,
  InvalidIdentifier,
  InvalidVersion,
  InvalidRange,
  InvalidJson,
  InvalidDocument,
  InvalidCapabilityValue,
  InvalidConstraint,
  InvalidSelection,
  NotFound,
  DuplicateIdentity,
  ConflictingRules,
  ValidationFailed,
  UnsatisfiableConstraint,
  UnreachableRule,
  CycleDetected,
  UndefinedReference,
  ImmutableGeneration,
  DigestMismatch,
  CorruptPersistence,
  UnsupportedFormatVersion,
  StaleGeneration,
  StaleEpoch,
  StaleIncarnation,
  GenerationLimitReached,
  ParentDigestMismatch,
  Cancelled,
  ShuttingDown,
  ResourceExhausted,
  LimitExceeded,
  ArithmeticOverflow,
  TransportError,
  ProtocolError,
  HandshakeFailed,
  ConnectionLimitReached,
  IoError,
  LockContention,
  Internal,
};

std::string_view ErrorCodeName(ErrorCode code) noexcept;
// Inverse of ErrorCodeName; used when a failure crosses a transport boundary.
ErrorCode ErrorCodeFromName(std::string_view name) noexcept;

struct Error {
  ErrorCode code = ErrorCode::Internal;
  std::string message;
  std::string detail;

  Error() = default;
  Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
  Error(ErrorCode c, std::string msg, std::string det)
      : code(c), message(std::move(msg)), detail(std::move(det)) {}

  std::string ToString() const;
};

inline Error MakeError(ErrorCode code, std::string message) {
  return Error(code, std::move(message));
}

inline Error MakeError(ErrorCode code, std::string message, std::string detail) {
  return Error(code, std::move(message), std::move(detail));
}

// Result<T> is a value-or-error. T must be movable.
//
// value() and operator* are only valid when has_value() is true; calling them
// on a failed result throws std::bad_variant_access, exactly as a checked
// variant access would. Every runtime path returns an Error value instead of
// throwing, so a caller only ever sees that exception through its own
// unchecked access.
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT

  bool has_value() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  T& value() & { return std::get<0>(storage_); }
  const T& value() const& { return std::get<0>(storage_); }
  T&& value() && { return std::get<0>(std::move(storage_)); }
  T* operator->() { return &std::get<0>(storage_); }
  const T* operator->() const { return &std::get<0>(storage_); }
  T& operator*() & { return std::get<0>(storage_); }
  const T& operator*() const& { return std::get<0>(storage_); }

  const Error& error() const& { return std::get<1>(storage_); }
  Error& error() & { return std::get<1>(storage_); }
  Error&& error() && { return std::get<1>(std::move(storage_)); }

  ErrorCode code() const { return std::get<1>(storage_).code; }

  template <class U>
  T value_or(U&& fallback) const {
    return has_value() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  std::variant<T, Error> storage_;
};

class Status {
 public:
  Status() = default;
  Status(Error error) : error_(std::move(error)), ok_(false) {}  // NOLINT

  bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }
  const Error& error() const { return error_; }
  ErrorCode code() const { return ok_ ? ErrorCode::Ok : error_.code; }

 private:
  Error error_{};
  bool ok_ = true;
};

// Convenience for constructing a failure Status from a code + message.
inline Status Fail(ErrorCode code, std::string message) {
  return Status(Error(code, std::move(message)));
}

inline Status Fail(ErrorCode code, std::string message, std::string detail) {
  return Status(Error(code, std::move(message), std::move(detail)));
}

}  // namespace fcr
