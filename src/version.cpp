#include "fcr/version.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <limits>
#include <sstream>

namespace fcr {
namespace {

constexpr std::uint32_t kMaxComponent = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kMaxVersionTextLength = 512;
constexpr std::size_t kMaxPrereleaseIdentifiers = 32;
constexpr std::size_t kMaxRangeTextLength = 4096;
constexpr std::size_t kMaxRangeGroups = 64;
constexpr std::size_t kMaxComparatorsPerGroup = 64;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsAlphaNumHyphen(char c) {
  return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

bool IsAllDigits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!IsDigit(c)) return false;
  }
  return true;
}

Result<std::uint32_t> ParseU32Component(std::string_view text) {
  if (text.empty()) {
    return MakeError(ErrorCode::InvalidVersion, "empty numeric version component");
  }
  if (text.size() > 10) {
    return MakeError(ErrorCode::InvalidVersion,
                     "numeric version component exceeds 32-bit range",
                     std::string(text));
  }
  if (text.size() > 1 && text.front() == '0') {
    return MakeError(ErrorCode::InvalidVersion,
                     "numeric version component has a leading zero", std::string(text));
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (!IsDigit(c)) {
      return MakeError(ErrorCode::InvalidVersion, "numeric version component is not decimal",
                       std::string(text));
    }
    value = value * 10u + static_cast<std::uint64_t>(c - '0');
  }
  if (value > kMaxComponent) {
    return MakeError(ErrorCode::InvalidVersion,
                     "numeric version component exceeds 32-bit range", std::string(text));
  }
  return static_cast<std::uint32_t>(value);
}

Result<std::vector<std::string>> SplitIdentifiers(std::string_view text, bool prerelease) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (true) {
    const std::size_t dot = text.find('.', start);
    const std::string_view piece =
        dot == std::string_view::npos ? text.substr(start) : text.substr(start, dot - start);
    if (piece.empty()) {
      return MakeError(prerelease ? ErrorCode::InvalidVersion : ErrorCode::InvalidVersion,
                       "empty dotted identifier in version", std::string(text));
    }
    for (char c : piece) {
      if (!IsAlphaNumHyphen(c)) {
        return MakeError(ErrorCode::InvalidVersion, "illegal character in version identifier",
                         std::string(piece));
      }
    }
    if (prerelease && IsAllDigits(piece) && piece.size() > 1 && piece.front() == '0') {
      return MakeError(ErrorCode::InvalidVersion,
                       "numeric prerelease identifier has a leading zero", std::string(piece));
    }
    if (out.size() >= kMaxPrereleaseIdentifiers) {
      return MakeError(ErrorCode::LimitExceeded, "too many version identifiers",
                       std::string(text));
    }
    out.emplace_back(piece);
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return out;
}

std::strong_ordering ComparePrerelease(const std::vector<std::string>& a,
                                       const std::vector<std::string>& b) {
  // SemVer 2.0.0: no prerelease outranks having a prerelease.
  if (a.empty() && b.empty()) return std::strong_ordering::equal;
  if (a.empty()) return std::strong_ordering::greater;
  if (b.empty()) return std::strong_ordering::less;
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const bool an = IsAllDigits(a[i]);
    const bool bn = IsAllDigits(b[i]);
    if (an && bn) {
      if (a[i].size() != b[i].size()) return a[i].size() < b[i].size() ? std::strong_ordering::less
                                                                       : std::strong_ordering::greater;
      const int cmp = a[i].compare(b[i]);
      if (cmp != 0) return cmp < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
    } else if (an != bn) {
      return an ? std::strong_ordering::less : std::strong_ordering::greater;
    } else {
      const int cmp = a[i].compare(b[i]);
      if (cmp != 0) return cmp < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
    }
  }
  if (a.size() == b.size()) return std::strong_ordering::equal;
  return a.size() < b.size() ? std::strong_ordering::less : std::strong_ordering::greater;
}

struct PartialVersion {
  std::optional<std::uint32_t> major;
  std::optional<std::uint32_t> minor;
  std::optional<std::uint32_t> patch;
  std::vector<std::string> prerelease;
  bool wildcard_tail = false;

