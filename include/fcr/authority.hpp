// Fabric Compatibility Registry - Summon Software Labs
// RegistryAuthority: the embeddable library surface. Fabric Upgrade Manager and
// other runtimes link this directly; the network service is a thin wrapper.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "fcr/cancel.hpp"
#include "fcr/document.hpp"
#include "fcr/error.hpp"
#include "fcr/identity.hpp"
#include "fcr/registry.hpp"
#include "fcr/store.hpp"
#include "fcr/validation.hpp"

namespace fcr {

struct AuthorityOptions {
  StoreOptions store;
  ValidationOptions validation;
  // When false, Open requires an existing published generation. When true an
  // empty data directory is accepted and no generation is authoritative yet.
  bool allow_empty = true;
};

struct AuthorityStatus {
  bool has_generation = false;
  GenerationId generation;
  Incarnation incarnation;
  PublisherEpoch epoch;
  std::string registry_name;
  std::uint64_t total_publications = 0;
  Timestamp updated_at;
  std::size_t rule_count = 0;
  std::size_t active_rule_count = 0;
  std::size_t capability_count = 0;
  std::size_t kind_count = 0;
  std::size_t retained_generations = 0;
  bool persistent = false;
};

struct PublishRequest {
  // The generation the caller based its change on. The authority refuses the
  // publication when the head has moved on.
  GenerationNumber expected_generation;
  CancellationToken token;
};

struct PublishResult {
  GenerationId id;
  ValidationReport validation;
  std::size_t rule_count = 0;
};

// Locking discipline (audited, see README):
//   * One shared_mutex guards the current snapshot, the retained history and
//     the cached head metadata.
//   * Queries take a shared lock, copy an immutable snapshot handle and return.
//     No lock is held while a caller inspects evidence.
//   * Publication validates and compiles outside any lock, then takes the
//     exclusive lock only to re-check the fence and swap the head.
//   * The store's OS-level writer lock is acquired once during Open and is
//     never acquired while the shared_mutex is held by a different path.
//   * No callback, no logging sink and no event emission ever runs under a lock.
class RegistryAuthority {
 public:
  static Result<std::shared_ptr<RegistryAuthority>> Open(const AuthorityOptions& options);

  // In-memory authority for embedding and tests. No persistence; publication
  // still validates, still fences on the expected generation.
  static Result<std::shared_ptr<RegistryAuthority>> OpenInMemory(const RegistryDocument& seed,
                                                                ValidationOptions validation =
                                                                    ValidationOptions{});

  RegistryAuthority(const RegistryAuthority&) = delete;
  RegistryAuthority& operator=(const RegistryAuthority&) = delete;

  // --- Queries ------------------------------------------------------------
  Result<Decision> QueryPair(const ComponentSpec& left, const ComponentSpec& right) const;
  Result<SetDecision> QuerySet(const std::vector<ComponentSpec>& members) const;

  // --- Generation access --------------------------------------------------
  bool HasGeneration() const;
  Result<GenerationId> CurrentGeneration() const;
  // Immutable snapshot handle; safe to use after the call returns.
  Result<std::shared_ptr<const RegistryGeneration>> Snapshot() const;
  Result<std::shared_ptr<const RegistryGeneration>> SnapshotAt(GenerationNumber number) const;
  Result<std::vector<GenerationNumber>> Generations() const;

  // --- Publication --------------------------------------------------------
  // Pure validation: never mutates, never fences.
  ValidationReport ValidateDocument(const RegistryDocument& document) const;
  // Validation plus the generation/digest the document would become. No commit.
  Result<PublishResult> SimulatePublish(const RegistryDocument& document) const;
  Result<PublishResult> Publish(const RegistryDocument& document, const PublishRequest& request);

  // --- Analysis -----------------------------------------------------------
  Result<GenerationDiff> Diff(GenerationNumber from, GenerationNumber to) const;
  Result<std::vector<ProvenanceRecord>> Provenance(const RuleId& rule) const;
  Result<DecisionReplayDiff> Replay(const std::vector<std::vector<ComponentSpec>>& queries,
                                    GenerationNumber from, GenerationNumber to) const;

  // --- Authority state ----------------------------------------------------
  Result<AuthorityStatus> GetStatus() const;
  IncarnationStamp Stamp() const;
  // Fences a caller that presents an obsolete incarnation or generation.
  fcr::Status CheckStamp(const IncarnationStamp& stamp) const;
  Result<std::vector<GenerationNumber>> Prune(std::size_t keep);

  const ValidationOptions& validation_options() const { return options_.validation; }

 private:
  RegistryAuthority() = default;

  struct HeadState {
    std::shared_ptr<const RegistryGeneration> current;
    StoreMeta meta;
  };
  Result<HeadState> HeadLocked() const;
  Result<PublishResult> Commit(const RegistryDocument& document, GenerationNumber expected);

  AuthorityOptions options_;
  mutable std::shared_mutex mutex_;
  std::shared_ptr<const RegistryGeneration> current_;
  std::map<GenerationNumber, std::shared_ptr<const RegistryGeneration>> history_;
  StoreMeta meta_;
  std::unique_ptr<RegistryStore> store_;
  Incarnation incarnation_;
  PublisherEpoch epoch_;
};

}  // namespace fcr
