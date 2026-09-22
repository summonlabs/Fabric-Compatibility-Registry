#include "fcr/protocol.hpp"

#include "fcr/document.hpp"
#include "fcr/registry.hpp"

namespace fcr {

std::string_view OperationName(Operation operation) {
  switch (operation) {
    case Operation::Ping: return "ping";
    case Operation::Status: return "status";
    case Operation::Validate: return "validate";
    case Operation::Publish: return "publish";
    case Operation::QueryPair: return "query_pair";
    case Operation::QuerySet: return "query_set";
    case Operation::Diff: return "diff";
    case Operation::ReplayDiff: return "replay";
    case Operation::Provenance: return "provenance";
    case Operation::Generations: return "generations";
    case Operation::Prune: return "prune";
    case Operation::Shutdown: return "shutdown";
  }
  return "unknown";
}

Result<Operation> ParseOperation(std::string_view text) {
  if (text == "ping") return Operation::Ping;
  if (text == "status") return Operation::Status;
  if (text == "validate") return Operation::Validate;
  if (text == "publish") return Operation::Publish;
  if (text == "query_pair") return Operation::QueryPair;
  if (text == "query_set") return Operation::QuerySet;
  if (text == "diff") return Operation::Diff;
  if (text == "replay") return Operation::ReplayDiff;
  if (text == "provenance") return Operation::Provenance;
  if (text == "generations") return Operation::Generations;
  if (text == "prune") return Operation::Prune;
  if (text == "shutdown") return Operation::Shutdown;
  return MakeError(ErrorCode::InvalidArgument, "unknown registry operation", std::string(text));
}

JsonValue ErrorToJson(const Error& error) {
  JsonValue out = JsonValue::Obj();
  out.Set("code", JsonValue::Str(std::string(ErrorCodeName(error.code))));
  out.Set("message", JsonValue::Str(error.message));
  if (!error.detail.empty()) out.Set("detail", JsonValue::Str(error.detail));
  return out;
}

JsonValue OkResponse(JsonValue result) {
  JsonValue out = JsonValue::Obj();
  out.Set("ok", JsonValue::Bool(true));
  out.Set("result", std::move(result));
  return out;
}

JsonValue ErrorResponse(const Error& error) {
  JsonValue out = JsonValue::Obj();
  out.Set("ok", JsonValue::Bool(false));
  out.Set("error", ErrorToJson(error));
  return out;
}

JsonValue AuthorityStatusToJson(const AuthorityStatus& status) {
  JsonValue out = JsonValue::Obj();
  out.Set("service", JsonValue::Str(std::string(kServiceName)));
  out.Set("protocol_version", JsonValue::UInt(kServiceProtocolVersion));
  out.Set("persistent", JsonValue::Bool(status.persistent));
  out.Set("has_generation", JsonValue::Bool(status.has_generation));
  if (status.has_generation) {
    JsonValue generation = JsonValue::Obj();
    generation.Set("number", JsonValue::UInt(status.generation.number.value()));
    generation.Set("digest", JsonValue::Str(status.generation.digest.ToHex()));
    generation.Set("id", JsonValue::Str(status.generation.ToString()));
    out.Set("generation", std::move(generation));
  }
  out.Set("incarnation", JsonValue::UInt(status.incarnation.value()));
  out.Set("publisher_epoch", JsonValue::UInt(status.epoch.value()));
  out.Set("registry_name", JsonValue::Str(status.registry_name));
  out.Set("total_publications", JsonValue::UInt(status.total_publications));
  out.Set("updated_at", JsonValue::Str(status.updated_at.ToIso8601()));
  out.Set("rule_count", JsonValue::UInt(status.rule_count));
  out.Set("active_rule_count", JsonValue::UInt(status.active_rule_count));
  out.Set("capability_count", JsonValue::UInt(status.capability_count));
  out.Set("kind_count", JsonValue::UInt(status.kind_count));
  out.Set("retained_generations", JsonValue::UInt(status.retained_generations));
  return out;
}

namespace {

Result<GenerationNumber> ReadGeneration(const JsonValue& request, std::string_view key) {
  const JsonValue* field = request.Find(key);
  if (field == nullptr || field->IsNull()) {
    return MakeError(ErrorCode::NotFound, "generation selector is absent");
  }
  auto value = field->AsUInt();
  if (!value.has_value()) {
    return MakeError(ErrorCode::InvalidArgument,
                     "field '" + std::string(key) + "' must be a non-negative integer");
  }
  return GenerationNumber(value.value());
}

GenerationNumber RequiredGeneration(const JsonValue& request, std::string_view key) {
  auto value = ReadGeneration(request, key);
  return value.has_value() ? value.value() : GenerationNumber(0);
}

Error ValidationFailure(const ValidationReport& report) {
  return MakeError(ErrorCode::ValidationFailed,
                   "registry document failed static validation with " +
                       std::to_string(report.error_count) + " error(s)",
                   report.Render());
}

JsonValue ValidationJson(const ValidationReport& report) {
  return report.ToJson();
}

Result<JsonValue> HandleStatus(RequestContext& context) {
  auto status = context.authority->GetStatus();
  if (!status.has_value()) return status.error();
  JsonValue out = AuthorityStatusToJson(status.value());
  const IncarnationStamp stamp = context.authority->Stamp();
  JsonValue stamp_json = JsonValue::Obj();
  stamp_json.Set("incarnation", JsonValue::UInt(stamp.incarnation.value()));
  if (stamp.generation.IsValid()) {
    stamp_json.Set("generation", JsonValue::Str(stamp.generation.ToString()));
  }
  out.Set("stamp", std::move(stamp_json));
  return out;
}

Result<JsonValue> HandleValidate(const JsonValue& request, RequestContext& context) {
  auto document_json = RequireField(request, "document", "request");
  if (!document_json.has_value()) return document_json.error();
  auto document = ParseRegistryDocument(*document_json.value());
  if (!document.has_value()) return document.error();
  const ValidationReport report = context.authority->ValidateDocument(document.value());
  JsonValue out = report.ToJson();
  if (report.Publishable()) {
    auto simulated = context.authority->SimulatePublish(document.value());
    if (simulated.has_value()) {
      JsonValue generation = JsonValue::Obj();
      generation.Set("number", JsonValue::UInt(simulated.value().id.number.value()));
      generation.Set("digest", JsonValue::Str(simulated.value().id.digest.ToHex()));
      generation.Set("id", JsonValue::Str(simulated.value().id.ToString()));
      out.Set("would_publish_as", std::move(generation));
    }
  }
  return out;
}

Result<JsonValue> HandlePublish(const JsonValue& request, RequestContext& context) {
  if (!context.allow_publish) {
    return MakeError(ErrorCode::ImmutableGeneration,
                     "this service is configured as a read-only replica; publication is refused");
  }
  auto document_json = RequireField(request, "document", "request");
  if (!document_json.has_value()) return document_json.error();
  auto document = ParseRegistryDocument(*document_json.value());
  if (!document.has_value()) return document.error();
  const bool dry_run = request.Find("dry_run") != nullptr &&
                       request.Find("dry_run")->AsBool(false);
  auto expected = ReadGeneration(request, "expected_generation");
  if (!expected.has_value()) {
    return MakeError(ErrorCode::InvalidArgument,
                     "publish requires 'expected_generation'; a publisher must state which "
                     "generation it based its change on");
  }
  if (dry_run) {
    auto simulated = context.authority->SimulatePublish(document.value());
    if (!simulated.has_value()) {
      if (simulated.error().code == ErrorCode::ValidationFailed) {
        Error error = simulated.error();
        error.detail = context.authority->ValidateDocument(document.value()).Render();
        return error;
      }
      return simulated.error();
    }
    JsonValue out = JsonValue::Obj();
    out.Set("dry_run", JsonValue::Bool(true));
    JsonValue generation = JsonValue::Obj();
    generation.Set("number", JsonValue::UInt(simulated.value().id.number.value()));
    generation.Set("digest", JsonValue::Str(simulated.value().id.digest.ToHex()));
    generation.Set("id", JsonValue::Str(simulated.value().id.ToString()));
    out.Set("generation", std::move(generation));
    out.Set("validation", simulated.value().validation.ToJson());
    return out;
  }
  PublishRequest publish_request;
  publish_request.expected_generation = expected.value();
  publish_request.token = context.shutdown_token;
  auto result = context.authority->Publish(document.value(), publish_request);
  if (!result.has_value()) {
    if (result.error().code == ErrorCode::ValidationFailed) {
      Error error = result.error();
      error.detail = context.authority->ValidateDocument(document.value()).Render();
      return error;
    }
    return result.error();
  }
  JsonValue out = JsonValue::Obj();
  out.Set("dry_run", JsonValue::Bool(false));
  JsonValue generation = JsonValue::Obj();
  generation.Set("number", JsonValue::UInt(result.value().id.number.value()));
  generation.Set("digest", JsonValue::Str(result.value().id.digest.ToHex()));
  generation.Set("id", JsonValue::Str(result.value().id.ToString()));
  out.Set("generation", std::move(generation));
  out.Set("rule_count", JsonValue::UInt(result.value().rule_count));
  out.Set("validation", result.value().validation.ToJson());
  return out;
}

Result<std::shared_ptr<const RegistryGeneration>> SelectGeneration(const JsonValue& request,
                                                                  RequestContext& context) {
  auto selector = ReadGeneration(request, "generation");
  if (!selector.has_value()) return context.authority->Snapshot();
  return context.authority->SnapshotAt(selector.value());
}

Result<JsonValue> HandleQueryPair(const JsonValue& request, RequestContext& context) {
  auto snapshot = SelectGeneration(request, context);
  if (!snapshot.has_value()) return snapshot.error();
  auto left_json = RequireField(request, "left", "request");
  if (!left_json.has_value()) return left_json.error();
  auto right_json = RequireField(request, "right", "request");
  if (!right_json.has_value()) return right_json.error();
  auto left = ParseComponentSpec(*left_json.value(), snapshot.value()->taxonomy(), "left");
  if (!left.has_value()) return left.error();
  auto right = ParseComponentSpec(*right_json.value(), snapshot.value()->taxonomy(), "right");
  if (!right.has_value()) return right.error();
  const Decision decision = snapshot.value()->EvaluatePair(left.value(), right.value());
  JsonValue out = DecisionToJson(decision);
  if (request.Find("explain") != nullptr && request.Find("explain")->AsBool(false)) {
    out.Set("explanation", JsonValue::Str(decision.narrative));
  }
  return out;
}

Result<JsonValue> HandleQuerySet(const JsonValue& request, RequestContext& context) {
  auto snapshot = SelectGeneration(request, context);
  if (!snapshot.has_value()) return snapshot.error();
  auto members_json = RequireArray(request, "members", "request");
  if (!members_json.has_value()) return members_json.error();
  if (members_json.value()->empty()) {
    return MakeError(ErrorCode::InvalidArgument, "set query requires at least two members");
  }
  std::vector<ComponentSpec> members;
  members.reserve(members_json.value()->size());
  for (std::size_t i = 0; i < members_json.value()->size(); ++i) {
    auto member = ParseComponentSpec((*members_json.value())[i], snapshot.value()->taxonomy(),
                                     "members[" + std::to_string(i) + "]");
    if (!member.has_value()) return member.error();
    members.push_back(std::move(member).value());
  }
  auto decision = snapshot.value()->EvaluateSet(members);
  if (!decision.has_value()) return decision.error();
  JsonValue out = SetDecisionToJson(decision.value());
  if (request.Find("explain") != nullptr && request.Find("explain")->AsBool(false)) {
    out.Set("explanation", JsonValue::Str(decision.value().narrative));
  }
  return out;
}

Result<JsonValue> HandleDiff(const JsonValue& request, RequestContext& context) {
  auto from = ReadGeneration(request, "from");
  if (!from.has_value()) return from.error();
  auto to = ReadGeneration(request, "to");
  if (!to.has_value()) return to.error();
  auto diff = context.authority->Diff(from.value(), to.value());
  if (!diff.has_value()) return diff.error();
  return GenerationDiffToJson(diff.value());
}

Result<JsonValue> HandleReplay(const JsonValue& request, RequestContext& context) {
  auto from = ReadGeneration(request, "from");
  if (!from.has_value()) return from.error();
  auto to = ReadGeneration(request, "to");
  if (!to.has_value()) return to.error();
  auto queries_json = RequireArray(request, "queries", "request");
  if (!queries_json.has_value()) return queries_json.error();
  auto snapshot = context.authority->SnapshotAt(to.value());
  if (!snapshot.has_value()) return snapshot.error();
  std::vector<std::vector<ComponentSpec>> queries;
  for (std::size_t i = 0; i < queries_json.value()->size(); ++i) {
    const JsonValue& entry = (*queries_json.value())[i];
    const JsonValue::Array* members = entry.AsArray();
    if (members == nullptr) {
      return MakeError(ErrorCode::InvalidArgument,
                       "each replay query must be a JSON array of component specs");
    }
    std::vector<ComponentSpec> query;
    for (std::size_t j = 0; j < members->size(); ++j) {
      auto member = ParseComponentSpec((*members)[j], snapshot.value()->taxonomy(),
                                       "queries[" + std::to_string(i) + "][" + std::to_string(j) +
                                           "]");
      if (!member.has_value()) return member.error();
      query.push_back(std::move(member).value());
    }
    queries.push_back(std::move(query));
  }
  auto replay = context.authority->Replay(queries, from.value(), to.value());
  if (!replay.has_value()) return replay.error();
  JsonValue out = JsonValue::Obj();
  {
    JsonValue json = JsonValue::Obj();
    json.Set("id", JsonValue::Str(replay.value().from.ToString()));
    json.Set("number", JsonValue::UInt(replay.value().from.number.value()));
    json.Set("digest", JsonValue::Str(replay.value().from.digest.ToHex()));
    out.Set("from", std::move(json));
  }
  {
    JsonValue json = JsonValue::Obj();
    json.Set("id", JsonValue::Str(replay.value().to.ToString()));
    json.Set("number", JsonValue::UInt(replay.value().to.number.value()));
    json.Set("digest", JsonValue::Str(replay.value().to.digest.ToHex()));
    out.Set("to", std::move(json));
  }
  out.Set("compared", JsonValue::UInt(replay.value().compared));
  out.Set("changed", JsonValue::UInt(replay.value().changes.size()));
  {
    JsonValue::Array changes;
    changes.reserve(replay.value().changes.size());
    for (const DecisionChange& change : replay.value().changes) {
      JsonValue entry_json = JsonValue::Obj();
      JsonValue::Array subjects;
      for (const ComponentRef& subject : change.subjects) {
        JsonValue item = JsonValue::Obj();
        item.Set("kind", JsonValue::Str(subject.kind.value()));
        item.Set("version", JsonValue::Str(subject.version.ToString()));
        if (!subject.instance.empty()) item.Set("instance", JsonValue::Str(subject.instance.value()));
        subjects.push_back(std::move(item));
      }
      entry_json.Set("subjects", JsonValue::Arr(std::move(subjects)));
      entry_json.Set("before_outcome",
                     JsonValue::Str(std::string(DecisionOutcomeName(change.before_outcome))));
      entry_json.Set("after_outcome",
                     JsonValue::Str(std::string(DecisionOutcomeName(change.after_outcome))));
      entry_json.Set("before_decision", JsonValue::Str(change.before_id.ToHex()));
      entry_json.Set("after_decision", JsonValue::Str(change.after_id.ToHex()));
      if (change.before_rule.has_value()) {
        entry_json.Set("before_rule", JsonValue::Str(change.before_rule->ToString()));
      }
      if (change.after_rule.has_value()) {
        entry_json.Set("after_rule", JsonValue::Str(change.after_rule->ToString()));
      }
      changes.push_back(std::move(entry_json));
    }
    out.Set("changes", JsonValue::Arr(std::move(changes)));
  }
  return out;
}

Result<JsonValue> HandleProvenance(const JsonValue& request, RequestContext& context) {
  auto rule_text = RequireString(request, "rule", "request");
  if (!rule_text.has_value()) return rule_text.error();
  auto id = RuleId::Parse(rule_text.value());
  if (!id.has_value()) return id.error();
  auto history = context.authority->Provenance(id.value());
  if (!history.has_value()) return history.error();
  JsonValue out = JsonValue::Obj();
  out.Set("rule", JsonValue::Str(id.value().value()));
  out.Set("text", JsonValue::Str(RenderProvenanceHistory(history.value())));
  JsonValue::Array records;
  records.reserve(history.value().size());
  for (const ProvenanceRecord& record : history.value()) {
    JsonValue entry = JsonValue::Obj();
    entry.Set("generation", JsonValue::UInt(record.generation.value()));
    entry.Set("present", JsonValue::Bool(record.present));
    entry.Set("rule", JsonValue::Str(record.rule.ToString()));
    entry.Set("rule_digest", JsonValue::Str(record.rule_digest.ToHex()));
    entry.Set("lifecycle", JsonValue::Str(std::string(LifecycleStateName(record.lifecycle))));
    JsonValue provenance = JsonValue::Obj();
    provenance.Set("publisher", JsonValue::Str(record.provenance.publisher.value()));
    provenance.Set("source", JsonValue::Str(record.provenance.source));
    provenance.Set("reference", JsonValue::Str(record.provenance.reference));
    provenance.Set("recorded_at", JsonValue::Str(record.provenance.recorded_at.ToIso8601()));
    provenance.Set("change_note", JsonValue::Str(record.provenance.change_note));
    entry.Set("provenance", std::move(provenance));
    records.push_back(std::move(entry));
  }
  out.Set("records", JsonValue::Arr(std::move(records)));
  return out;
}

Result<JsonValue> HandleGenerations(RequestContext& context) {
  auto generations = context.authority->Generations();
  if (!generations.has_value()) return generations.error();
  JsonValue out = JsonValue::Obj();
  JsonValue::Array array;
  array.reserve(generations.value().size());
  for (GenerationNumber number : generations.value()) {
    array.push_back(JsonValue::UInt(number.value()));
  }
  out.Set("generations", JsonValue::Arr(std::move(array)));
  return out;
}

Result<JsonValue> HandlePrune(const JsonValue& request, RequestContext& context) {
  if (!context.allow_prune) {
    return MakeError(ErrorCode::ImmutableGeneration,
                     "this service is configured without pruning authority");
  }
  auto keep = RequireUInt(request, "keep", "request");
  if (!keep.has_value()) return keep.error();
  if (keep.value() > 100000) {
    return MakeError(ErrorCode::LimitExceeded, "prune retention is unreasonably large");
  }
  auto removed = context.authority->Prune(static_cast<std::size_t>(keep.value()));
  if (!removed.has_value()) return removed.error();
  JsonValue out = JsonValue::Obj();
  JsonValue::Array array;
  array.reserve(removed.value().size());
  for (GenerationNumber number : removed.value()) {
    array.push_back(JsonValue::UInt(number.value()));
  }
  out.Set("removed", JsonValue::Arr(std::move(array)));
  return out;
}

}  // namespace

Result<JsonValue> ExecuteRequest(const JsonValue& request, RequestContext& context) {
  if (context.authority == nullptr) {
    return MakeError(ErrorCode::Internal, "request context has no registry authority");
  }
  if (!request.IsObject()) {
    return MakeError(ErrorCode::InvalidArgument, "a request payload must be a JSON object");
  }
  auto op_text = RequireString(request, "op", "request");
  if (!op_text.has_value()) return op_text.error();
  auto op = ParseOperation(op_text.value());
  if (!op.has_value()) return op.error();

  // Optional fencing stamp: a caller that states which incarnation and
  // generation it believes it is talking to is refused when either has moved.
  const JsonValue* stamp_json = request.Find("stamp");
  if (stamp_json != nullptr && !stamp_json->IsNull()) {
    if (!stamp_json->IsObject()) {
      return MakeError(ErrorCode::InvalidArgument, "stamp must be a JSON object");
    }
    IncarnationStamp stamp;
    auto incarnation = RequireUInt(*stamp_json, "incarnation", "stamp");
    if (!incarnation.has_value()) return incarnation.error();
    stamp.incarnation = Incarnation(incarnation.value());
    const JsonValue* generation = stamp_json->Find("generation");
    if (generation != nullptr && !generation->IsNull()) {
      const std::string* text = generation->AsString();
      if (text == nullptr) {
        return MakeError(ErrorCode::InvalidArgument, "stamp.generation must be a string");
      }
      auto parsed = ParseGenerationId(*text);
      if (!parsed.has_value()) return parsed.error();
      stamp.generation = parsed.value();
    }
    fcr::Status check = context.authority->CheckStamp(stamp);
    if (!check.ok()) return check.error();
  }

  switch (op.value()) {
    case Operation::Ping: {
      JsonValue out = JsonValue::Obj();
      out.Set("service", JsonValue::Str(std::string(kServiceName)));
      out.Set("protocol_version", JsonValue::UInt(kServiceProtocolVersion));
      return out;
    }
    case Operation::Status: return HandleStatus(context);
    case Operation::Validate: return HandleValidate(request, context);
    case Operation::Publish: return HandlePublish(request, context);
    case Operation::QueryPair: return HandleQueryPair(request, context);
    case Operation::QuerySet: return HandleQuerySet(request, context);
    case Operation::Diff: return HandleDiff(request, context);
    case Operation::ReplayDiff: return HandleReplay(request, context);
    case Operation::Provenance: return HandleProvenance(request, context);
    case Operation::Generations: return HandleGenerations(context);
    case Operation::Prune: return HandlePrune(request, context);
    case Operation::Shutdown: {
      if (!context.allow_shutdown || !context.request_shutdown) {
        return MakeError(ErrorCode::InvalidArgument,
                         "this service does not accept remote shutdown requests");
      }
      JsonValue out = JsonValue::Obj();
      out.Set("stopping", JsonValue::Bool(true));
      context.request_shutdown();
      return out;
    }
  }
  return MakeError(ErrorCode::InvalidArgument, "unhandled registry operation");
}

JsonValue DispatchEnvelope(const JsonValue& request, RequestContext& context) {
  Status runtime = Status{};
  (void)runtime;
  auto result = ExecuteRequest(request, context);
  if (!result.has_value()) return ErrorResponse(result.error());
  return OkResponse(std::move(result).value());
}

}  // namespace fcr