  bool is_full() const { return major.has_value() && minor.has_value() && patch.has_value(); }
  SemVersion Filled() const {
    return SemVersion(major.value_or(0), minor.value_or(0), patch.value_or(0));
  }
  SemVersion FilledWithPrerelease() const {
    SemVersion v = Filled();
    if (!prerelease.empty()) {
      // Rebuild through text so the parsed prerelease travels with the value.
      std::string text = v.ToString();
      text.push_back('-');
      for (std::size_t i = 0; i < prerelease.size(); ++i) {
        if (i != 0) text.push_back('.');
        text.append(prerelease[i]);
      }
      auto parsed = SemVersion::Parse(text);
      if (parsed.has_value()) return parsed.value();
    }
    return v;
  }
};

// Parses "1", "1.2", "1.2.3", "1.*", "1.2.x", "1.2.3-rc.1". Wildcards and
// partial forms are only legal where the caller permits them.
Result<PartialVersion> ParsePartial(std::string_view text) {
  PartialVersion pv;
  if (text.empty()) {
    return MakeError(ErrorCode::InvalidVersion, "empty version text");
  }
  if (text.size() > kMaxVersionTextLength) {
    return MakeError(ErrorCode::LimitExceeded, "version text too long", std::string(text.substr(0, 32)));
  }
  std::string_view core = text;
  const std::size_t plus = core.find('+');
  if (plus != std::string_view::npos) {
    const std::string_view build = core.substr(plus + 1);
    if (build.empty()) {
      return MakeError(ErrorCode::InvalidVersion, "empty build metadata", std::string(text));
    }
    auto build_ids = SplitIdentifiers(build, false);
    if (!build_ids.has_value()) return build_ids.error();
    core = core.substr(0, plus);
  }
  const std::size_t dash = core.find('-');
  if (dash != std::string_view::npos) {
    const std::string_view pre = core.substr(dash + 1);
    auto ids = SplitIdentifiers(pre, true);
    if (!ids.has_value()) return ids.error();
    pv.prerelease = std::move(ids).value();
    core = core.substr(0, dash);
  }
  std::size_t index = 0;
  std::optional<std::uint32_t>* slots[3] = {&pv.major, &pv.minor, &pv.patch};
  std::size_t slot = 0;
  std::size_t start = 0;
  while (true) {
    const std::size_t dot = core.find('.', start);
    const std::string_view piece =
        dot == std::string_view::npos ? core.substr(start) : core.substr(start, dot - start);
    if (piece.empty()) {
      return MakeError(ErrorCode::InvalidVersion, "empty version component", std::string(text));
    }
    if (slot >= 3) {
      return MakeError(ErrorCode::InvalidVersion, "too many version components", std::string(text));
    }
    if (piece == "*" || piece == "x" || piece == "X") {
      if (dot != std::string_view::npos) {
        return MakeError(ErrorCode::InvalidVersion,
                         "version component after wildcard", std::string(text));
      }
      pv.wildcard_tail = true;
      break;
    }
    if (pv.wildcard_tail) {
      return MakeError(ErrorCode::InvalidVersion,
                       "version component after wildcard", std::string(text));
    }
    auto parsed = ParseU32Component(piece);
    if (!parsed.has_value()) return parsed.error();
    *slots[slot] = parsed.value();
    ++slot;
    ++index;
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  if (index == 0 && !pv.wildcard_tail) {
    return MakeError(ErrorCode::InvalidVersion, "version has no numeric components",
                     std::string(text));
  }
  if (!pv.prerelease.empty() && !pv.is_full()) {
    return MakeError(ErrorCode::InvalidVersion,
                     "prerelease requires a full major.minor.patch version", std::string(text));
  }
  return pv;
}

std::vector<std::string> Tokenize(std::string_view text) {
  std::vector<std::string> tokens;
  std::string current;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) tokens.push_back(current);
  return tokens;
}

