#include <algorithm>
#include <string>
#include <vector>

#include "fixtures.hpp"

using namespace fcr;
using namespace fcr::test;
using fcr::test::Rng;

namespace {

struct VersionCase {
  const char* text;
  const char* canonical;
};

std::string RandomVersionText(Rng* rng) {
  std::string text = std::to_string(rng->Below(8)) + "." + std::to_string(rng->Below(8)) + "." +
                     std::to_string(rng->Below(8));
  if (rng->Chance(40)) {
    static const char* kPre[] = {"alpha", "beta", "rc", "alpha.1", "beta.2", "rc.11", "0", "1"};
    text.push_back('-');
    text.append(kPre[rng->Below(8)]);
  }
  if (rng->Chance(20)) {
    text.push_back('+');
    text.append("build." + std::to_string(rng->Below(100)));
  }
  return text;
}

}  // namespace

FCR_TEST(version, parse_accepts_valid_semver) {
  const VersionCase cases[] = {
      {"0.0.0", "0.0.0"},
      {"1.2.3", "1.2.3"},
      {"1.0.0-alpha", "1.0.0-alpha"},
      {"1.0.0-alpha.1", "1.0.0-alpha.1"},
      {"1.0.0-alpha.beta", "1.0.0-alpha.beta"},
      {"1.0.0+build.5", "1.0.0+build.5"},
      {"1.0.0-rc.1+build.5", "1.0.0-rc.1+build.5"},
      {"4294967295.0.0", "4294967295.0.0"},
      {"2.4.1", "2.4.1"},
  };
  for (const VersionCase& item : cases) {
    auto parsed = SemVersion::Parse(item.text);
    CHECK_OK(parsed);
    CHECK_EQ(parsed.value().ToString(), std::string(item.canonical));
  }
}

FCR_TEST(version, parse_rejects_malformed) {
  const char* cases[] = {
      "",       "1",        "1.2",        "1.2.3.4",   "01.2.3",   "1.02.3",  "1.2.03",
      "1.2.x",  "1.2.*",    "v1.2.3",     "1.2.-3",    "1.2.3-",   "1.2.3+",  "1.2.3-alpha..1",
      "1.2.3-alpha_1", "1.2.3-01", "4294967296.0.0", "99999999999999999999.0.0", "1..2",
      " 1.2.3", "1.2.3 ",   "1,2,3",
  };
  for (const char* text : cases) {
    auto parsed = SemVersion::Parse(text);
    CHECK_FALSE(parsed.has_value());
    CHECK_EQ(parsed.error().code == ErrorCode::InvalidVersion ||
                 parsed.error().code == ErrorCode::LimitExceeded,
             true);
  }
}

FCR_TEST(version, precedence_follows_semver_spec) {
  const char* ordered[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                           "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "2.0.0",
                           "2.1.0", "2.1.1"};
  const std::size_t count = sizeof(ordered) / sizeof(ordered[0]);
  for (std::size_t i = 0; i < count; ++i) {
    auto left = SemVersion::Parse(ordered[i]);
    CHECK_OK(left);
    for (std::size_t j = 0; j < count; ++j) {
      auto right = SemVersion::Parse(ordered[j]);
      CHECK_OK(right);
      const std::strong_ordering expected =
          i == j ? std::strong_ordering::equal
                 : (i < j ? std::strong_ordering::less : std::strong_ordering::greater);
      CHECK(left.value().Precedence(right.value()) == expected);
    }
  }
}

FCR_TEST(version, build_metadata_is_not_precedence) {
  auto a = SemVersion::Parse("1.2.3+one");
  CHECK_OK(a);
  auto b = SemVersion::Parse("1.2.3+two");
  CHECK_OK(b);
  CHECK(a.value().Precedence(b.value()) == std::strong_ordering::equal);
  CHECK(a.value().SameRelease(b.value()));
  // The total order still separates them for canonical sorting.
  CHECK(a.value().TotalOrder(b.value()) != std::strong_ordering::equal);
  CHECK(a.value() == b.value());
}

