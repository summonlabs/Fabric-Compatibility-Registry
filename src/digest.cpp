#include "fcr/digest.hpp"

#include <cstring>

namespace fcr {
namespace {

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline std::uint32_t RotateRight(std::uint32_t v, unsigned bits) {
  return (v >> bits) | (v << (32u - bits));
}

std::uint32_t LoadBigEndian(const std::byte* p) {
  return (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[0])) << 24) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[1])) << 16) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[2])) << 8) |
         static_cast<std::uint32_t>(std::to_integer<unsigned>(p[3]));
}

void StoreBigEndian(std::byte* p, std::uint32_t v) {
  p[0] = static_cast<std::byte>((v >> 24) & 0xffu);
  p[1] = static_cast<std::byte>((v >> 16) & 0xffu);
  p[2] = static_cast<std::byte>((v >> 8) & 0xffu);
  p[3] = static_cast<std::byte>(v & 0xffu);
}

std::uint32_t Crc32cTableEntry(std::uint32_t index) {
  std::uint32_t crc = index;
  for (int bit = 0; bit < 8; ++bit) {
    crc = (crc & 1u) ? ((crc >> 1) ^ 0x82F63B78u) : (crc >> 1);
  }
  return crc;
}

const std::uint32_t* Crc32cTable() {
  static const std::uint32_t* table = [] {
    static std::uint32_t t[256];
    for (std::uint32_t i = 0; i < 256; ++i) t[i] = Crc32cTableEntry(i);
    return t;
  }();
  return table;
}

}  // namespace

Sha256::Sha256() : bit_length_(0), buffered_(0) {
  state_[0] = 0x6a09e667u;
  state_[1] = 0xbb67ae85u;
  state_[2] = 0x3c6ef372u;
  state_[3] = 0xa54ff53au;
  state_[4] = 0x510e527fu;
  state_[5] = 0x9b05688cu;
  state_[6] = 0x1f83d9abu;
  state_[7] = 0x5be0cd19u;
  std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::Compress(const std::byte* block) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = LoadBigEndian(block + i * 4);
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
    const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::Update(std::span<const std::byte> data) {
  bit_length_ += static_cast<std::uint64_t>(data.size()) * 8u;
  std::size_t offset = 0;
  if (buffered_ != 0) {
    const std::size_t want = kBlockBytes - buffered_;
    const std::size_t take = data.size() < want ? data.size() : want;
    std::memcpy(buffer_ + buffered_, data.data(), take);
    buffered_ += take;
    offset += take;
    if (buffered_ == kBlockBytes) {
      Compress(buffer_);
      buffered_ = 0;
    }
  }
  while (data.size() - offset >= kBlockBytes) {
    Compress(data.data() + offset);
    offset += kBlockBytes;
  }
  if (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    std::memcpy(buffer_, data.data() + offset, remaining);
    buffered_ = remaining;
  }
}

void Sha256::Update(std::string_view text) {
  Update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

ContentDigest Sha256::Final() {
  const std::uint64_t bit_length = bit_length_;
  std::byte padding[kBlockBytes * 2];
  std::memset(padding, 0, sizeof(padding));
  const std::size_t pad_length = (buffered_ < 56) ? (56 - buffered_) : (120 - buffered_);
  padding[0] = std::byte{0x80};
  for (int i = 0; i < 8; ++i) {
    padding[pad_length + static_cast<std::size_t>(i)] =
        static_cast<std::byte>((bit_length >> (56 - 8 * i)) & 0xffu);
  }
  // Feed the padded tail without disturbing bit_length_ accounting.
  std::size_t offset = 0;
  std::size_t remaining = pad_length + 8;
  if (buffered_ != 0) {
    const std::size_t want = kBlockBytes - buffered_;
    const std::size_t take = remaining < want ? remaining : want;
    std::memcpy(buffer_ + buffered_, padding, take);
    buffered_ += take;
    offset += take;
    remaining -= take;
    if (buffered_ == kBlockBytes) {
      Compress(buffer_);
      buffered_ = 0;
    }
  }
  while (remaining >= kBlockBytes) {
    Compress(padding + offset);
    offset += kBlockBytes;
    remaining -= kBlockBytes;
  }
  if (remaining != 0) {
    std::memcpy(buffer_, padding + offset, remaining);
    buffered_ = remaining;
  }
  std::array<std::byte, kDigestBytes> out{};
  for (int i = 0; i < 8; ++i) StoreBigEndian(out.data() + i * 4, state_[i]);
  // Reset to a clean state so Final() is idempotent-safe to call last.
  buffered_ = 0;
  return ContentDigest::FromBytes(out);
}

ContentDigest Sha256::Hash(std::span<const std::byte> data) {
  Sha256 hasher;
  hasher.Update(data);
  return hasher.Final();
}

ContentDigest Sha256::Hash(std::string_view text) {
  Sha256 hasher;
  hasher.Update(text);
  return hasher.Final();
}

void Crc32cAccumulator::Update(std::span<const std::byte> data) {
  const std::uint32_t* table = Crc32cTable();
  std::uint32_t crc = value_;
  for (std::byte b : data) {
    crc = table[(crc ^ std::to_integer<std::uint8_t>(b)) & 0xffu] ^ (crc >> 8);
  }
  value_ = crc;
}

std::uint32_t Crc32c(std::span<const std::byte> data) {
  Crc32cAccumulator acc;
  acc.Update(data);
  return acc.value() ^ 0xFFFFFFFFu;
}

std::uint32_t Crc32c(std::string_view text) {
  return Crc32c(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

}  // namespace fcr