// Builds the interval set for one comparator token such as ">=1.2.3" or "^1.0".
Result<VersionRange> ComparatorToRange(std::string_view token) {
  std::string_view op;
  std::string_view rest = token;
  if (token.size() >= 2 && (token.substr(0, 2) == ">=" || token.substr(0, 2) == "<=")) {
    op = token.substr(0, 2);
    rest = token.substr(2);
  } else if (!token.empty() && (token[0] == '>' || token[0] == '<' || token[0] == '=' ||
                                token[0] == '^' || token[0] == '~')) {
    op = token.substr(0, 1);
    rest = token.substr(1);
  }
  auto parsed = ParsePartial(rest);
  if (!parsed.has_value()) {
    return MakeError(ErrorCode::InvalidRange, "invalid version in range", std::string(token));
  }
  const PartialVersion& pv = parsed.value();
  if (op.empty()) {
    if (pv.wildcard_tail) op = "^partial-wildcard";
  }
  if (op.empty() || op == "=") {
    if (pv.is_full()) {
      return VersionRange::Exact(pv.FilledWithPrerelease());
    }
    // A partial version denotes the band it names: "1.2" and "1.2.*" both mean
    // >=1.2.0 <1.3.0, and "1" and "1.*" both mean >=1.0.0 <2.0.0.
    if (pv.major.has_value() && pv.minor.has_value()) {
      return VersionRange::Parse("~" + std::to_string(*pv.major) + "." + std::to_string(*pv.minor));
    }
    if (pv.major.has_value()) {
      return VersionRange::Parse("~" + std::to_string(*pv.major));
    }
    return VersionRange::Any();
  }
  if (op == "*") {
    return VersionRange::Any();
  }
  if (op == "^") {
    if (!pv.major.has_value()) return VersionRange::Any();
    const std::uint32_t maj = *pv.major;
    const std::uint32_t min = pv.minor.value_or(0);
    const std::uint32_t pat = pv.patch.value_or(0);
    VersionInterval iv;
    iv.low = SemVersion(maj, min, pat);
    if (pv.is_full() && !pv.prerelease.empty()) iv.low = pv.FilledWithPrerelease();
    if (maj > 0) {
      iv.high = SemVersion(maj + 1, 0, 0);
    } else if (pv.minor.has_value() && min > 0) {
      iv.high = SemVersion(0, min + 1, 0);
    } else if (pv.is_full()) {
      iv.high = SemVersion(0, min, pat + 1);
    } else if (pv.minor.has_value()) {
      iv.high = SemVersion(0, 1, 0);
    } else {
      iv.high = SemVersion(1, 0, 0);
    }
    iv.high_inclusive = false;
    return VersionRange::FromIntervals({iv});
  }
  if (op == "~") {
    if (!pv.major.has_value()) return VersionRange::Any();
    const std::uint32_t maj = *pv.major;
    const std::uint32_t min = pv.minor.value_or(0);
    const std::uint32_t pat = pv.patch.value_or(0);
    VersionInterval iv;
    iv.low = SemVersion(maj, min, pat);
    if (pv.is_full() && !pv.prerelease.empty()) iv.low = pv.FilledWithPrerelease();
    if (pv.minor.has_value()) {
      iv.high = SemVersion(maj, min + 1, 0);
    } else {
      iv.high = SemVersion(maj + 1, 0, 0);
    }
    iv.high_inclusive = false;
    return VersionRange::FromIntervals({iv});
  }
  if (op == "^partial-wildcard") {
    if (!pv.major.has_value()) return VersionRange::Any();
    std::string rebuilt = "~" + std::to_string(*pv.major);
    if (pv.minor.has_value()) rebuilt += "." + std::to_string(*pv.minor);
    return VersionRange::Parse(rebuilt);
  }
  VersionInterval iv;
  if (op == ">") {
    if (pv.is_full()) {
      iv.low = pv.FilledWithPrerelease();
      iv.low_inclusive = false;
    } else if (pv.minor.has_value()) {
      iv.low = SemVersion(*pv.major, *pv.minor + 1, 0);
    } else if (pv.major.has_value()) {
      iv.low = SemVersion(*pv.major + 1, 0, 0);
    }
  } else if (op == ">=") {
    iv.low = pv.FilledWithPrerelease();
  } else if (op == "<") {
    if (pv.is_full()) {
      iv.high = pv.FilledWithPrerelease();
      iv.high_inclusive = false;
    } else {
      iv.high = SemVersion(*pv.major, pv.minor.value_or(0), 0);
      iv.high_inclusive = false;
    }
  } else if (op == "<=") {
    if (pv.is_full()) {
      iv.high = pv.FilledWithPrerelease();
    } else if (pv.minor.has_value()) {
      iv.high = SemVersion(*pv.major, *pv.minor + 1, 0);
      iv.high_inclusive = false;
    } else {
      iv.high = SemVersion(*pv.major + 1, 0, 0);
      iv.high_inclusive = false;
    }
  } else {
    return MakeError(ErrorCode::InvalidRange, "unsupported range operator", std::string(token));
  }
  return VersionRange::FromIntervals({iv});
}

