// Fabric Compatibility Registry - Summon Software Labs
// Semantic versions (SemVer 2.0.0) and canonical version ranges.
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fcr/error.hpp"

namespace fcr {

// A SemVer 2.0.0 version with checked 32-bit numeric components.
//
// Ordering surfaces are deliberately separate:
//   * Precedence() implements SemVer 2.0.0 precedence (build metadata ignored).
//   * operator< etc. implement a total order used only for canonical sorting,
//     where build metadata is the final tie-break.
// Range membership always uses Precedence().
class SemVersion {
 public:
  SemVersion() = default;
  SemVersion(std::uint32_t major, std::uint32_t minor, std::uint32_t patch)
      : major_(major), minor_(minor), patch_(patch) {}

  // Strict parse: exactly major.minor.patch with optional -prerelease and
  // +build. No wildcards, no leading zeros, no sign characters.
  static Result<SemVersion> Parse(std::string_view text);

  std::uint32_t major() const noexcept { return major_; }
  std::uint32_t minor() const noexcept { return minor_; }
  std::uint32_t patch() const noexcept { return patch_; }
  const std::vector<std::string>& prerelease() const noexcept { return prerelease_; }
  const std::string& build() const noexcept { return build_; }
  bool has_prerelease() const noexcept { return !prerelease_.empty(); }
  bool has_build() const noexcept { return !build_.empty(); }

  // "1.2.3-rc.1+build.5"
  std::string ToString() const;

  // SemVer 2.0.0 precedence comparison (build metadata ignored).
  std::strong_ordering Precedence(const SemVersion& other) const;

  // Total order: precedence first, then build metadata lexicographically.
  // Used for deterministic canonical ordering of collections only.
  std::strong_ordering TotalOrder(const SemVersion& other) const;

  // Version identity for compatibility purposes: equal precedence (build
  // metadata is not part of compatibility meaning).
  bool SameRelease(const SemVersion& other) const {
    return Precedence(other) == std::strong_ordering::equal;
  }

  friend bool operator==(const SemVersion& a, const SemVersion& b) { return a.SameRelease(b); }
  friend bool operator!=(const SemVersion& a, const SemVersion& b) { return !a.SameRelease(b); }
  friend bool operator<(const SemVersion& a, const SemVersion& b) {
    return a.Precedence(b) == std::strong_ordering::less;
  }
  friend bool operator>(const SemVersion& a, const SemVersion& b) { return b < a; }
  friend bool operator<=(const SemVersion& a, const SemVersion& b) { return !(b < a); }
  friend bool operator>=(const SemVersion& a, const SemVersion& b) { return !(a < b); }

 private:
  std::uint32_t major_ = 0;
  std::uint32_t minor_ = 0;
  std::uint32_t patch_ = 0;
  std::vector<std::string> prerelease_;
  std::string build_;
};

// A single half-open/closed interval over SemVer precedence.
//   nullopt low  => -infinity,  nullopt high => +infinity
struct VersionInterval {
  std::optional<SemVersion> low;
  bool low_inclusive = true;
  std::optional<SemVersion> high;
  bool high_inclusive = true;

  static VersionInterval Any() { return VersionInterval{}; }
  static VersionInterval Exact(const SemVersion& v) {
    VersionInterval i;
    i.low = v;
    i.high = v;
    return i;
  }

  // Structural emptiness: no SemVer precedence value can fall inside.
  bool StructurallyEmpty() const;
  // True when the interval pins exactly one release.
  bool IsExact() const;
  // Structural containment (does not consider prerelease admission policy).
  bool ContainsBound(const SemVersion& v) const;
  bool Contains(const SemVersion& v) const;
  bool Subsumes(const VersionInterval& other) const;
  std::string ToString() const;

  friend bool operator==(const VersionInterval& a, const VersionInterval& b);
};

// A canonical, sorted, disjoint union of intervals. An empty interval list
// matches no version at all (it is *not* the same as "any").
class VersionRange {
 public:
  VersionRange() = default;  // empty range

  static VersionRange Any();
  static VersionRange Empty() { return VersionRange{}; }
  static VersionRange Exact(const SemVersion& v);
  static Result<VersionRange> Parse(std::string_view text);
  static Result<VersionRange> FromIntervals(std::vector<VersionInterval> intervals);

  bool IsEmpty() const noexcept { return intervals_.empty(); }
  bool IsAny() const;
  bool IsExact() const;
  std::optional<SemVersion> ExactVersion() const;
  const std::vector<VersionInterval>& intervals() const noexcept { return intervals_; }

  bool Contains(const SemVersion& v) const;

  // Union / intersection of canonical ranges.
  static VersionRange Union(const VersionRange& a, const VersionRange& b);
  static VersionRange Intersect(const VersionRange& a, const VersionRange& b);
  static bool Overlaps(const VersionRange& a, const VersionRange& b);

  // True when every version admitted by other is admitted by *this.
  bool Subsumes(const VersionRange& other) const;

  // Canonical round-trippable textual form.
  std::string ToString() const;

  friend bool operator==(const VersionRange& a, const VersionRange& b) {
    return a.intervals_ == b.intervals_;
  }

 private:
  std::vector<VersionInterval> intervals_;
};

}  // namespace fcr
