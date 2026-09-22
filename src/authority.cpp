#include "fcr/authority.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace fcr {
namespace {

Timestamp NowTimestamp() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return Timestamp::FromNanos(
      static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
}

}  // namespace

Result<std::shared_ptr<RegistryAuthority>> RegistryAuthority::Open(
    const AuthorityOptions& options) {
  auto store = RegistryStore::Open(options.store);
  if (!store.has_value()) return store.error();
  auto authority = std::shared_ptr<RegistryAuthority>(new RegistryAuthority());
  authority->options_ = options;
  authority->store_ = std::make_unique<RegistryStore>(std::move(store).value());
  authority->incarnation_ = authority->store_->incarnation();
  authority->epoch_ = authority->store_->epoch();

  auto meta = authority->store_->ReadMeta();
  if (!meta.has_value()) return meta.error();
  authority->meta_ = meta.value();

  if (!authority->meta_.generation.IsZero()) {
    auto loaded = authority->store_->LoadLatest();
    if (!loaded.has_value()) return loaded.error();
    authority->current_ = loaded.value().registry;
    authority->history_.emplace(loaded.value().id.number, loaded.value().registry);
  } else if (!options.allow_empty) {
    return MakeError(ErrorCode::NotFound,
                     "registry data directory holds no published generation and empty is not "
                     "permitted");
  }
  return authority;
}

Result<std::shared_ptr<RegistryAuthority>> RegistryAuthority::OpenInMemory(
    const RegistryDocument& seed, ValidationOptions validation) {
  auto compiled = RegistryGeneration::Compile(seed, validation);
  if (!compiled.has_value()) return compiled.error();
  auto authority = std::shared_ptr<RegistryAuthority>(new RegistryAuthority());
  authority->options_.validation = validation;
  authority->options_.allow_empty = false;
  authority->current_ = compiled.value();
  authority->incarnation_ = Incarnation(1);
  authority->epoch_ = PublisherEpoch(1);
  authority->meta_.generation = compiled.value()->id().number;
  authority->meta_.generation_digest = compiled.value()->id().digest;
  authority->meta_.epoch = authority->epoch_;
  authority->meta_.incarnation = authority->incarnation_;
  authority->meta_.total_publications = compiled.value()->id().number.value();
  authority->history_.emplace(compiled.value()->id().number, compiled.value());
  return authority;
}

Result<RegistryAuthority::HeadState> RegistryAuthority::HeadLocked() const {
  HeadState state;
  state.current = current_;
  state.meta = meta_;
  return state;
}

bool RegistryAuthority::HasGeneration() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return current_ != nullptr;
}

Result<GenerationId> RegistryAuthority::CurrentGeneration() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (current_ == nullptr) {
    return MakeError(ErrorCode::NotFound, "no registry generation is authoritative yet");
  }
  return current_->id();
}

Result<std::shared_ptr<const RegistryGeneration>> RegistryAuthority::Snapshot() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (current_ == nullptr) {
    return MakeError(ErrorCode::NotFound, "no registry generation is authoritative yet");
  }
  return current_;
}

Result<std::shared_ptr<const RegistryGeneration>> RegistryAuthority::SnapshotAt(
    GenerationNumber number) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto it = history_.find(number);
  if (it != history_.end()) return it->second;
  if (store_ == nullptr) {
    return MakeError(ErrorCode::NotFound,
                     "generation is not retained in memory and this authority is not persistent");
  }
  auto loaded = store_->LoadGeneration(number);
  if (!loaded.has_value()) return loaded.error();
  return loaded.value().registry;
}