Result<VersionRange> ParseGroup(const std::vector<std::string>& tokens, std::size_t begin,
                                std::size_t end) {
  VersionRange group = VersionRange::Any();
  std::size_t i = begin;
  while (i < end) {
    // Hyphen range: "<low> - <high>".
    if (i + 2 < end && tokens[i + 1] == "-") {
      auto low = ParsePartial(tokens[i]);
      if (!low.has_value()) return low.error();
      auto high = ParsePartial(tokens[i + 2]);
      if (!high.has_value()) return high.error();
      VersionInterval iv;
      iv.low = low.value().FilledWithPrerelease();
      iv.high = high.value().FilledWithPrerelease();
      auto piece = VersionRange::FromIntervals({iv});
      if (!piece.has_value()) return piece.error();
      group = VersionRange::Intersect(group, piece.value());
      i += 3;
      continue;
    }
    auto piece = ComparatorToRange(tokens[i]);
    if (!piece.has_value()) return piece.error();
    group = VersionRange::Intersect(group, piece.value());
    ++i;
  }
  return group;
}

}  // namespace

Result<SemVersion> SemVersion::Parse(std::string_view text) {
  auto partial = ParsePartial(text);
  if (!partial.has_value()) return partial.error();
  const PartialVersion& pv = partial.value();
  if (pv.wildcard_tail || !pv.is_full()) {
    return MakeError(ErrorCode::InvalidVersion,
                     "version must be a full major.minor.patch release", std::string(text));
  }
  SemVersion v(*pv.major, *pv.minor, *pv.patch);
  v.prerelease_ = pv.prerelease;
  const std::size_t plus = text.find('+');
  if (plus != std::string_view::npos) {
    auto build = SplitIdentifiers(text.substr(plus + 1), false);
    if (!build.has_value()) return build.error();
    v.build_ = std::string(text.substr(plus + 1));
  }
  return v;
}

std::string SemVersion::ToString() const {
  std::string out;
  out.reserve(24);
  out.append(std::to_string(major_));
  out.push_back('.');
  out.append(std::to_string(minor_));
  out.push_back('.');
  out.append(std::to_string(patch_));
  if (!prerelease_.empty()) {
    out.push_back('-');
    for (std::size_t i = 0; i < prerelease_.size(); ++i) {
      if (i != 0) out.push_back('.');
      out.append(prerelease_[i]);
    }
  }
  if (!build_.empty()) {
    out.push_back('+');
    out.append(build_);
  }
  return out;
}

std::strong_ordering SemVersion::Precedence(const SemVersion& other) const {
  if (major_ != other.major_) return major_ < other.major_ ? std::strong_ordering::less : std::strong_ordering::greater;
  if (minor_ != other.minor_) return minor_ < other.minor_ ? std::strong_ordering::less : std::strong_ordering::greater;
  if (patch_ != other.patch_) return patch_ < other.patch_ ? std::strong_ordering::less : std::strong_ordering::greater;
  return ComparePrerelease(prerelease_, other.prerelease_);
}

std::strong_ordering SemVersion::TotalOrder(const SemVersion& other) const {
  const std::strong_ordering p = Precedence(other);
  if (p != std::strong_ordering::equal) return p;
  const int cmp = build_.compare(other.build_);
  if (cmp == 0) return std::strong_ordering::equal;
  return cmp < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
}

bool operator==(const VersionInterval& a, const VersionInterval& b) {
  const auto bound_equal = [](const std::optional<SemVersion>& x, const std::optional<SemVersion>& y) {
    if (x.has_value() != y.has_value()) return false;
    if (!x.has_value()) return true;
    return x->Precedence(*y) == std::strong_ordering::equal;
  };
  return bound_equal(a.low, b.low) && bound_equal(a.high, b.high) &&
         (a.low.has_value() ? a.low_inclusive == b.low_inclusive : true) &&
         (a.high.has_value() ? a.high_inclusive == b.high_inclusive : true);
}

