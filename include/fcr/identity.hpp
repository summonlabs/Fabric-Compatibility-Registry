// Fabric Compatibility Registry - Summon Software Labs
// Strongly typed domain identities: identifiers, counters, digests, epochs.
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "fcr/error.hpp"

namespace fcr {

// ---------------------------------------------------------------------------
// Identifier grammar
// ---------------------------------------------------------------------------
// QualifiedName: [A-Za-z0-9]([A-Za-z0-9._-]{0,62}[A-Za-z0-9])?
//   ASCII only. Separators may not lead, trail, or repeat. Maximum 64 bytes.
// Opaque: printable ASCII (0x21..0x7e), maximum 64 bytes.
//
// Every domain object carries its own tag type, so a capability id can never be
// passed where a component kind id is expected.
enum class IdGrammar { QualifiedName, Opaque };

#define FCR_DECLARE_ID_TAG(TagName, LabelText, GrammarKind)       \
  struct TagName {                                                \
    static constexpr std::string_view kLabel = LabelText;         \
    static constexpr IdGrammar kGrammar = IdGrammar::GrammarKind; \
  }

FCR_DECLARE_ID_TAG(ComponentFamilyTag, "component family", QualifiedName);
FCR_DECLARE_ID_TAG(ComponentKindTag, "component kind", QualifiedName);
FCR_DECLARE_ID_TAG(ProtocolTag, "protocol", QualifiedName);
FCR_DECLARE_ID_TAG(SchemaTag, "schema", QualifiedName);
FCR_DECLARE_ID_TAG(CapabilityTag, "capability", QualifiedName);
FCR_DECLARE_ID_TAG(FeatureTag, "feature", QualifiedName);
FCR_DECLARE_ID_TAG(HardwareClassTag, "hardware class", QualifiedName);
FCR_DECLARE_ID_TAG(RuleTag, "rule", QualifiedName);
FCR_DECLARE_ID_TAG(PublisherTag, "publisher", Opaque);
FCR_DECLARE_ID_TAG(InstanceTag, "component instance", Opaque);
FCR_DECLARE_ID_TAG(VendorTag, "vendor", Opaque);

#undef FCR_DECLARE_ID_TAG

template <class Tag>
Status ValidateIdText(std::string_view text) {
  if (text.empty()) {
    return Fail(ErrorCode::InvalidIdentifier, std::string("empty ") + std::string(Tag::kLabel));
  }
  if (text.size() > 64) {
    return Fail(ErrorCode::InvalidIdentifier, std::string(Tag::kLabel) + " exceeds 64 bytes");
  }
  const auto alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  };
  if constexpr (Tag::kGrammar == IdGrammar::Opaque) {
    for (char c : text) {
      const unsigned char u = static_cast<unsigned char>(c);
      if (u < 0x21 || u > 0x7e) {
        return Fail(ErrorCode::InvalidIdentifier,
                    std::string(Tag::kLabel) + " contains non-printable or whitespace characters");
      }
    }
    return Status{};
  } else {
    if (!alnum(text.front()) || !alnum(text.back())) {
      return Fail(ErrorCode::InvalidIdentifier,
                  std::string(Tag::kLabel) + " must start and end with an alphanumeric character");
    }
    bool previous_separator = false;
    for (char c : text) {
      const bool separator = (c == '.' || c == '_' || c == '-');
      if (!alnum(c) && !separator) {
        return Fail(ErrorCode::InvalidIdentifier,
                    std::string(Tag::kLabel) +
                        " contains an illegal character (allowed: a-z A-Z 0-9 . _ -)");
      }
      if (separator && previous_separator) {
        return Fail(ErrorCode::InvalidIdentifier,
                    std::string(Tag::kLabel) + " contains consecutive separators");
      }
      previous_separator = separator;
    }
    return Status{};
  }
}

template <class Tag>
class TypedId {
 public:
  TypedId() = default;

  static Result<TypedId> Parse(std::string_view text) {
    Status status = ValidateIdText<Tag>(text);
    if (!status.ok()) return status.error();
    TypedId id;
    id.value_.assign(text);
    return id;
  }

  const std::string& value() const noexcept { return value_; }
  bool empty() const noexcept { return value_.empty(); }