Result<std::vector<GenerationNumber>> RegistryAuthority::Generations() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::vector<GenerationNumber> out;
  out.reserve(history_.size());
  for (const auto& [number, generation] : history_) {
    (void)generation;
    out.push_back(number);
  }
  if (store_ != nullptr) {
    std::error_code ec;
    std::filesystem::directory_iterator iterator(store_->options().directory, ec);
    if (!ec) {
      std::size_t scanned = 0;
      for (const auto& entry : iterator) {
        if (++scanned > store_->options().max_scan_entries) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() != 4 + 20 + 4) continue;
        if (name.compare(0, 4, "gen-") != 0) continue;
        if (name.compare(name.size() - 4, 4, ".fcr") != 0) continue;
        bool digits = true;
        std::uint64_t value = 0;
        for (std::size_t i = 4; i < name.size() - 4; ++i) {
          if (name[i] < '0' || name[i] > '9') {
            digits = false;
            break;
          }
          value = value * 10u + static_cast<std::uint64_t>(name[i] - '0');
        }
        if (!digits) continue;
        out.push_back(GenerationNumber(value));
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Result<Decision> RegistryAuthority::QueryPair(const ComponentSpec& left,
                                              const ComponentSpec& right) const {
  auto snapshot = Snapshot();
  if (!snapshot.has_value()) return snapshot.error();
  return snapshot.value()->EvaluatePair(left, right);
}

Result<SetDecision> RegistryAuthority::QuerySet(const std::vector<ComponentSpec>& members) const {
  auto snapshot = Snapshot();
  if (!snapshot.has_value()) return snapshot.error();
  return snapshot.value()->EvaluateSet(members);
}

ValidationReport RegistryAuthority::ValidateDocument(const RegistryDocument& document) const {
  RegistryDocument normalized = document;
  normalized.Normalize();
  return ValidateRegistryDocument(normalized, options_.validation);
}

Result<PublishResult> RegistryAuthority::SimulatePublish(const RegistryDocument& document) const {
  GenerationNumber head;
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    head = meta_.generation;
  }
  auto next = NextGeneration(head);
  if (!next.has_value()) return next.error();
  RegistryDocument candidate = document;
  candidate.meta.generation = next.value();
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (head.IsZero()) {
      candidate.meta.parent_digest.reset();
    } else {
      candidate.meta.parent_digest = meta_.generation_digest;
    }
  }
  candidate.Normalize();
  PublishResult result;
  result.validation = ValidateRegistryDocument(candidate, options_.validation);
  if (!result.validation.Publishable()) {
    return MakeError(ErrorCode::ValidationFailed,
                     "the registry document failed static validation; see the validation report");
  }
  auto compiled = RegistryGeneration::Compile(candidate, options_.validation);
  if (!compiled.has_value()) return compiled.error();
  result.id = compiled.value()->id();
  result.rule_count = compiled.value()->document().rules.size();
  return result;
}

Result<PublishResult> RegistryAuthority::Commit(const RegistryDocument& document,
                                                GenerationNumber expected) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (!(meta_.generation == expected)) {
    return MakeError(ErrorCode::StaleGeneration,
                     "the registry head moved on while this publication was being prepared",
                     "expected gen-" + std::to_string(expected.value()) + " but the head is gen-" +
                         std::to_string(meta_.generation.value()));
  }
  if (store_ != nullptr) {
    PublishFence fence;
    fence.expected_generation = meta_.generation;
    fence.epoch = epoch_;
    fence.incarnation = incarnation_;
    auto outcome = store_->Publish(document, fence, CancellationToken{});
    if (!outcome.has_value()) return outcome.error();
    auto loaded = store_->LoadGeneration(outcome.value().id.number);
    if (!loaded.has_value()) return loaded.error();
    current_ = loaded.value().registry;
    history_[loaded.value().id.number] = loaded.value().registry;
    meta_ = outcome.value().meta;
    PublishResult result;
    result.id = outcome.value().id;
    result.rule_count = current_->document().rules.size();
    return result;
  }
  auto next = NextGeneration(meta_.generation);
  if (!next.has_value()) return next.error();
  RegistryDocument candidate = document;
  candidate.meta.generation = next.value();
  candidate.meta.epoch = epoch_;
  if (meta_.generation.IsZero()) {
    candidate.meta.parent_digest.reset();
  } else {
    candidate.meta.parent_digest = meta_.generation_digest;
  }
  candidate.Normalize();
  auto compiled = RegistryGeneration::Compile(candidate, options_.validation);
  if (!compiled.has_value()) return compiled.error();
  current_ = compiled.value();
  history_[compiled.value()->id().number] = compiled.value();
  meta_.generation = compiled.value()->id().number;
  meta_.generation_digest = compiled.value()->id().digest;
  meta_.total_publications += 1;
  meta_.updated_at = NowTimestamp();
  meta_.last_publisher = candidate.meta.publisher;
  PublishResult result;
  result.id = compiled.value()->id();
  result.rule_count = compiled.value()->document().rules.size();
  return result;
}