bool VersionInterval::StructurallyEmpty() const {
  if (!low.has_value() || !high.has_value()) return false;
  const std::strong_ordering cmp = low->Precedence(*high);
  if (cmp == std::strong_ordering::greater) return true;
  if (cmp == std::strong_ordering::equal) return !(low_inclusive && high_inclusive);
  return false;
}

bool VersionInterval::IsExact() const {
  return low.has_value() && high.has_value() && low_inclusive && high_inclusive &&
         low->Precedence(*high) == std::strong_ordering::equal;
}

bool VersionInterval::ContainsBound(const SemVersion& v) const {
  if (low.has_value()) {
    const std::strong_ordering cmp = v.Precedence(*low);
    if (cmp == std::strong_ordering::less) return false;
    if (cmp == std::strong_ordering::equal && !low_inclusive) return false;
  }
  if (high.has_value()) {
    const std::strong_ordering cmp = v.Precedence(*high);
    if (cmp == std::strong_ordering::greater) return false;
    if (cmp == std::strong_ordering::equal && !high_inclusive) return false;
  }
  return true;
}

bool VersionInterval::Contains(const SemVersion& v) const {
  if (!ContainsBound(v)) return false;
  if (!v.has_prerelease()) return true;
  // SemVer range convention: a prerelease release is admitted only by an
  // interval whose lower bound carries a prerelease with the same
  // major.minor.patch tuple. This keeps ">=1.0.0 <2.0.0" from silently
  // admitting "1.5.0-rc.1".
  if (!low.has_value() || !low->has_prerelease()) return false;
  return low->major() == v.major() && low->minor() == v.minor() && low->patch() == v.patch();
}

bool VersionInterval::Subsumes(const VersionInterval& other) const {
  // Lower bound: this interval's low must be <= other's low.
  if (!low.has_value()) {
    // -inf covers everything below.
  } else if (!other.low.has_value()) {
    return false;
  } else {
    const std::strong_ordering cmp = low->Precedence(*other.low);
    if (cmp == std::strong_ordering::greater) return false;
    if (cmp == std::strong_ordering::equal && low_inclusive && !other.low_inclusive) return false;
  }
  if (!high.has_value()) {
    // +inf covers everything above.
  } else if (!other.high.has_value()) {
    return false;
  } else {
    const std::strong_ordering cmp = high->Precedence(*other.high);
    if (cmp == std::strong_ordering::less) return false;
    if (cmp == std::strong_ordering::equal && high_inclusive && !other.high_inclusive) return false;
  }
  return true;
}

std::string VersionInterval::ToString() const {
  if (low.has_value() && high.has_value() && IsExact()) {
    return "=" + low->ToString();
  }
  std::string out;
  if (low.has_value()) {
    out.append(low_inclusive ? ">=" : ">");
    out.append(low->ToString());
  }
  if (high.has_value()) {
    if (!out.empty()) out.push_back(' ');
    out.append(high_inclusive ? "<=" : "<");
    out.append(high->ToString());
  }
  if (out.empty()) out = "*";
  return out;
}

VersionRange VersionRange::Any() {
  VersionRange r;
  r.intervals_.push_back(VersionInterval::Any());
  return r;
}

VersionRange VersionRange::Exact(const SemVersion& v) {
  VersionRange r;
  r.intervals_.push_back(VersionInterval::Exact(v));
  return r;
}