  friend bool operator==(const TypedId& a, const TypedId& b) { return a.value_ == b.value_; }
  friend bool operator!=(const TypedId& a, const TypedId& b) { return a.value_ != b.value_; }
  friend bool operator<(const TypedId& a, const TypedId& b) { return a.value_ < b.value_; }
  friend bool operator>(const TypedId& a, const TypedId& b) { return b.value_ < a.value_; }
  friend bool operator<=(const TypedId& a, const TypedId& b) { return !(b.value_ < a.value_); }
  friend bool operator>=(const TypedId& a, const TypedId& b) { return !(a.value_ < b.value_); }
  friend std::strong_ordering operator<=>(const TypedId& a, const TypedId& b) {
    return a.value_ <=> b.value_;
  }

 private:
  std::string value_;
};

using ComponentFamilyId = TypedId<ComponentFamilyTag>;
using ComponentKindId = TypedId<ComponentKindTag>;
using ProtocolId = TypedId<ProtocolTag>;
using SchemaId = TypedId<SchemaTag>;
using CapabilityId = TypedId<CapabilityTag>;
using FeatureId = TypedId<FeatureTag>;
using HardwareClassId = TypedId<HardwareClassTag>;
using RuleId = TypedId<RuleTag>;
using PublisherId = TypedId<PublisherTag>;
using ComponentInstanceId = TypedId<InstanceTag>;
using VendorId = TypedId<VendorTag>;

// ---------------------------------------------------------------------------
// Strong counters
// ---------------------------------------------------------------------------
template <class Tag, class Rep>
class StrongCounter {
 public:
  using RepType = Rep;
  constexpr StrongCounter() = default;
  constexpr explicit StrongCounter(Rep value) : value_(value) {}
  constexpr Rep value() const noexcept { return value_; }
  constexpr bool IsZero() const noexcept { return value_ == Rep{0}; }

  friend constexpr bool operator==(StrongCounter a, StrongCounter b) { return a.value_ == b.value_; }
  friend constexpr bool operator!=(StrongCounter a, StrongCounter b) { return a.value_ != b.value_; }
  friend constexpr bool operator<(StrongCounter a, StrongCounter b) { return a.value_ < b.value_; }
  friend constexpr bool operator>(StrongCounter a, StrongCounter b) { return b.value_ < a.value_; }
  friend constexpr bool operator<=(StrongCounter a, StrongCounter b) { return !(b.value_ < a.value_); }
  friend constexpr bool operator>=(StrongCounter a, StrongCounter b) { return !(a.value_ < b.value_); }
  friend constexpr std::strong_ordering operator<=>(StrongCounter a, StrongCounter b) {
    return a.value_ <=> b.value_;
  }

 private:
  Rep value_{0};
};

struct GenerationNumberTag {
  static constexpr std::string_view kLabel = "registry generation";
};
struct PublisherEpochTag {
  static constexpr std::string_view kLabel = "publisher epoch";
};
struct IncarnationTag {
  static constexpr std::string_view kLabel = "process incarnation";
};
struct RuleRevisionTag {
  static constexpr std::string_view kLabel = "rule revision";
};
struct RulePriorityTag {
  static constexpr std::string_view kLabel = "rule priority";
};

using GenerationNumber = StrongCounter<GenerationNumberTag, std::uint64_t>;
using PublisherEpoch = StrongCounter<PublisherEpochTag, std::uint64_t>;
using Incarnation = StrongCounter<IncarnationTag, std::uint64_t>;
using RuleRevision = StrongCounter<RuleRevisionTag, std::uint32_t>;
using RulePriority = StrongCounter<RulePriorityTag, std::int32_t>;

inline Result<GenerationNumber> NextGeneration(GenerationNumber g) {
  if (g.value() == UINT64_MAX) {
    return MakeError(ErrorCode::ArithmeticOverflow, "registry generation counter exhausted");
  }
  return GenerationNumber(g.value() + 1);
}

inline Result<Incarnation> NextIncarnation(Incarnation i) {
  if (i.value() == UINT64_MAX) {
    return MakeError(ErrorCode::ArithmeticOverflow, "process incarnation counter exhausted");
  }
  return Incarnation(i.value() + 1);
}

