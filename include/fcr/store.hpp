// Fabric Compatibility Registry - Summon Software Labs
// Versioned, integrity-checked persistent registry generations with stale
// publisher fencing and conservative recovery.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fcr/cancel.hpp"
#include "fcr/document.hpp"
#include "fcr/error.hpp"
#include "fcr/identity.hpp"
#include "fcr/json.hpp"
#include "fcr/registry.hpp"

namespace fcr {

inline constexpr std::string_view kStoreMetaFormat = "fcr.registry.meta";
inline constexpr std::uint32_t kStoreMetaFormatVersion = 1;

// Binary generation file layout (little endian):
//   0   4   magic 'F','C','R','G'
//   4   2   format version
//   6   2   header size (96)
//   8   8   generation number
//   16  8   publisher epoch
//   24  8   publisher incarnation
//   32  8   payload length in bytes
//   40  32  payload SHA-256
//   72  4   CRC-32C over header bytes [0,72)
//   76  20  reserved, zero
//   96  ..  canonical registry document JSON
inline constexpr std::uint32_t kGenerationHeaderBytes = 96;
inline constexpr std::uint32_t kGenerationFormatVersion = 1;

enum class OpenMode {
  // Reads existing state; never mutates the directory.
  ReadOnly,
  // Creates the directory if needed, takes the exclusive writer lock, and
  // advances the incarnation and publisher epoch.
  ReadWrite,
};

struct StoreOptions {
  std::filesystem::path directory;
  OpenMode mode = OpenMode::ReadOnly;
  std::uint64_t max_generation_bytes = 64ull * 1024ull * 1024ull;
  std::uint64_t max_meta_bytes = 64ull * 1024ull;
  // Hard ceiling on retained generations. Publication beyond it is refused
  // until an explicit prune releases space.
  std::size_t max_generations = 512;
  // Bound on directory scanning.
  std::size_t max_scan_entries = 8192;
};

// The persistent head of the registry: which generation is authoritative, who
// may publish next, and how many times the directory has been opened for
// writing.
struct StoreMeta {
  GenerationNumber generation;
  ContentDigest generation_digest;
  PublisherEpoch epoch;
  Incarnation incarnation;
  std::uint64_t total_publications = 0;
  Timestamp updated_at;
  PublisherId last_publisher;

  bool IsEmpty() const { return generation.IsZero(); }
};

// What happened while recovering: a store never silently treats stale or
// damaged evidence as current.
struct RecoveryReport {
  bool recovered = false;
  bool repaired = false;
  std::vector<GenerationNumber> skipped_corrupt;
  std::vector<GenerationNumber> discarded_uncommitted;
  std::string detail;
};

struct LoadedGeneration {
  GenerationId id;
  std::shared_ptr<const RegistryGeneration> registry;
  RecoveryReport recovery;
};

// Fencing values a publisher must present. A publisher that read generation N
// cannot publish on top of generation N+1.
struct PublishFence {
  GenerationNumber expected_generation;
  PublisherEpoch epoch;
  Incarnation incarnation;
};

struct PublishOutcome {
  GenerationId id;
  StoreMeta meta;
};

class RegistryStore {
 public:
  static Result<RegistryStore> Open(const StoreOptions& options);

  // Ownership of the exclusive writer lock is transferred, never duplicated:
  // a moved-from store no longer holds the lock and never releases it.
  RegistryStore(RegistryStore&& other) noexcept;
  RegistryStore& operator=(RegistryStore&& other) noexcept;
  RegistryStore(const RegistryStore&) = delete;
  RegistryStore& operator=(const RegistryStore&) = delete;
  ~RegistryStore();

  const StoreOptions& options() const { return options_; }
  OpenMode mode() const { return options_.mode; }
  const Incarnation& incarnation() const { return incarnation_; }
  const PublisherEpoch& epoch() const { return epoch_; }

  // Reads and verifies the head metadata. Never mutates.
  Result<StoreMeta> ReadMeta() const;
  // The fence the current open handle must present.
  Result<PublishFence> CurrentFence() const;

  // Loads the newest valid generation. If the head generation is unreadable,
  // falls back to the newest readable generation and reports it.
  Result<LoadedGeneration> LoadLatest();
  Result<LoadedGeneration> LoadGeneration(GenerationNumber number);
  Result<std::vector<GenerationNumber>> ListGenerations() const;

  // Publishes a document as the next generation. Atomic: the generation file
  // is written and flushed before the head metadata switches over.
  Result<PublishOutcome> Publish(const RegistryDocument& document, const PublishFence& fence,
                                 const CancellationToken& token = CancellationToken{});

  // Explicit administrative pruning. Returns the generations removed.
  Result<std::vector<GenerationNumber>> Prune(std::size_t keep);

  // Serializes a document into the on-disk frame (used by tools and tests).
  static Result<std::vector<std::byte>> EncodeGeneration(const RegistryDocument& document,
                                                         GenerationNumber generation,
                                                         PublisherEpoch epoch,
                                                         Incarnation incarnation,
                                                         std::uint64_t max_bytes);
  static Result<RegistryDocument> DecodeGeneration(const std::vector<std::byte>& bytes,
                                                   GenerationNumber* generation,
                                                   PublisherEpoch* epoch,
                                                   Incarnation* incarnation);

 private:
  RegistryStore() = default;

  Result<std::filesystem::path> GenerationPath(GenerationNumber number) const;
  Result<StoreMeta> LoadMetaLocked() const;
  Result<StoreMeta> WriteMeta(const StoreMeta& meta) const;
  Result<std::vector<std::byte>> ReadFileBounded(const std::filesystem::path& path,
                                                 std::uint64_t max_bytes) const;
  Result<bool> WriteFileAtomic(const std::filesystem::path& path,
                               const std::vector<std::byte>& bytes) const;
  void ReleaseLock();

  StoreOptions options_;
  Incarnation incarnation_;
  PublisherEpoch epoch_;
  void* lock_handle_ = nullptr;  // Win32 HANDLE when opened read-write
  // Key of the process-local writer registry entry held by this store.
  std::string held_directory_key_;
};

// Process-local registry of data directories currently held for writing. The
// Win32 byte-range lock excludes other processes; this excludes a second writer
// inside the same process, where byte-range locks on separate handles of the
// same file are not a reliable exclusion primitive.
bool DirectoryHeldForWrite(const std::string& canonical_directory);

// Canonical serialization of the head metadata (used by tools and tests).
JsonValue StoreMetaToJson(const StoreMeta& meta);
Result<StoreMeta> ParseStoreMeta(const JsonValue& json);

}  // namespace fcr