Result<VersionRange> VersionRange::FromIntervals(std::vector<VersionInterval> intervals) {
  std::vector<VersionInterval> kept;
  kept.reserve(intervals.size());
  for (auto& iv : intervals) {
    if (!iv.StructurallyEmpty()) kept.push_back(std::move(iv));
  }
  std::sort(kept.begin(), kept.end(), [](const VersionInterval& a, const VersionInterval& b) {
    if (a.low.has_value() != b.low.has_value()) return !a.low.has_value();
    if (a.low.has_value()) {
      const std::strong_ordering cmp = a.low->Precedence(*b.low);
      if (cmp != std::strong_ordering::equal) return cmp == std::strong_ordering::less;
      if (a.low_inclusive != b.low_inclusive) return a.low_inclusive;
    }
    if (a.high.has_value() != b.high.has_value()) return !a.high.has_value();
    if (a.high.has_value()) {
      const std::strong_ordering cmp = a.high->Precedence(*b.high);
      if (cmp != std::strong_ordering::equal) return cmp == std::strong_ordering::less;
      if (a.high_inclusive != b.high_inclusive) return !a.high_inclusive;
    }
    return false;
  });

  std::vector<VersionInterval> merged;
  for (auto& iv : kept) {
    if (merged.empty()) {
      merged.push_back(std::move(iv));
      continue;
    }
    VersionInterval& cur = merged.back();
    bool join = false;
    if (!cur.high.has_value()) {
      join = true;
    } else if (!iv.low.has_value()) {
      join = true;
    } else {
      const std::strong_ordering cmp = cur.high->Precedence(*iv.low);
      if (cmp == std::strong_ordering::greater) {
        join = true;
      } else if (cmp == std::strong_ordering::equal) {
        join = cur.high_inclusive || iv.low_inclusive;
      }
    }
    if (!join) {
      merged.push_back(std::move(iv));
      continue;
    }
    if (!cur.high.has_value() || !iv.high.has_value()) {
      cur.high.reset();
      cur.high_inclusive = true;
    } else {
      const std::strong_ordering cmp = cur.high->Precedence(*iv.high);
      if (cmp == std::strong_ordering::less) {
        cur.high = iv.high;
        cur.high_inclusive = iv.high_inclusive;
      } else if (cmp == std::strong_ordering::equal) {
        cur.high_inclusive = cur.high_inclusive || iv.high_inclusive;
      }
    }
  }
  VersionRange r;
  r.intervals_ = std::move(merged);
  return r;
}

Result<VersionRange> VersionRange::Parse(std::string_view text) {
  if (text.size() > kMaxRangeTextLength) {
    return MakeError(ErrorCode::LimitExceeded, "version range text too long");
  }
  // Trim.
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
  const std::string_view trimmed = text.substr(begin, end - begin);
  if (trimmed.empty() || trimmed == "*" || trimmed == "x" || trimmed == "X" || trimmed == "any") {
    return Any();
  }
  if (trimmed == "<none>") return Empty();

  // Split into OR groups on "||" (a single "|" is accepted too).
  std::vector<std::string> groups;
  {
    std::string current;
    for (std::size_t i = 0; i < trimmed.size(); ++i) {
      if (trimmed[i] == '|') {
        if (i + 1 < trimmed.size() && trimmed[i + 1] == '|') ++i;
        groups.push_back(current);
        current.clear();
        continue;
      }
      current.push_back(trimmed[i]);
    }
    groups.push_back(current);
  }
  if (groups.size() > kMaxRangeGroups) {
    return MakeError(ErrorCode::LimitExceeded, "too many alternative groups in version range");
  }
  std::vector<VersionInterval> all;
  for (const std::string& group_text : groups) {
    std::vector<std::string> tokens = Tokenize(group_text);
    // Fold "<op> <version>" splits introduced by whitespace.
    std::vector<std::string> folded;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      const std::string& t = tokens[i];
      const bool op_only = (t == ">" || t == ">=" || t == "<" || t == "<=" || t == "=" || t == "^" ||
                            t == "~");
      if (op_only && i + 1 < tokens.size() && tokens[i + 1] != "-") {
        folded.push_back(t + tokens[i + 1]);
        ++i;
      } else {
        folded.push_back(t);
      }
    }
    if (folded.empty()) {
      // An empty OR group is the wildcard: "||" alone means any.
      return Any();
    }
    if (folded.size() > kMaxComparatorsPerGroup) {
      return MakeError(ErrorCode::LimitExceeded, "too many comparators in version range group");
    }
    auto group_range = ParseGroup(folded, 0, folded.size());
    if (!group_range.has_value()) return group_range.error();
    const std::vector<VersionInterval>& pieces = group_range.value().intervals();
    all.insert(all.end(), pieces.begin(), pieces.end());
  }
  return FromIntervals(std::move(all));
}

bool VersionRange::IsAny() const {
  return intervals_.size() == 1 && !intervals_[0].low.has_value() && !intervals_[0].high.has_value();
}

