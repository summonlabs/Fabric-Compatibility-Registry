#include "fcr/error.hpp"

namespace fcr {

std::string_view ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::InvalidIdentifier: return "invalid_identifier";
    case ErrorCode::InvalidVersion: return "invalid_version";
    case ErrorCode::InvalidRange: return "invalid_range";
    case ErrorCode::InvalidJson: return "invalid_json";
    case ErrorCode::InvalidDocument: return "invalid_document";
    case ErrorCode::InvalidCapabilityValue: return "invalid_capability_value";
    case ErrorCode::InvalidConstraint: return "invalid_constraint";
    case ErrorCode::InvalidSelection: return "invalid_selection";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::DuplicateIdentity: return "duplicate_identity";
    case ErrorCode::ConflictingRules: return "conflicting_rules";
    case ErrorCode::ValidationFailed: return "validation_failed";
    case ErrorCode::UnsatisfiableConstraint: return "unsatisfiable_constraint";
    case ErrorCode::UnreachableRule: return "unreachable_rule";
    case ErrorCode::CycleDetected: return "cycle_detected";
    case ErrorCode::UndefinedReference: return "undefined_reference";
    case ErrorCode::ImmutableGeneration: return "immutable_generation";
    case ErrorCode::DigestMismatch: return "digest_mismatch";
    case ErrorCode::CorruptPersistence: return "corrupt_persistence";
    case ErrorCode::UnsupportedFormatVersion: return "unsupported_format_version";
    case ErrorCode::StaleGeneration: return "stale_generation";
    case ErrorCode::StaleEpoch: return "stale_epoch";
    case ErrorCode::StaleIncarnation: return "stale_incarnation";
    case ErrorCode::GenerationLimitReached: return "generation_limit_reached";
    case ErrorCode::ParentDigestMismatch: return "parent_digest_mismatch";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::ShuttingDown: return "shutting_down";
    case ErrorCode::ResourceExhausted: return "resource_exhausted";
    case ErrorCode::LimitExceeded: return "limit_exceeded";
    case ErrorCode::ArithmeticOverflow: return "arithmetic_overflow";
    case ErrorCode::TransportError: return "transport_error";
    case ErrorCode::ProtocolError: return "protocol_error";
    case ErrorCode::HandshakeFailed: return "handshake_failed";
    case ErrorCode::ConnectionLimitReached: return "connection_limit_reached";
    case ErrorCode::IoError: return "io_error";
    case ErrorCode::LockContention: return "lock_contention";
    case ErrorCode::Internal: return "internal";
  }
  return "unknown";
}

ErrorCode ErrorCodeFromName(std::string_view name) noexcept {
  for (int i = 0; i <= static_cast<int>(ErrorCode::Internal); ++i) {
    const ErrorCode code = static_cast<ErrorCode>(i);
    if (ErrorCodeName(code) == name) return code;
  }
  return ErrorCode::Internal;
}

std::string Error::ToString() const {
  std::string out;
  out.reserve(message.size() + detail.size() + 32);
  out.append(ErrorCodeName(code));
  out.append(": ");
  out.append(message);
  if (!detail.empty()) {
    out.append(" [");
    out.append(detail);
    out.append("]");
  }
  return out;
}

}  // namespace fcr
