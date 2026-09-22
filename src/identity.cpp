#include "fcr/identity.hpp"

#include <cstdio>
#include <cstring>

namespace fcr {
namespace {

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

constexpr std::int64_t kNanosPerSecond = 1000000000LL;
constexpr std::int64_t kNanosPerDay = 86400LL * kNanosPerSecond;

// Howard Hinnant's civil-from-days, valid for the full int64 range we accept.
void CivilFromDays(std::int64_t z, int& year, unsigned& month, unsigned& day) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  year = static_cast<int>(y + (m <= 2 ? 1 : 0));
  month = m;
  day = d;
}

}  // namespace

Result<GenerationId> ParseGenerationId(std::string_view text) {
  if (text.size() < 8 || text.compare(0, 4, "gen-") != 0) {
    return MakeError(ErrorCode::InvalidArgument, "generation identifier must start with 'gen-'");
  }
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos || colon <= 4) {
    return MakeError(ErrorCode::InvalidArgument,
                     "generation identifier must be 'gen-<number>:<digest>'");
  }
  std::uint64_t number = 0;
  for (std::size_t i = 4; i < colon; ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return MakeError(ErrorCode::InvalidArgument, "generation number is not decimal");
    }
    if (number > UINT64_MAX / 10u) {
      return MakeError(ErrorCode::ArithmeticOverflow, "generation number is out of range");
    }
    number = number * 10u + static_cast<std::uint64_t>(c - '0');
  }
  auto digest = ContentDigest::FromHex(text.substr(colon + 1));
  if (!digest.has_value()) return digest.error();
  GenerationId id;
  id.number = GenerationNumber(number);
  id.digest = digest.value();
  return id;
}

ContentDigest ContentDigest::FromBytes(const std::array<std::byte, kSize>& bytes) {
  ContentDigest d;
  d.bytes_ = bytes;
  return d;
}

Result<ContentDigest> ContentDigest::FromHex(std::string_view hex) {
  if (hex.size() != kSize * 2) {
    return MakeError(ErrorCode::InvalidArgument, "content digest must be 64 hex characters");
  }
  ContentDigest d;
  for (std::size_t i = 0; i < kSize; ++i) {
    const int hi = HexValue(hex[i * 2]);
    const int lo = HexValue(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return MakeError(ErrorCode::InvalidArgument, "content digest contains non-hex characters");
    }
    d.bytes_[i] = static_cast<std::byte>((hi << 4) | lo);
  }
  return d;
}

std::string ContentDigest::ToHex() const {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.resize(kSize * 2);
  for (std::size_t i = 0; i < kSize; ++i) {
    const unsigned v = std::to_integer<unsigned>(bytes_[i]);
    out[i * 2] = kHex[(v >> 4) & 0x0f];
    out[i * 2 + 1] = kHex[v & 0x0f];
  }
  return out;
}

bool ContentDigest::IsZero() const {
  for (std::byte b : bytes_) {
    if (b != std::byte{0}) return false;
  }
  return true;
}

Result<Timestamp> Timestamp::FromIso8601(std::string_view text) {
  // Accepts YYYY-MM-DDTHH:MM:SS[.fffffffff]Z (UTC only).
  if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':' || text.back() != 'Z') {
    return MakeError(ErrorCode::InvalidArgument, "timestamp must be RFC3339 UTC (YYYY-MM-DDTHH:MM:SSZ)");
  }
  const auto digits = [&](std::size_t offset, std::size_t count, int& out) -> bool {
    for (std::size_t i = 0; i < count; ++i) {
      const char c = text[offset + i];
      if (c < '0' || c > '9') return false;
    }
    out = 0;
    for (std::size_t i = 0; i < count; ++i) out = out * 10 + (text[offset + i] - '0');
    return true;
  };
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!digits(0, 4, year) || !digits(5, 2, month) || !digits(8, 2, day) || !digits(11, 2, hour) ||
      !digits(14, 2, minute) || !digits(17, 2, second)) {
    return MakeError(ErrorCode::InvalidArgument, "timestamp contains non-digit fields");
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
    return MakeError(ErrorCode::InvalidArgument, "timestamp field out of range");
  }
  std::int64_t fraction = 0;
  std::size_t pos = 19;
  if (pos < text.size() - 1 && text[pos] == '.') {
    ++pos;
    std::size_t count = 0;
    while (pos < text.size() - 1 && count < 9) {
      const char c = text[pos];
      if (c < '0' || c > '9') {
        return MakeError(ErrorCode::InvalidArgument, "timestamp fraction contains non-digits");
      }
      fraction = fraction * 10 + (c - '0');
      ++pos;
      ++count;
    }
    while (count < 9) {
      fraction *= 10;
      ++count;
    }
    if (pos != text.size() - 1) {
      return MakeError(ErrorCode::InvalidArgument, "timestamp fraction has too many digits");
    }
  } else if (pos != text.size() - 1) {
    return MakeError(ErrorCode::InvalidArgument, "malformed timestamp");
  }
  // days from civil
  const int y = month <= 2 ? year - 1 : year;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned mp = static_cast<unsigned>(month > 2 ? month - 3 : month + 9);
  const unsigned doy = (153u * mp + 2u) / 5u + static_cast<unsigned>(day - 1);
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const std::int64_t days = era * 146097 + static_cast<std::int64_t>(doe) - 719468;
  const std::int64_t seconds =
      days * 86400 + static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60 +
      static_cast<std::int64_t>(second);
  return Timestamp(seconds * kNanosPerSecond + fraction);
}

std::string Timestamp::ToIso8601() const {
  std::int64_t seconds = nanos_ / kNanosPerSecond;
  std::int64_t fraction = nanos_ % kNanosPerSecond;
  if (fraction < 0) {
    fraction += kNanosPerSecond;
    --seconds;
  }
  std::int64_t days = seconds / 86400;
  std::int64_t rem = seconds % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  int year = 1970;
  unsigned month = 1;
  unsigned day = 1;
  CivilFromDays(days, year, month, day);
  const int hour = static_cast<int>(rem / 3600);
  const int minute = static_cast<int>((rem % 3600) / 60);
  const int second = static_cast<int>(rem % 60);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02d:%02d:%02d.%09lldZ", year, month, day,
                hour, minute, second, static_cast<long long>(fraction));
  return std::string(buffer);
}

}  // namespace fcr