bool VersionRange::IsExact() const { return intervals_.size() == 1 && intervals_[0].IsExact(); }

std::optional<SemVersion> VersionRange::ExactVersion() const {
  if (!IsExact()) return std::nullopt;
  return intervals_[0].low;
}

bool VersionRange::Contains(const SemVersion& v) const {
  for (const auto& iv : intervals_) {
    if (iv.Contains(v)) return true;
  }
  return false;
}

VersionRange VersionRange::Intersect(const VersionRange& a, const VersionRange& b) {
  std::vector<VersionInterval> out;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.intervals_.size() && j < b.intervals_.size()) {
    const VersionInterval& x = a.intervals_[i];
    const VersionInterval& y = b.intervals_[j];
    VersionInterval iv;
    // low = max
    {
      if (!x.low.has_value()) {
        iv.low = y.low;
        iv.low_inclusive = y.low_inclusive;
      } else if (!y.low.has_value()) {
        iv.low = x.low;
        iv.low_inclusive = x.low_inclusive;
      } else {
        const std::strong_ordering cmp = x.low->Precedence(*y.low);
        if (cmp == std::strong_ordering::greater) {
          iv.low = x.low;
          iv.low_inclusive = x.low_inclusive;
        } else if (cmp == std::strong_ordering::less) {
          iv.low = y.low;
          iv.low_inclusive = y.low_inclusive;
        } else {
          iv.low = x.low;
          iv.low_inclusive = x.low_inclusive && y.low_inclusive;
        }
      }
    }
    // high = min
    {
      if (!x.high.has_value()) {
        iv.high = y.high;
        iv.high_inclusive = y.high_inclusive;
      } else if (!y.high.has_value()) {
        iv.high = x.high;
        iv.high_inclusive = x.high_inclusive;
      } else {
        const std::strong_ordering cmp = x.high->Precedence(*y.high);
        if (cmp == std::strong_ordering::less) {
          iv.high = x.high;
          iv.high_inclusive = x.high_inclusive;
        } else if (cmp == std::strong_ordering::greater) {
          iv.high = y.high;
          iv.high_inclusive = y.high_inclusive;
        } else {
          iv.high = x.high;
          iv.high_inclusive = x.high_inclusive && y.high_inclusive;
        }
      }
    }
    if (!iv.StructurallyEmpty()) out.push_back(std::move(iv));
    // Advance whichever interval ends first.
    bool advance_x = false;
    if (!x.high.has_value()) {
      advance_x = false;
    } else if (!y.high.has_value()) {
      advance_x = true;
    } else {
      const std::strong_ordering cmp = x.high->Precedence(*y.high);
      if (cmp == std::strong_ordering::less) {
        advance_x = true;
      } else if (cmp == std::strong_ordering::greater) {
        advance_x = false;
      } else {
        advance_x = x.high_inclusive ? false : true;
      }
    }
    if (advance_x) {
      ++i;
    } else {
      ++j;
    }
  }
  auto result = FromIntervals(std::move(out));
  return result.has_value() ? result.value() : VersionRange::Empty();
}

VersionRange VersionRange::Union(const VersionRange& a, const VersionRange& b) {
  std::vector<VersionInterval> all = a.intervals_;
  all.insert(all.end(), b.intervals_.begin(), b.intervals_.end());
  auto result = FromIntervals(std::move(all));
  return result.has_value() ? result.value() : VersionRange::Empty();
}

bool VersionRange::Overlaps(const VersionRange& a, const VersionRange& b) {
  return !Intersect(a, b).IsEmpty();
}

bool VersionRange::Subsumes(const VersionRange& other) const {
  for (const auto& iv : other.intervals_) {
    bool covered = false;
    for (const auto& mine : intervals_) {
      if (mine.Subsumes(iv)) {
        covered = true;
        break;
      }
    }
    if (!covered) return false;
  }
  return true;
}

std::string VersionRange::ToString() const {
  if (intervals_.empty()) return "<none>";
  if (IsAny()) return "*";
  std::string out;
  for (std::size_t i = 0; i < intervals_.size(); ++i) {
    if (i != 0) out.append(" || ");
    out.append(intervals_[i].ToString());
  }
  return out;
}

}  // namespace fcr
