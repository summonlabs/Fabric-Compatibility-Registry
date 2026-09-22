#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "fixtures.hpp"

using namespace fcr;
using namespace fcr::test;
using fcr::test::Rng;

namespace {

ContentDigest DigestOf(const std::string& text) { return Sha256::Hash(text); }

std::string Hex(const ContentDigest& digest) { return digest.ToHex(); }

std::uint32_t Crc32cBytes(const std::vector<std::uint8_t>& bytes) {
  return Crc32c(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()),
                                           bytes.size()));
}

}  // namespace

FCR_TEST(digest, sha256_matches_published_vectors) {
  struct Case {
    const char* input;
    const char* expected;
  };
  const Case cases[] = {
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {"The quick brown fox jumps over the lazy dog",
       "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"},
      {"The quick brown fox jumps over the lazy dog.",
       "ef537f25c895bfa782526529a9b63d97aa631564d5d789c2b765448c8635fb6c"},
  };
  for (const Case& item : cases) {
    CHECK_EQ(Hex(DigestOf(item.input)), std::string(item.expected));
  }
  // One million 'a' characters, the classic long-message vector.
  std::string million(1000000, 'a');
  CHECK_EQ(Hex(DigestOf(million)),
           std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

FCR_TEST(digest, sha256_incremental_matches_one_shot) {
  Rng rng(0xc0ffeeull);
  for (int i = 0; i < 200; ++i) {
    std::string text(static_cast<std::size_t>(rng.Below(300)), 'x');
    for (char& c : text) c = static_cast<char>('a' + static_cast<int>(rng.Below(26)));
    Sha256 incremental;
    std::size_t offset = 0;
    while (offset < text.size()) {
      const std::size_t chunk = std::min<std::size_t>(1 + rng.Below(17), text.size() - offset);
      incremental.Update(std::string_view(text).substr(offset, chunk));
      offset += chunk;
    }
    CHECK(Hex(incremental.Final()) == Hex(DigestOf(text)));
  }
}

FCR_TEST(digest, crc32c_matches_published_vectors) {
  CHECK_EQ(Crc32cBytes({}), 0x00000000u);
  {
    const std::string text = "123456789";
    CHECK_EQ(Crc32c(text), 0xE3069283u);
  }
  {
    std::vector<std::uint8_t> zeros(32, 0);
    CHECK_EQ(Crc32cBytes(zeros), 0x8A9136AAu);
  }
  {
    std::vector<std::uint8_t> ones(32, 0xFF);
    CHECK_EQ(Crc32cBytes(ones), 0x62A8AB43u);
  }
  {
    std::vector<std::uint8_t> ascending(32);
    for (std::size_t i = 0; i < ascending.size(); ++i) ascending[i] = static_cast<std::uint8_t>(i);
    CHECK_EQ(Crc32cBytes(ascending), 0x46DD794Eu);
  }
  {
    std::vector<std::uint8_t> descending(32);
    for (std::size_t i = 0; i < descending.size(); ++i) {
      descending[i] = static_cast<std::uint8_t>(31 - i);
    }
    CHECK_EQ(Crc32cBytes(descending), 0x113FDB5Cu);
  }
}

FCR_TEST(digest, content_digest_hex_round_trips) {
  Rng rng(0x5151ull);
  for (int i = 0; i < 200; ++i) {
    const std::string text = "payload-" + std::to_string(rng.Next());
    const ContentDigest digest = DigestOf(text);
    auto parsed = ContentDigest::FromHex(digest.ToHex());
    CHECK_OK(parsed);
    CHECK(parsed.value() == digest);
    CHECK_FALSE(digest.IsZero());
  }
  CHECK_FALSE(ContentDigest::FromHex("abc").has_value());
  CHECK_FALSE(ContentDigest::FromHex(std::string(64, 'z')).has_value());
  CHECK(ContentDigest::Zero().IsZero());
}

FCR_TEST(digest, document_digest_is_order_independent) {
  const RegistryDocument canonical = MakeStandardDocument();
  const std::string baseline = canonical.Digest().ToHex();
  CHECK_EQ(baseline.size(), std::size_t{64});

  Rng rng(0x1010ull);
  for (int attempt = 0; attempt < 40; ++attempt) {
    RegistryDocument shuffled = canonical;
    for (std::size_t i = shuffled.rules.size(); i > 1; --i) {
      const std::size_t j = rng.Below(static_cast<std::uint32_t>(i));
      std::swap(shuffled.rules[i - 1], shuffled.rules[j]);
    }
    for (Rule& rule : shuffled.rules) {
      for (std::size_t i = rule.constraints.size(); i > 1; --i) {
        const std::size_t j = rng.Below(static_cast<std::uint32_t>(i));
        std::swap(rule.constraints[i - 1], rule.constraints[j]);
      }
    }
    CHECK_EQ(shuffled.Digest().ToHex(), baseline);
    CHECK_EQ(shuffled.CanonicalJson(), canonical.CanonicalJson());
  }
}

FCR_TEST(digest, document_json_round_trips_exactly) {
  const RegistryDocument original = MakeStandardDocument();
  const JsonValue json = RegistryDocumentToJson(original);
  auto reparsed = ParseRegistryDocument(json);
  CHECK_OK(reparsed);
  CHECK_EQ(reparsed.value().CanonicalJson(), original.CanonicalJson());
  CHECK_EQ(reparsed.value().Digest().ToHex(), original.Digest().ToHex());
  CHECK(reparsed.value() == original);
}

FCR_TEST(digest, rule_digest_changes_with_meaning_only) {
  const RegistryDocument document = MakeStandardDocument();
  CHECK(!document.rules.empty());
  const Rule& rule = document.rules.front();
  const ContentDigest baseline = RuleContentDigest(rule);
  Rule same = rule;
  std::swap(same.left, same.right);
  NormalizeRule(same);
  // Swapping both selectors changes the declared region, so the digest moves.
  CHECK(RuleContentDigest(same) != baseline || rule.left.IsWildcard());
  Rule changed = rule;
  changed.priority = RulePriority(rule.priority.value() + 1);
  CHECK(RuleContentDigest(changed) != baseline);
}
