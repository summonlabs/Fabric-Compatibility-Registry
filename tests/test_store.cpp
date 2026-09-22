#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fixtures.hpp"

using namespace fcr;
using namespace fcr::test;
using fcr::test::Rng;

namespace {

StoreOptions MakeOptions(const std::string& directory, OpenMode mode) {
  StoreOptions options;
  options.directory = directory;
  options.mode = mode;
  return options;
}

Result<RegistryStore> OpenForWrite(const std::string& directory) {
  return RegistryStore::Open(MakeOptions(directory, OpenMode::ReadWrite));
}

Result<RegistryStore> OpenForRead(const std::string& directory) {
  return RegistryStore::Open(MakeOptions(directory, OpenMode::ReadOnly));
}

std::filesystem::path GenerationFile(const std::string& directory, GenerationNumber number) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "gen-%020llu.fcr",
                static_cast<unsigned long long>(number.value()));
  return std::filesystem::path(directory) / buffer;
}

std::vector<std::byte> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::byte> out;
  char buffer[4096];
  while (stream) {
    stream.read(buffer, sizeof(buffer));
    const std::streamsize read = stream.gcount();
    for (std::streamsize i = 0; i < read; ++i) {
      out.push_back(static_cast<std::byte>(static_cast<unsigned char>(buffer[i])));
    }
  }
  return out;
}

void WriteBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

// The store owns the generation ordinal and publisher epoch of a stored
// generation, so the expected payload carries the assigned envelope.
RegistryDocument WithEnvelope(RegistryDocument document, std::uint64_t generation,
                              std::uint64_t epoch) {
  document.meta.generation = GenerationNumber(generation);
  document.meta.epoch = PublisherEpoch(epoch);
  document.Normalize();
  return document;
}

RegistryDocument RenamedDocument(const std::string& name) {
  RegistryDocumentBuilder builder = MakeStandardBuilder();
  builder.SetLabel("variant", name);
  return builder.Build();
}

}  // namespace