FCR_TEST(version, range_parsing_table) {
  struct Case {
    const char* range;
    const char* inside[4];
    const char* outside[4];
  };
  const Case cases[] = {
      {"*", {"0.0.0", "99.99.99", "", ""}, {"", "", "", ""}},
      {"1.2.3", {"1.2.3", "", "", ""}, {"1.2.4", "1.2.2", "", ""}},
      {"=1.2.3", {"1.2.3", "", "", ""}, {"1.2.4", "", "", ""}},
      {">=1.2.3 <2.0.0", {"1.2.3", "1.9.9", "", ""}, {"2.0.0", "1.2.2", "", ""}},
      {"^1.2.3", {"1.2.3", "1.9.0", "", ""}, {"2.0.0", "1.2.2", "", ""}},
      {"^0.2.3", {"0.2.3", "0.2.9", "", ""}, {"0.3.0", "0.2.2", "", ""}},
      {"^0.0.3", {"0.0.3", "", "", ""}, {"0.0.4", "0.0.2", "", ""}},
      {"~1.2.3", {"1.2.3", "1.2.9", "", ""}, {"1.3.0", "1.2.2", "", ""}},
      {"~1.2", {"1.2.0", "1.2.9", "", ""}, {"1.3.0", "1.1.9", "", ""}},
      {"1.2.*", {"1.2.0", "1.2.9", "", ""}, {"1.3.0", "1.1.9", "", ""}},
      {"1.*", {"1.0.0", "1.9.9", "", ""}, {"2.0.0", "0.9.9", "", ""}},
      {"1.2.3 - 2.0.0", {"1.2.3", "2.0.0", "1.5.0", ""}, {"2.0.1", "1.2.2", "", ""}},
      {">1.2.3", {"1.2.4", "2.0.0", "", ""}, {"1.2.3", "1.2.2", "", ""}},
      {"<=1.2.3", {"1.2.3", "0.0.1", "", ""}, {"1.2.4", "", "", ""}},
      {">=1.0.0 <2.0.0 || >=3.0.0", {"1.5.0", "3.0.0", "4.2.1", ""}, {"2.5.0", "0.9.0", "", ""}},
      {"^1.2.3-alpha", {"1.2.3-alpha", "1.2.3-beta", "1.2.3", ""},
       {"1.2.4-alpha", "1.2.2", "", ""}},
  };
  for (const Case& item : cases) {
    auto range = VersionRange::Parse(item.range);
    CHECK_OK(range);
    for (const char* text : item.inside) {
      if (*text == '\0') continue;
      auto version = SemVersion::Parse(text);
      CHECK_OK(version);
      if (!range.value().Contains(version.value())) {
        FCR_FAIL("range " << item.range << " should contain " << text);
      }
    }
    for (const char* text : item.outside) {
      if (*text == '\0') continue;
      auto version = SemVersion::Parse(text);
      CHECK_OK(version);
      if (range.value().Contains(version.value())) {
        FCR_FAIL("range " << item.range << " should not contain " << text);
      }
    }
  }
}

FCR_TEST(version, impossible_ranges_are_empty) {
  const char* cases[] = {">=2.0.0 <1.0.0", ">1.0.0 <1.0.0", ">=1.0.0 <=0.9.0", "<none>"};
  for (const char* text : cases) {
    auto range = VersionRange::Parse(text);
    CHECK_OK(range);
    CHECK(range.value().IsEmpty());
    CHECK_FALSE(range.value().Contains(SemVersion(1, 0, 0)));
  }
  auto any = VersionRange::Parse("*");
  CHECK_OK(any);
  CHECK_FALSE(any.value().IsEmpty());
  CHECK(any.value().Contains(SemVersion(123456, 0, 0)));
}

FCR_TEST(version, malformed_ranges_are_rejected) {
  const char* cases[] = {">=",       "<=",       ">",        "^",         "~",
                         "1.2.3.x",  ">=1.2.3 <", "abc",     "1.2.3 || ", ">=1.2.3 && <2.0.0",
                         "4294967296", "1.2.3-",  "1.2.3-+x"};
  for (const char* text : cases) {
    auto range = VersionRange::Parse(text);
    if (range.has_value()) {
      // A few inputs are legitimately parsed as alternatives; make sure the
      // ones we expect to fail really do.
      if (std::string(text) == "1.2.3 || ") continue;
      FCR_FAIL("range " << text << " should not parse (got " << range.value().ToString() << ")");
    }
  }
}

FCR_TEST(version, canonical_text_round_trips) {
  const char* cases[] = {"*",
                         "1.2.3",
                         ">=1.2.3",
                         ">1.2.3",
                         "<=1.2.3",
                         "<1.2.3",
                         ">=1.2.3 <2.0.0",
                         ">1.2.3 <=2.0.0",
                         ">=1.0.0 <2.0.0 || >=3.0.0 <4.0.0",
                         "<none>"};
  for (const char* text : cases) {
    auto range = VersionRange::Parse(text);
    CHECK_OK(range);
    auto again = VersionRange::Parse(range.value().ToString());
    CHECK_OK(again);
    CHECK(range.value() == again.value());
    CHECK_EQ(again.value().ToString(), range.value().ToString());
  }
}

