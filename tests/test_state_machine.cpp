#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "fixtures.hpp"

using namespace fcr;
using namespace fcr::test;
using fcr::test::Rng;

namespace {

class Machine {
 public:
  Machine() : directory_(fcr::test::UniqueTempDirectory("machine")), rng_(0x5eed5eedull) {}

  ~Machine() {
    authority_.reset();
    RemoveDirectoryQuietly(directory_);
  }

  void Start() {
    OpenAuthority();
    auto current = authority_->CurrentGeneration();
    if (current.has_value()) {
      publications_ = current.value().number.value();
    } else {
      // A directory with no published generation has no authoritative head.
      PublishValid();
    }
  }

  void Run(int steps) {
    for (int step = 0; step < steps; ++step) {
      switch (rng_.Below(8)) {
        case 0:
        case 1:
        case 2: PublishValid(); break;
        case 3: PublishWithWrongFence(); break;
        case 4: Query(); break;
        case 5: Inspect(); break;
        case 6: if (rng_.Chance(30)) Restart(); break;
        default: Prune(); break;
      }
      CheckInvariants();
    }
  }

  std::uint64_t publications() const { return publications_; }

 private:
  void OpenAuthority() {
    AuthorityOptions options;
    options.store.directory = directory_;
    options.store.mode = OpenMode::ReadWrite;
    options.store.max_generations = 4096;
    auto authority = RegistryAuthority::Open(options);
    CHECK_OK(authority);
    authority_ = authority.value();
  }

  void Restart() {
    authority_.reset();
    OpenAuthority();
  }

  RegistryDocument Variant(const std::string& label) {
    RegistryDocumentBuilder builder = MakeStandardBuilder();
    builder.SetLabel("variant", label);
    if (rng_.Chance(40)) builder.SetDescription("variant " + label);
    if (rng_.Chance(30)) {
      Rule extra = MakeRule("extra-" + label, rng_.Chance(50) ? RuleOutcome::Compatible
                                                              : RuleOutcome::Incompatible);
      extra.left.kind = GpuKind();
      extra.right.kind = GpuKind();
      extra.priority = RulePriority(static_cast<std::int32_t>(rng_.Below(5)));
      CHECK_OK(builder.AddRule(extra));
    }
    return builder.Build();
  }

  void PublishValid() {
    const std::uint64_t head = HeadNumber();
    PublishRequest request;
    request.expected_generation = GenerationNumber(head);
    auto published = authority_->Publish(Variant(std::to_string(publications_ + 1)), request);
    if (!published.has_value()) {
      FCR_FAIL("publication with the correct fence failed: " << published.error().ToString());
    }
    ++publications_;
  }

  void PublishWithWrongFence() {
    const std::uint64_t head = HeadNumber();
    std::uint64_t wrong = static_cast<std::uint64_t>(rng_.Below(static_cast<std::uint32_t>(head + 3)));
    if (wrong == head && rng_.Chance(50)) wrong = head + 1;
    PublishRequest request;
    request.expected_generation = GenerationNumber(wrong);
    auto published = authority_->Publish(Variant("wrong"), request);
    if (wrong == head) {
      if (!published.has_value()) {
        FCR_FAIL("publication with the correct fence failed: " << published.error().ToString());
      }
      ++publications_;
    } else {
      if (published.has_value()) {
        FCR_FAIL("stale fence " << wrong << " was accepted at head " << head);
      }
      CHECK_EQ(published.error().code, ErrorCode::StaleGeneration);
    }
  }

  void Query() {
    ComponentSpec left = MakeNic("nic-a", std::to_string(rng_.Below(4)) + ".0.0");
    ComponentSpec right = MakeNic("nic-b", std::to_string(rng_.Below(4)) + ".0.0");
    if (rng_.Chance(60)) {
      CHECK_OK(left.capabilities.Set(RoceCapability(), CapabilityValue::Flag(true)));
      CHECK_OK(right.capabilities.Set(RoceCapability(), CapabilityValue::Flag(true)));
      CHECK_OK(left.protocols.Set(RdmaProtocol(), {SemVersion(2, 0, 0)}));
      CHECK_OK(right.protocols.Set(RdmaProtocol(), {SemVersion(2, 0, 0)}));
    }
    auto first = authority_->QueryPair(left, right);
    CHECK_OK(first);
    auto second = authority_->QueryPair(left, right);
    CHECK_OK(second);
    if (!(first.value().id == second.value().id)) {
      FCR_FAIL("repeated query produced a different decision identifier");
    }
    if (!(first.value().generation == second.value().generation)) {
      FCR_FAIL("decision generation changed between identical queries");
    }
  }