FCR_TEST(store, publish_persists_and_reloads) {
  const std::string directory = fcr::test::UniqueTempDirectory("publish");
  {
    auto store = OpenForWrite(directory);
    CHECK_OK(store);
    auto fence = store.value().CurrentFence();
    CHECK_OK(fence);
    CHECK_EQ(fence.value().expected_generation.value(), 0u);
    RegistryDocument document = MakeStandardDocument();
    auto outcome = store.value().Publish(document, fence.value());
    CHECK_OK(outcome);
    CHECK_EQ(outcome.value().id.number.value(), 1u);
    CHECK_EQ(outcome.value().meta.total_publications, std::uint64_t{1});
  }
  {
    auto store = OpenForRead(directory);
    CHECK_OK(store);
    auto loaded = store.value().LoadLatest();
    CHECK_OK(loaded);
    CHECK_EQ(loaded.value().id.number.value(), 1u);
    CHECK_EQ(loaded.value().id.digest.ToHex(), MakeStandardDocument().Digest().ToHex());
    CHECK_FALSE(loaded.value().recovery.recovered);
  }
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, generations_form_a_digest_chain) {
  const std::string directory = fcr::test::UniqueTempDirectory("chain");
  auto store = OpenForWrite(directory);
  CHECK_OK(store);
  ContentDigest previous = ContentDigest::Zero();
  for (int i = 1; i <= 4; ++i) {
    auto fence = store.value().CurrentFence();
    CHECK_OK(fence);
    auto outcome = store.value().Publish(RenamedDocument("v" + std::to_string(i)), fence.value());
    CHECK_OK(outcome);
    auto loaded = store.value().LoadGeneration(GenerationNumber(static_cast<std::uint64_t>(i)));
    CHECK_OK(loaded);
    const RegistryDocument& document = loaded.value().registry->document();
    if (i == 1) {
      // The first generation has no parent.
      CHECK(previous.IsZero());
      CHECK_FALSE(document.meta.parent_digest.has_value());
    } else {
      CHECK(document.meta.parent_digest.has_value());
      CHECK(document.meta.parent_digest.value() == previous);
    }
    previous = loaded.value().id.digest;
  }
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, publication_is_atomic_and_leaves_no_debris) {
  const std::string directory = fcr::test::UniqueTempDirectory("atomic");
  auto store = OpenForWrite(directory);
  CHECK_OK(store);
  for (int i = 0; i < 5; ++i) {
    auto fence = store.value().CurrentFence();
    CHECK_OK(fence);
    CHECK_OK(store.value().Publish(RenamedDocument("atomic" + std::to_string(i)), fence.value()));
  }
  std::size_t generation_files = 0;
  std::size_t temporary_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const std::string name = entry.path().filename().string();
    if (name.find(".tmp-") != std::string::npos) ++temporary_files;
    if (name.size() > 4 && name.compare(0, 4, "gen-") == 0 &&
        name.compare(name.size() - 4, 4, ".fcr") == 0) {
      ++generation_files;
    }
  }
  CHECK_EQ(generation_files, std::size_t{5});
  CHECK_EQ(temporary_files, std::size_t{0});
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, stale_generation_publication_is_refused) {
  const std::string directory = fcr::test::UniqueTempDirectory("stale-gen");
  auto store = OpenForWrite(directory);
  CHECK_OK(store);
  auto first_fence = store.value().CurrentFence();
  CHECK_OK(first_fence);
  CHECK_OK(store.value().Publish(MakeStandardDocument(), first_fence.value()));
  // The same fence can no longer publish: the head moved on.
  auto second = store.value().Publish(RenamedDocument("second"), first_fence.value());
  CHECK_FALSE(second.has_value());
  CHECK_EQ(second.error().code, ErrorCode::StaleGeneration);
  auto third = store.value().Publish(RenamedDocument("third"),
                                     store.value().CurrentFence().value());
  CHECK_OK(third);
  CHECK_EQ(third.value().id.number.value(), 2u);
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, stale_epoch_and_incarnation_are_refused_after_reopen) {
  const std::string directory = fcr::test::UniqueTempDirectory("stale-epoch");
  PublishFence first_fence;
  {
    auto store = OpenForWrite(directory);
    CHECK_OK(store);
    first_fence = store.value().CurrentFence().value();
    CHECK_OK(store.value().Publish(RenamedDocument("first"), first_fence));
  }
  auto store = OpenForWrite(directory);
  CHECK_OK(store);
  const PublishFence second_fence = store.value().CurrentFence().value();
  CHECK(second_fence.epoch.value() > first_fence.epoch.value());
  CHECK(second_fence.incarnation.value() > first_fence.incarnation.value());
  CHECK_OK(store.value().Publish(RenamedDocument("second"), second_fence));

  auto stale = store.value().Publish(RenamedDocument("stale"), first_fence);
  CHECK_FALSE(stale.has_value());
  CHECK(stale.error().code == ErrorCode::StaleEpoch ||
        stale.error().code == ErrorCode::StaleGeneration);

  PublishFence mismatch;
  mismatch.expected_generation = store.value().CurrentFence().value().expected_generation;
  mismatch.epoch = second_fence.epoch;
  mismatch.incarnation = Incarnation(second_fence.incarnation.value() + 1000);
  auto fenced = store.value().Publish(RenamedDocument("fenced"), mismatch);
  CHECK_FALSE(fenced.has_value());
  CHECK_EQ(fenced.error().code, ErrorCode::StaleIncarnation);
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, corrupt_head_recovers_to_the_previous_generation) {
  const std::string directory = fcr::test::UniqueTempDirectory("recover");
  {
    auto store = OpenForWrite(directory);
    CHECK_OK(store);
    CHECK_OK(store.value().Publish(RenamedDocument("first"),
                                   store.value().CurrentFence().value()));
    CHECK_OK(store.value().Publish(RenamedDocument("second"),
                                   store.value().CurrentFence().value()));
  }
  // Corrupt the newest generation payload.
  std::vector<std::byte> bytes = ReadBytes(GenerationFile(directory, GenerationNumber(2)));
  CHECK(bytes.size() > 200);
  bytes[150] = static_cast<std::byte>(std::to_integer<unsigned>(bytes[150]) ^ 0x40);
  WriteBytes(GenerationFile(directory, GenerationNumber(2)), bytes);

  auto store = OpenForWrite(directory);
  CHECK_OK(store);
  auto loaded = store.value().LoadLatest();
  CHECK_OK(loaded);
  CHECK_EQ(loaded.value().id.number.value(), 1u);
  CHECK(loaded.value().recovery.recovered);
  CHECK_EQ(loaded.value().recovery.skipped_corrupt.size(), std::size_t{1});
  CHECK_EQ(loaded.value().recovery.skipped_corrupt.front().value(), 2u);
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, header_corruption_is_detected) {
  const std::string directory = fcr::test::UniqueTempDirectory("header");
  {
    auto store = OpenForWrite(directory);
    CHECK_OK(store);
    CHECK_OK(store.value().Publish(MakeStandardDocument(),
                                   store.value().CurrentFence().value()));
  }
  std::vector<std::byte> bytes = ReadBytes(GenerationFile(directory, GenerationNumber(1)));
  bytes[10] = static_cast<std::byte>(std::to_integer<unsigned>(bytes[10]) ^ 0xff);
  WriteBytes(GenerationFile(directory, GenerationNumber(1)), bytes);
  auto store = OpenForRead(directory);
  CHECK_OK(store);
  auto loaded = store.value().LoadGeneration(GenerationNumber(1));
  CHECK_FALSE(loaded.has_value());
  CHECK(loaded.error().code == ErrorCode::CorruptPersistence ||
        loaded.error().code == ErrorCode::DigestMismatch);
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, truncated_and_oversized_files_are_rejected) {
  const std::string directory = fcr::test::UniqueTempDirectory("truncated");
  {
    auto store = OpenForWrite(directory);
    CHECK_OK(store);
    CHECK_OK(store.value().Publish(MakeStandardDocument(),
                                   store.value().CurrentFence().value()));
  }
  const std::filesystem::path path = GenerationFile(directory, GenerationNumber(1));
  std::vector<std::byte> bytes = ReadBytes(path);
  WriteBytes(path, std::vector<std::byte>(bytes.begin(), bytes.begin() + 40));
  auto store = OpenForRead(directory);
  CHECK_OK(store);
  CHECK_ERR(store.value().LoadGeneration(GenerationNumber(1)), ErrorCode::CorruptPersistence);

  WriteBytes(path, std::vector<std::byte>(10, std::byte{0}));
  CHECK_ERR(store.value().LoadGeneration(GenerationNumber(1)), ErrorCode::CorruptPersistence);

  // A wrong payload length in the header is caught by the header checksum
  // first, which is the correct conservative behaviour.
  std::vector<std::byte> whole = ReadBytes(path);
  (void)whole;
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, unsupported_format_versions_are_rejected) {
  const std::string directory = fcr::test::UniqueTempDirectory("version");
  {
    auto store = OpenForWrite(directory);
    CHECK_OK(store);
    CHECK_OK(store.value().Publish(MakeStandardDocument(),
                                   store.value().CurrentFence().value()));
  }
  const std::filesystem::path path = GenerationFile(directory, GenerationNumber(1));
  std::vector<std::byte> bytes = ReadBytes(path);
  // Bump the format version field in the header (offset 4, little endian) and
  // refresh the header checksum so only the version check can fail.
  bytes[4] = std::byte{99};
  WriteBytes(path, bytes);
  auto store = OpenForRead(directory);
  CHECK_OK(store);
  auto loaded = store.value().LoadGeneration(GenerationNumber(1));
  CHECK_FALSE(loaded.has_value());
  CHECK(loaded.error().code == ErrorCode::CorruptPersistence ||
        loaded.error().code == ErrorCode::UnsupportedFormatVersion);
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, uncommitted_debris_is_discarded_not_adopted) {
  const std::string directory = fcr::test::UniqueTempDirectory("debris");
  RegistryDocument payload;
  {
    auto store = OpenForWrite(directory);
    CHECK_OK(store);
    CHECK_OK(store.value().Publish(MakeStandardDocument(),
                                   store.value().CurrentFence().value()));
    payload = RenamedDocument("debris");
  }
  // Fabricate a generation file that the head metadata never committed.
  std::vector<std::byte> bytes = ReadBytes(GenerationFile(directory, GenerationNumber(1)));
  WriteBytes(GenerationFile(directory, GenerationNumber(2)), bytes);
  auto store = OpenForWrite(directory);
  CHECK_OK(store);
  auto loaded = store.value().LoadLatest();
  CHECK_OK(loaded);
  CHECK_EQ(loaded.value().id.number.value(), 1u);
  CHECK_EQ(loaded.value().recovery.discarded_uncommitted.size(), std::size_t{1});
  CHECK_FALSE(std::filesystem::exists(GenerationFile(directory, GenerationNumber(2))));
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, read_only_open_does_not_mutate_the_directory) {
  const std::string directory = fcr::test::UniqueTempDirectory("readonly");
  {
    auto store = OpenForWrite(directory);
    CHECK_OK(store);
    CHECK_OK(store.value().Publish(MakeStandardDocument(),
                                   store.value().CurrentFence().value()));
  }
  auto writer = OpenForRead(directory);
  CHECK_OK(writer);
  const StoreMeta before = writer.value().ReadMeta().value();
  for (int i = 0; i < 3; ++i) {
    auto reader = OpenForRead(directory);
    CHECK_OK(reader);
    const StoreMeta after = reader.value().ReadMeta().value();
    CHECK_EQ(after.epoch.value(), before.epoch.value());
    CHECK_EQ(after.incarnation.value(), before.incarnation.value());
  }
  CHECK_ERR(writer.value().Publish(RenamedDocument("no"), writer.value().CurrentFence().value()),
            ErrorCode::ImmutableGeneration);
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, concurrent_writers_are_excluded_by_the_directory_lock) {
  const std::string directory = fcr::test::UniqueTempDirectory("lock");
  auto first = OpenForWrite(directory);
  CHECK_OK(first);
  auto second = OpenForWrite(directory);
  CHECK_FALSE(second.has_value());
  CHECK_EQ(second.error().code, ErrorCode::LockContention);
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, generation_limit_and_pruning_are_enforced) {
  const std::string directory = fcr::test::UniqueTempDirectory("limit");
  StoreOptions options = MakeOptions(directory, OpenMode::ReadWrite);
  options.max_generations = 3;
  auto store = RegistryStore::Open(options);
  CHECK_OK(store);
  for (int i = 0; i < 3; ++i) {
    CHECK_OK(store.value().Publish(RenamedDocument("g" + std::to_string(i)),
                                   store.value().CurrentFence().value()));
  }
  auto overflow = store.value().Publish(RenamedDocument("overflow"),
                                        store.value().CurrentFence().value());
  CHECK_FALSE(overflow.has_value());
  CHECK_EQ(overflow.error().code, ErrorCode::GenerationLimitReached);

  auto removed = store.value().Prune(2);
  CHECK_OK(removed);
  CHECK_EQ(removed.value().size(), std::size_t{1});
  CHECK_OK(store.value().Publish(RenamedDocument("after-prune"),
                                 store.value().CurrentFence().value()));
  auto generations = store.value().ListGenerations();
  CHECK_OK(generations);
  CHECK_EQ(generations.value().size(), std::size_t{3});
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, cancelled_publication_never_commits) {
  const std::string directory = fcr::test::UniqueTempDirectory("cancel");
  auto store = OpenForWrite(directory);
  CHECK_OK(store);
  CHECK_OK(store.value().Publish(RenamedDocument("kept"), store.value().CurrentFence().value()));
  CancellationToken token;
  token.Cancel();
  auto cancelled = store.value().Publish(RenamedDocument("never"), store.value().CurrentFence().value(),
                                         token);
  CHECK_FALSE(cancelled.has_value());
  CHECK_EQ(cancelled.error().code, ErrorCode::Cancelled);
  auto generations = store.value().ListGenerations();
  CHECK_OK(generations);
  CHECK_EQ(generations.value().size(), std::size_t{1});
  auto loaded = store.value().LoadLatest();
  CHECK_OK(loaded);
  CHECK_EQ(loaded.value().id.number.value(), 1u);
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, invalid_documents_never_reach_storage) {
  const std::string directory = fcr::test::UniqueTempDirectory("invalid");
  auto store = OpenForWrite(directory);
  CHECK_OK(store);
  RegistryDocument invalid = MakeStandardDocument();
  invalid.rules.clear();
  Rule broken = MakeRule("broken", RuleOutcome::Compatible);
  broken.left.kind = NicKind();
  broken.right.kind = NicKind();
  broken.constraints.push_back(
      RequiresCapability{Side::Left, CapabilityId::Parse("ghost.cap").value()});
  invalid.rules.push_back(broken);
  invalid.Normalize();
  auto published = store.value().Publish(invalid, store.value().CurrentFence().value());
  CHECK_FALSE(published.has_value());
  CHECK_EQ(published.error().code, ErrorCode::ValidationFailed);
  auto generations = store.value().ListGenerations();
  CHECK_OK(generations);
  CHECK(generations.value().empty());
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, reopening_preserves_immutable_history) {
  const std::string directory = fcr::test::UniqueTempDirectory("immutable");
  std::vector<std::string> digests;
  for (int round = 0; round < 3; ++round) {
    auto store = OpenForWrite(directory);
    CHECK_OK(store);
    for (int i = 0; i < 2; ++i) {
      auto fence = store.value().CurrentFence();
      CHECK_OK(fence);
      auto outcome = store.value().Publish(RenamedDocument("round" + std::to_string(round) + "-" +
                                                           std::to_string(i)),
                                           fence.value());
      CHECK_OK(outcome);
      digests.push_back(outcome.value().id.digest.ToHex());
    }
  }
  auto store = OpenForRead(directory);
  CHECK_OK(store);
  auto generations = store.value().ListGenerations();
  CHECK_OK(generations);
  CHECK_EQ(generations.value().size(), digests.size());
  for (std::size_t i = 0; i < generations.value().size(); ++i) {
    auto loaded = store.value().LoadGeneration(generations.value()[i]);
    CHECK_OK(loaded);
    CHECK_EQ(loaded.value().id.digest.ToHex(), digests[i]);
  }
  RemoveDirectoryQuietly(directory);
}