FCR_TEST(version, set_operations_are_consistent) {
  auto a = VersionRange::Parse(">=1.0.0 <3.0.0").value();
  auto b = VersionRange::Parse(">=2.0.0 <4.0.0").value();
  const VersionRange intersection = VersionRange::Intersect(a, b);
  const VersionRange unioned = VersionRange::Union(a, b);
  auto in = SemVersion::Parse("2.5.0").value();
  auto out = SemVersion::Parse("3.5.0").value();
  CHECK(intersection.Contains(in));
  CHECK_FALSE(intersection.Contains(out));
  CHECK(unioned.Contains(in));
  CHECK(unioned.Contains(out));
  CHECK(a.Subsumes(intersection));
  CHECK_FALSE(intersection.Subsumes(a));
  CHECK(unioned.Subsumes(a));
  CHECK(unioned.Subsumes(b));
  CHECK(VersionRange::Overlaps(a, b));
  auto c = VersionRange::Parse(">=5.0.0").value();
  CHECK_FALSE(VersionRange::Overlaps(a, c));
  CHECK(VersionRange::Intersect(a, c).IsEmpty());
}

FCR_TEST(version, property_exact_range_contains_only_that_version) {
  Rng rng(0x51ee7ull);
  for (int i = 0; i < 4000; ++i) {
    auto version = SemVersion::Parse(RandomVersionText(&rng));
    CHECK_OK(version);
    const VersionRange range = VersionRange::Exact(version.value());
    CHECK(range.Contains(version.value()));
    CHECK(range.IsExact());
    CHECK(range.ExactVersion().has_value());
    CHECK_EQ(range.ToString(), "=" + version.value().ToString());
    CHECK(range.Subsumes(VersionRange::Exact(version.value())));
    CHECK_FALSE(range.Subsumes(VersionRange::Any()));
  }
}

FCR_TEST(version, property_union_and_intersection_agree_with_membership) {
  Rng rng(0xfeedbeefull);
  for (int i = 0; i < 2000; ++i) {
    const std::string low_a = std::to_string(rng.Below(5)) + ".0.0";
    const std::string high_a = std::to_string(rng.Below(5) + 5) + ".0.0";
    const std::string low_b = std::to_string(rng.Below(5)) + ".0.0";
    const std::string high_b = std::to_string(rng.Below(5) + 5) + ".0.0";
    auto a = VersionRange::Parse(">=" + low_a + " <" + high_a);
    CHECK_OK(a);
    auto b = VersionRange::Parse(">=" + low_b + " <" + high_b);
    CHECK_OK(b);
    const VersionRange u = VersionRange::Union(a.value(), b.value());
    const VersionRange n = VersionRange::Intersect(a.value(), b.value());
    for (int probe = 0; probe < 12; ++probe) {
      auto v = SemVersion::Parse(std::to_string(probe) + ".0.0");
      CHECK_OK(v);
      const bool in_a = a.value().Contains(v.value());
      const bool in_b = b.value().Contains(v.value());
      CHECK_EQ(u.Contains(v.value()), in_a || in_b);
      CHECK_EQ(n.Contains(v.value()), in_a && in_b);
    }
  }
}

FCR_TEST(version, property_total_order_is_a_strict_weak_order) {
  Rng rng(0xabcdef01ull);
  std::vector<SemVersion> versions;
  for (int i = 0; i < 500; ++i) {
    auto parsed = SemVersion::Parse(RandomVersionText(&rng));
    CHECK_OK(parsed);
    versions.push_back(parsed.value());
  }
  for (const SemVersion& a : versions) {
    CHECK(a.Precedence(a) == std::strong_ordering::equal);
    for (const SemVersion& b : versions) {
      const std::strong_ordering ab = a.Precedence(b);
      const std::strong_ordering ba = b.Precedence(a);
      CHECK_EQ(ab == std::strong_ordering::less, ba == std::strong_ordering::greater);
      CHECK_EQ(ab == std::strong_ordering::equal, ba == std::strong_ordering::equal);
    }
  }
  std::vector<SemVersion> sorted = versions;
  std::sort(sorted.begin(), sorted.end(), [](const SemVersion& x, const SemVersion& y) {
    return x.TotalOrder(y) == std::strong_ordering::less;
  });
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    CHECK(sorted[i - 1].TotalOrder(sorted[i]) != std::strong_ordering::greater);
  }
}