inline Result<PublisherEpoch> NextEpoch(PublisherEpoch e) {
  if (e.value() == UINT64_MAX) {
    return MakeError(ErrorCode::ArithmeticOverflow, "publisher epoch counter exhausted");
  }
  return PublisherEpoch(e.value() + 1);
}

// ---------------------------------------------------------------------------
// Content digest (SHA-256)
// ---------------------------------------------------------------------------
class ContentDigest {
 public:
  static constexpr std::size_t kSize = 32;

  ContentDigest() = default;
  static ContentDigest FromBytes(const std::array<std::byte, kSize>& bytes);
  static Result<ContentDigest> FromHex(std::string_view hex);
  static ContentDigest Zero() { return ContentDigest{}; }

  const std::array<std::byte, kSize>& bytes() const noexcept { return bytes_; }
  std::string ToHex() const;
  bool IsZero() const;

  friend bool operator==(const ContentDigest& a, const ContentDigest& b) {
    return a.bytes_ == b.bytes_;
  }
  friend bool operator!=(const ContentDigest& a, const ContentDigest& b) {
    return a.bytes_ != b.bytes_;
  }
  friend bool operator<(const ContentDigest& a, const ContentDigest& b) {
    return a.bytes_ < b.bytes_;
  }

 private:
  std::array<std::byte, kSize> bytes_{};
};

// ---------------------------------------------------------------------------
// Timestamps (explicit, never implicitly "now")
// ---------------------------------------------------------------------------
class Timestamp {
 public:
  Timestamp() = default;
  static constexpr Timestamp FromNanos(std::int64_t nanos) { return Timestamp(nanos); }
  static Result<Timestamp> FromIso8601(std::string_view text);

  constexpr std::int64_t nanos() const noexcept { return nanos_; }
  std::string ToIso8601() const;

  friend constexpr bool operator==(Timestamp a, Timestamp b) { return a.nanos_ == b.nanos_; }
  friend constexpr bool operator<(Timestamp a, Timestamp b) { return a.nanos_ < b.nanos_; }

 private:
  constexpr explicit Timestamp(std::int64_t nanos) : nanos_(nanos) {}
  std::int64_t nanos_ = 0;
};

// ---------------------------------------------------------------------------
// Composite identities
// ---------------------------------------------------------------------------
struct RuleRef {
  RuleId id;
  RuleRevision revision;

  friend bool operator==(const RuleRef& a, const RuleRef& b) {
    return a.id == b.id && a.revision == b.revision;
  }
  friend bool operator<(const RuleRef& a, const RuleRef& b) {
    if (a.id != b.id) return a.id < b.id;
    return a.revision < b.revision;
  }
  std::string ToString() const { return id.value() + "@r" + std::to_string(revision.value()); }
};

// A registry generation is identified by its ordinal *and* its content digest.
struct GenerationId {
  GenerationNumber number;
  ContentDigest digest;

  bool IsValid() const { return !digest.IsZero(); }
  std::string ToString() const {
    return "gen-" + std::to_string(number.value()) + ":" + digest.ToHex();
  }
  friend bool operator==(const GenerationId& a, const GenerationId& b) {
    return a.number == b.number && a.digest == b.digest;
  }
  friend bool operator<(const GenerationId& a, const GenerationId& b) {
    if (a.number != b.number) return a.number < b.number;
    return a.digest < b.digest;
  }
};

// Inverse of GenerationId::ToString: "gen-<number>:<64 hex characters>".
Result<GenerationId> ParseGenerationId(std::string_view text);

// Process incarnation plus the generation that incarnation published.
struct IncarnationStamp {
  Incarnation incarnation;
  GenerationId generation;

  friend bool operator==(const IncarnationStamp& a, const IncarnationStamp& b) {
    return a.incarnation == b.incarnation && a.generation == b.generation;
  }
};

}  // namespace fcr

namespace std {
template <class Tag>
struct hash<fcr::TypedId<Tag>> {
  std::size_t operator()(const fcr::TypedId<Tag>& id) const noexcept {
    return std::hash<std::string>{}(id.value());
  }
};
}  // namespace std