Result<PublishResult> RegistryAuthority::Publish(const RegistryDocument& document,
                                                const PublishRequest& request) {
  if (request.token.IsCancelled()) {
    return MakeError(ErrorCode::Cancelled, "publication cancelled before it started");
  }
  GenerationNumber head;
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    head = meta_.generation;
  }
  if (!(head == request.expected_generation)) {
    return MakeError(ErrorCode::StaleGeneration,
                     "the publisher based its change on an outdated registry generation",
                     "expected gen-" + std::to_string(request.expected_generation.value()) +
                         " but the head is gen-" + std::to_string(head.value()));
  }

  // Expensive work happens outside the exclusive window.
  auto next = NextGeneration(head);
  if (!next.has_value()) return next.error();
  RegistryDocument candidate = document;
  candidate.meta.generation = next.value();
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    candidate.meta.epoch = epoch_;
    if (head.IsZero()) {
      candidate.meta.parent_digest.reset();
    } else {
      candidate.meta.parent_digest = meta_.generation_digest;
    }
  }
  candidate.Normalize();
  if (request.token.IsCancelled()) {
    return MakeError(ErrorCode::Cancelled, "publication cancelled during preparation");
  }
  ValidationReport report = ValidateRegistryDocument(candidate, options_.validation);
  if (!report.Publishable()) {
    PublishResult result;
    result.validation = report;
    return MakeError(ErrorCode::ValidationFailed,
                     "the registry document failed static validation; see the validation report");
  }
  if (request.token.IsCancelled()) {
    return MakeError(ErrorCode::Cancelled, "publication cancelled before the commit point");
  }
  auto committed = Commit(candidate, request.expected_generation);
  if (!committed.has_value()) return committed.error();
  committed.value().validation = std::move(report);
  return committed;
}

Result<GenerationDiff> RegistryAuthority::Diff(GenerationNumber from, GenerationNumber to) const {
  auto before = SnapshotAt(from);
  if (!before.has_value()) return before.error();
  auto after = SnapshotAt(to);
  if (!after.has_value()) return after.error();
  return DiffGenerations(*before.value(), *after.value());
}

Result<std::vector<ProvenanceRecord>> RegistryAuthority::Provenance(const RuleId& rule) const {
  std::vector<GenerationNumber> numbers;
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    numbers.reserve(history_.size());
    for (const auto& [number, generation] : history_) {
      (void)generation;
      numbers.push_back(number);
    }
  }
  std::sort(numbers.begin(), numbers.end());
  std::vector<std::shared_ptr<const RegistryGeneration>> snapshots;
  snapshots.reserve(numbers.size());
  for (GenerationNumber number : numbers) {
    snapshots.push_back(SnapshotAt(number).value_or(nullptr));
  }
  std::vector<const RegistryGeneration*> raw;
  raw.reserve(snapshots.size());
  for (const auto& snapshot : snapshots) raw.push_back(snapshot.get());
  return ProvenanceHistory(rule, raw);
}

Result<DecisionReplayDiff> RegistryAuthority::Replay(
    const std::vector<std::vector<ComponentSpec>>& queries, GenerationNumber from,
    GenerationNumber to) const {
  auto before = SnapshotAt(from);
  if (!before.has_value()) return before.error();
  auto after = SnapshotAt(to);
  if (!after.has_value()) return after.error();
  return ReplayDiff(*before.value(), *after.value(), queries);
}

Result<AuthorityStatus> RegistryAuthority::GetStatus() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  AuthorityStatus status;
  status.has_generation = current_ != nullptr;
  if (current_ != nullptr) {
    status.generation = current_->id();
    status.registry_name = current_->document().meta.name;
    status.rule_count = current_->document().rules.size();
    for (const Rule& rule : current_->document().rules) {
      if (rule.lifecycle.state == LifecycleState::Active) ++status.active_rule_count;
    }
    status.capability_count = current_->document().taxonomy.capabilities().size();
    status.kind_count = current_->document().taxonomy.kinds().size();
  }
  status.incarnation = incarnation_;
  status.epoch = epoch_;
  status.total_publications = meta_.total_publications;
  status.updated_at = meta_.updated_at;
  status.retained_generations = history_.size();
  status.persistent = store_ != nullptr;
  return status;
}

IncarnationStamp RegistryAuthority::Stamp() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  IncarnationStamp stamp;
  stamp.incarnation = incarnation_;
  if (current_ != nullptr) stamp.generation = current_->id();
  return stamp;
}

fcr::Status RegistryAuthority::CheckStamp(const IncarnationStamp& stamp) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (!(stamp.incarnation == incarnation_)) {
    return Fail(ErrorCode::StaleIncarnation,
                "client holds a stale registry incarnation; the service has restarted since");
  }
  if (current_ != nullptr && !(stamp.generation == current_->id())) {
    return Fail(ErrorCode::StaleGeneration,
                "client refers to a generation that is no longer authoritative");
  }
  return Status{};
}

Result<std::vector<GenerationNumber>> RegistryAuthority::Prune(std::size_t keep) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (store_ == nullptr) {
    return MakeError(ErrorCode::ImmutableGeneration,
                     "an in-memory authority has no persistent history to prune");
  }
  auto removed = store_->Prune(keep);
  if (!removed.has_value()) return removed.error();
  for (GenerationNumber number : removed.value()) history_.erase(number);
  return removed;
}

}  // namespace fcr