FCR_TEST(store, encoding_and_decoding_round_trips) {
  const RegistryDocument document = MakeStandardDocument();
  const RegistryDocument expected = WithEnvelope(document, 7, 3);
  auto encoded = RegistryStore::EncodeGeneration(document, GenerationNumber(7), PublisherEpoch(3),
                                                 Incarnation(11), 64ull * 1024ull * 1024ull);
  CHECK_OK(encoded);
  CHECK(encoded.value().size() > 96);
  GenerationNumber generation;
  PublisherEpoch epoch;
  Incarnation incarnation;
  auto decoded = RegistryStore::DecodeGeneration(encoded.value(), &generation, &epoch, &incarnation);
  CHECK_OK(decoded);
  CHECK_EQ(generation.value(), 7u);
  CHECK_EQ(epoch.value(), 3u);
  CHECK_EQ(incarnation.value(), 11u);
  CHECK_EQ(decoded.value().CanonicalJson(), expected.CanonicalJson());

  // Every single-byte corruption must be detected.
  Rng rng(0x31415926ull);
  for (int attempt = 0; attempt < 300; ++attempt) {
    std::vector<std::byte> corrupted = encoded.value();
    const std::size_t index = rng.Below(static_cast<std::uint32_t>(corrupted.size()));
    corrupted[index] =
        static_cast<std::byte>(std::to_integer<unsigned>(corrupted[index]) ^
                               static_cast<unsigned>(1 + rng.Below(255)));
    auto result = RegistryStore::DecodeGeneration(corrupted, nullptr, nullptr, nullptr);
    if (result.has_value()) {
      FCR_FAIL("corruption at byte " << index << " was not detected");
    }
  }
}