  void Inspect() {
    auto generations = authority_->Generations();
    CHECK_OK(generations);
    if (generations.value().empty()) {
      FCR_FAIL("no generation is retained although " << publications_ << " were published");
    }
    if (generations.value().size() > publications_ + 1) {
      FCR_FAIL("retained generation count " << generations.value().size()
                                           << " exceeds the number of publications "
                                           << publications_ + 1);
    }
    const GenerationId head = authority_->CurrentGeneration().value();
    if (std::find(generations.value().begin(), generations.value().end(), head.number) ==
        generations.value().end()) {
      FCR_FAIL("the authoritative generation is not retained");
    }
    for (GenerationNumber number : generations.value()) {
      auto snapshot = authority_->SnapshotAt(number);
      CHECK_OK(snapshot);
      CHECK(snapshot.value()->id().digest == snapshot.value()->document().Digest());
    }
    if (generations.value().size() >= 2) {
      auto diff = authority_->Diff(generations.value().front(), generations.value().back());
      CHECK_OK(diff);
      auto again = authority_->Diff(generations.value().front(), generations.value().back());
      CHECK_OK(again);
      CHECK_EQ(RenderGenerationDiff(diff.value()), RenderGenerationDiff(again.value()));
    }
    auto history = authority_->Provenance(RuleId::Parse("nic-requires-roce").value());
    CHECK_OK(history);
    auto status = authority_->GetStatus();
    CHECK_OK(status);
  }

  void Prune() {
    if (publications_ < 5) return;
    auto removed = authority_->Prune(4);
    if (!removed.has_value()) return;
    auto generations = authority_->Generations();
    CHECK_OK(generations);
    CHECK(generations.value().size() <= 4);
    // The head must always survive pruning.
    auto current = authority_->CurrentGeneration();
    CHECK_OK(current);
    CHECK(std::find(generations.value().begin(), generations.value().end(),
                    current.value().number) != generations.value().end());
    publications_ = current.value().number.value();
  }

  std::uint64_t HeadNumber() {
    auto current = authority_->CurrentGeneration();
    if (!current.has_value()) return 0;
    return current.value().number.value();
  }

  void CheckInvariants() {
    auto current = authority_->CurrentGeneration();
    if (publications_ == 0) {
      CHECK_FALSE(current.has_value());
    } else {
      CHECK_OK(current);
      if (current.value().number.value() != publications_) {
        FCR_FAIL("head generation " << current.value().number.value() << " does not match the "
                                    << publications_ << " successful publications");
      }
    }
    auto snapshot = authority_->Snapshot();
    CHECK_OK(snapshot);
    CHECK(snapshot.value()->id().digest == snapshot.value()->document().Digest());
    const IncarnationStamp stamp = authority_->Stamp();
    CHECK_EQ(stamp.incarnation.value() > 0, true);
    CHECK(authority_->CheckStamp(stamp).ok());
    IncarnationStamp stale;
    stale.incarnation = Incarnation(stamp.incarnation.value() + 7);
    stale.generation = stamp.generation;
    CHECK_FALSE(authority_->CheckStamp(stale).ok());
  }

  std::string directory_;
  Rng rng_;
  std::shared_ptr<RegistryAuthority> authority_;
  std::uint64_t publications_ = 0;
};

}  // namespace

FCR_TEST(state_machine, seeded_random_operations_preserve_invariants) {
  for (std::uint64_t seed = 1; seed <= 4; ++seed) {
    Machine machine;
    machine.Start();
    machine.Run(60);
  }
}

FCR_TEST(state_machine, restart_after_every_step_preserves_the_head) {
  const std::string directory = fcr::test::UniqueTempDirectory("restart-steps");
  std::uint64_t expected = 0;
  for (int round = 0; round < 6; ++round) {
    AuthorityOptions options;
    options.store.directory = directory;
    options.store.mode = OpenMode::ReadWrite;
    auto authority = RegistryAuthority::Open(options);
    CHECK_OK(authority);
    auto current = authority.value()->CurrentGeneration();
    if (expected == 0) {
      CHECK_FALSE(current.has_value());
    } else {
      CHECK_OK(current);
      CHECK_EQ(current.value().number.value(), expected);
    }
    PublishRequest request;
    request.expected_generation = GenerationNumber(expected);
    RegistryDocumentBuilder builder = MakeStandardBuilder();
    builder.SetLabel("round", std::to_string(round));
    auto published = authority.value()->Publish(builder.Build(), request);
    CHECK_OK(published);
    ++expected;
    const IncarnationStamp stamp = authority.value()->Stamp();
    // Closing releases the writer lock; the next open is a fresh incarnation
    // and fences every stamp minted by the previous one.
    authority.value().reset();
    auto next = RegistryAuthority::Open(options);
    CHECK_OK(next);
    CHECK_FALSE(next.value()->CheckStamp(stamp).ok());
  }
  RemoveDirectoryQuietly(directory);
}
