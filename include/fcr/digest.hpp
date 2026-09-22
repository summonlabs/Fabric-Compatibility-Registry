// Fabric Compatibility Registry - Summon Software Labs
// Content hashing primitives used for canonical digests and integrity checks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "fcr/error.hpp"
#include "fcr/identity.hpp"

namespace fcr {

// Streaming SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;
  static constexpr std::size_t kBlockBytes = 64;

  Sha256();

  void Update(std::span<const std::byte> data);
  void Update(std::string_view text);
  ContentDigest Final();

  static ContentDigest Hash(std::span<const std::byte> data);
  static ContentDigest Hash(std::string_view text);

 private:
  void Compress(const std::byte* block);

  std::uint32_t state_[8];
  std::uint64_t bit_length_;
  std::byte buffer_[kBlockBytes];
  std::size_t buffered_;
};

// CRC-32C (Castagnoli), used for cheap structural integrity of headers.
std::uint32_t Crc32c(std::span<const std::byte> data);
std::uint32_t Crc32c(std::string_view text);

// Incremental CRC-32C.
class Crc32cAccumulator {
 public:
  void Update(std::span<const std::byte> data);
  std::uint32_t value() const { return value_; }

 private:
  std::uint32_t value_ = 0xFFFFFFFFu;
};

}  // namespace fcr
