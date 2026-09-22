// Fabric Compatibility Registry - Summon Software Labs
// fcrctl: publish, validate, query, explain, diff and inspect the registry.
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "cli_common.hpp"

namespace {

using fcr::Error;
using fcr::ErrorCode;
using fcr::JsonValue;
using fcr::Result;

struct Arguments {
  std::string command;
  std::vector<std::string> positional;
  std::map<std::string, std::string> flags;
  std::vector<std::string> switches;
};

Result<Arguments> ParseArguments(int argc, char** argv) {
  Arguments out;
  int index = 1;
  if (index < argc && argv[index][0] != '-') {
    out.command = argv[index];
    ++index;
  }
  for (; index < argc; ++index) {
    const std::string token = argv[index];
    if (token.size() >= 2 && token[0] == '-' && token[1] == '-') {
      const std::string name = token.substr(2);
      const std::size_t equals = name.find('=');
      if (equals != std::string::npos) {
        out.flags[name.substr(0, equals)] = name.substr(equals + 1);
        continue;
      }
      if (index + 1 < argc && argv[index + 1][0] != '-') {
        out.flags[name] = argv[index + 1];
        ++index;
        continue;
      }
      out.switches.push_back(name);
      continue;
    }
    out.positional.push_back(token);
  }
  return out;
}

bool HasSwitch(const Arguments& arguments, const std::string& name) {
  return std::find(arguments.switches.begin(), arguments.switches.end(), name) !=
         arguments.switches.end();
}

Result<std::string> RequireFlag(const Arguments& arguments, const std::string& name) {
  const auto it = arguments.flags.find(name);
  if (it == arguments.flags.end()) {
    return fcr::MakeError(ErrorCode::InvalidArgument, "missing required option --" + name);
  }
  return it->second;
}

Result<std::uint64_t> RequireUIntFlag(const Arguments& arguments, const std::string& name) {
  auto text = RequireFlag(arguments, name);
  if (!text.has_value()) return text.error();
  const std::string& value = text.value();
  if (value.empty()) {
    return fcr::MakeError(ErrorCode::InvalidArgument, "--" + name + " must be a number");
  }
  std::uint64_t out = 0;
  for (char c : value) {
    if (c < '0' || c > '9') {
      return fcr::MakeError(ErrorCode::InvalidArgument, "--" + name + " must be a number", value);
    }
    out = out * 10u + static_cast<std::uint64_t>(c - '0');
  }
  return out;
}

void PrintUsage() {
  std::cout <<
      "fcrctl - Fabric Compatibility Registry control tool\n"
      "\n"
      "usage: fcrctl <command> [options]\n"
      "\n"
      "connection:\n"
      "  --data-dir <dir>        open the registry data directory in process\n"
      "                          (default: %FCR_DATA_DIR% or ./fcr-data)\n"
      "  --endpoint <host:port>  talk to a running fcr-registryd instead\n"
      "\n"
      "commands:\n"
      "  status                              service and generation status\n"
      "  generations                         list retained generation numbers\n"
      "  validate <document.json>            static validation report\n"
      "  publish <document.json>             publish the next generation\n"
      "        [--expected-generation N] [--dry-run]\n"
      "  query pair --left <a.json> --right <b.json> [--generation N]\n"
      "  query set --members <members.json> [--generation N]\n"
      "  diff --from N --to M                structural generation diff\n"
      "  replay --from N --to M --queries <queries.json>\n"
      "  provenance --rule <rule-id>         rule history across generations\n"
      "  prune --keep N                      explicit retention pruning\n"
      "  version                             print the runtime version\n"
      "\n"
      "output:\n"
      "  --json       emit canonical JSON instead of the rendered report\n"
      "  --quiet      suppress the rendered report; exit code carries the answer\n"
      "\n"
      "exit codes: 0 ok, 1 error, 2 negative answer, 3 unknown answer\n";
}

int ReportOutcome(const fcr::cli::Options& options, const JsonValue& result,
                  const std::string& text_key) {
  if (options.quiet) return fcr::cli::kExitOk;
  if (options.json) {
    fcr::cli::PrintJson(result, true);
    return fcr::cli::kExitOk;
  }
  const JsonValue* text = result.Find(text_key);
  if (text != nullptr && text->AsString() != nullptr) {
    std::cout << *text->AsString();
    std::string rendered = *text->AsString();
    if (rendered.empty() || rendered.back() != '\n') std::cout << "\n";
    return fcr::cli::kExitOk;
  }
  fcr::cli::PrintJson(result, true);
  return fcr::cli::kExitOk;
}

int OutcomeExitCode(const char* outcome) {
  if (outcome == nullptr) return fcr::cli::kExitOk;
  const std::string text = outcome;
  if (text == "incompatible") return fcr::cli::kExitNegative;
  if (text == "unknown") return fcr::cli::kExitUnknown;
  return fcr::cli::kExitOk;
}

Result<fcr::RegistryDocument> LoadRegistryDocument(const std::string& path) {
  auto json = fcr::cli::LoadJsonFile(path, 8ull * 1024ull * 1024ull);
  if (!json.has_value()) return json.error();
  return fcr::ParseRegistryDocument(json.value());
}

Result<JsonValue> LoadDocument(const std::string& path) {
  auto document = LoadRegistryDocument(path);
  if (!document.has_value()) return document.error();
  // The canonical form of the document is what travels to a service.
  return fcr::RegistryDocumentToJson(document.value());
}

void PrintValidationReport(const fcr::ValidationReport& report) {
  std::cout << "errors:   " << report.error_count << "\n";
  std::cout << "warnings: " << report.warning_count << "\n";
  std::cout << "info:     " << report.info_count << "\n";
  for (const fcr::Diagnostic& diagnostic : report.diagnostics) {
    std::cout << "  [" << fcr::SeverityName(diagnostic.severity) << "] "
              << fcr::DiagnosticCodeName(diagnostic.code) << " "
              << (diagnostic.path.empty() ? std::string("-") : diagnostic.path) << ": "
              << diagnostic.message << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  auto parsed = ParseArguments(argc, argv);
  if (!parsed.has_value()) {
    fcr::cli::PrintError(parsed.error());
    return fcr::cli::kExitError;
  }
  const Arguments arguments = parsed.value();

  if (arguments.command.empty() || arguments.command == "help" ||
      HasSwitch(arguments, "help")) {
    PrintUsage();
    return fcr::cli::kExitOk;
  }
  if (arguments.command == "version") {
    std::cout << "fcrctl " << fcr::kRuntimeVersion << "\n";
    return fcr::cli::kExitOk;
  }

  fcr::cli::Options options;
  const auto data_dir = arguments.flags.find("data-dir");
  options.data_dir = data_dir != arguments.flags.end() ? std::filesystem::path(data_dir->second)
                                                       : std::filesystem::path(fcr::cli::DefaultDataDirectory());
  const auto endpoint = arguments.flags.find("endpoint");
  if (endpoint != arguments.flags.end()) options.endpoint = endpoint->second;
  options.json = HasSwitch(arguments, "json") || arguments.flags.count("json") != 0;
  options.quiet = HasSwitch(arguments, "quiet") || arguments.flags.count("quiet") != 0;
  options.read_write = arguments.command == "publish" || arguments.command == "prune";

  const std::string& command = arguments.command;
  // The session is opened on first use so that a purely local operation (such
  // as validating a document) never needs a registry directory.
  std::unique_ptr<fcr::cli::Session> session;
  auto call = [&](JsonValue request) -> Result<JsonValue> {
    if (!session) {
      auto opened = fcr::cli::Session::Open(options);
      if (!opened.has_value()) return opened.error();
      session = std::move(opened).value();
    }
    return session->Call(request);
  };

  if (command == "status") {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("status"));
    auto result = call(std::move(request));
    if (!result.has_value()) {
      fcr::cli::PrintError(result.error());
      return fcr::cli::kExitError;
    }
    if (options.quiet) return fcr::cli::kExitOk;
    if (options.json) {
      fcr::cli::PrintJson(result.value(), true);
      return fcr::cli::kExitOk;
    }
    const JsonValue& status = result.value();
    const auto text = [&status](const char* key) {
      const JsonValue* value = status.Find(key);
      if (value == nullptr) return std::string("-");
      if (value->AsString() != nullptr) return *value->AsString();
      if (value->AsUInt().has_value()) return std::to_string(value->AsUInt().value());
      if (value->IsBool()) return std::string(value->AsBool(false) ? "true" : "false");
      return std::string("-");
    };
    std::cout << "service:            " << text("service") << " (protocol "
              << text("protocol_version") << ")\n";
    std::cout << "storage:            " << (status.Find("persistent") != nullptr &&
                                                    status.Find("persistent")->AsBool(false)
                                                ? "persistent"
                                                : "in-memory")
              << "\n";
    const JsonValue* generation = status.Find("generation");
    if (generation != nullptr && generation->IsObject()) {
      const JsonValue* id = generation->Find("id");
      std::cout << "generation:         " << (id != nullptr && id->AsString() != nullptr
                                                  ? *id->AsString()
                                                  : std::string("-"))
                << "\n";
    } else {
      std::cout << "generation:         none published yet\n";
    }
    std::cout << "registry name:      " << text("registry_name") << "\n";
    std::cout << "rules:              " << text("rule_count") << " (" << text("active_rule_count")
              << " active)\n";
    std::cout << "capabilities:       " << text("capability_count") << "\n";
    std::cout << "component kinds:    " << text("kind_count") << "\n";
    std::cout << "incarnation:        " << text("incarnation") << "\n";
    std::cout << "publisher epoch:    " << text("publisher_epoch") << "\n";
    std::cout << "publications:       " << text("total_publications") << "\n";
    std::cout << "retained:           " << text("retained_generations") << " generation(s)\n";
    std::cout << "updated:            " << text("updated_at") << "\n";
    return fcr::cli::kExitOk;
  }

  if (command == "generations") {
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("generations"));
    auto result = call(std::move(request));
    if (!result.has_value()) {
      fcr::cli::PrintError(result.error());
      return fcr::cli::kExitError;
    }
    if (options.json) {
      fcr::cli::PrintJson(result.value(), true);
      return fcr::cli::kExitOk;
    }
    const JsonValue* list = result.value().Find("generations");
    if (list != nullptr && list->IsArray()) {
      for (const JsonValue& entry : *list->AsArray()) {
        if (entry.AsUInt().has_value()) std::cout << entry.AsUInt().value() << "\n";
      }
    }
    return fcr::cli::kExitOk;
  }

  if (command == "validate") {
    if (arguments.positional.empty()) {
      std::cerr << "error: validate needs a document path\n";
      return fcr::cli::kExitError;
    }
    if (options.endpoint.empty()) {
      // Static validation is a pure function of the document; no registry
      // directory is required.
      auto local_document = LoadRegistryDocument(arguments.positional.front());
      if (!local_document.has_value()) {
        fcr::cli::PrintError(local_document.error());
        return fcr::cli::kExitError;
      }
      const fcr::ValidationReport report =
          fcr::ValidateRegistryDocument(local_document.value());
      if (options.json) {
        fcr::cli::PrintJson(report.ToJson(), true);
      } else if (!options.quiet) {
        PrintValidationReport(report);
      }
      return report.Publishable() ? fcr::cli::kExitOk : fcr::cli::kExitNegative;
    }
    auto document = LoadDocument(arguments.positional.front());
    if (!document.has_value()) {
      fcr::cli::PrintError(document.error());
      return fcr::cli::kExitError;
    }
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("validate"));
    request.Set("document", document.value());
    auto result = call(std::move(request));
    if (!result.has_value()) {
      fcr::cli::PrintError(result.error());
      return fcr::cli::kExitError;
    }
    if (options.json) {
      fcr::cli::PrintJson(result.value(), true);
    } else if (!options.quiet) {
      const JsonValue* errors = result.value().Find("errors");
      const JsonValue* warnings = result.value().Find("warnings");
      const JsonValue* infos = result.value().Find("infos");
      std::cout << "errors:   " << (errors != nullptr ? errors->AsUInt().value_or(0) : 0) << "\n";
      std::cout << "warnings: " << (warnings != nullptr ? warnings->AsUInt().value_or(0) : 0)
                << "\n";
      std::cout << "info:     " << (infos != nullptr ? infos->AsUInt().value_or(0) : 0) << "\n";
      const JsonValue* diagnostics = result.value().Find("diagnostics");
      if (diagnostics != nullptr && diagnostics->IsArray()) {
        for (const JsonValue& entry : *diagnostics->AsArray()) {
          const JsonValue* severity = entry.Find("severity");
          const JsonValue* code = entry.Find("code");
          const JsonValue* path = entry.Find("path");
          const JsonValue* message = entry.Find("message");
          std::cout << "  [" << (severity != nullptr && severity->AsString() != nullptr
                                     ? *severity->AsString()
                                     : std::string("?"))
                    << "] " << (code != nullptr && code->AsString() != nullptr
                                    ? *code->AsString()
                                    : std::string("?"))
                    << " " << (path != nullptr && path->AsString() != nullptr
                                   ? *path->AsString()
                                   : std::string("-"))
                    << ": " << (message != nullptr && message->AsString() != nullptr
                                    ? *message->AsString()
                                    : std::string("-"))
                    << "\n";
        }
      }
    }
    const JsonValue* publishable = result.value().Find("publishable");
    return publishable != nullptr && publishable->AsBool(false) ? fcr::cli::kExitOk
                                                                : fcr::cli::kExitNegative;
  }

  if (command == "publish") {
    if (arguments.positional.empty()) {
      std::cerr << "error: publish needs a document path\n";
      return fcr::cli::kExitError;
    }
    if (!options.endpoint.empty() && !options.read_write) {
      std::cerr << "error: publish requires write authority\n";
      return fcr::cli::kExitError;
    }
    auto document = LoadDocument(arguments.positional.front());
    if (!document.has_value()) {
      fcr::cli::PrintError(document.error());
      return fcr::cli::kExitError;
    }
    std::uint64_t expected = 0;
    const auto explicit_expected = arguments.flags.find("expected-generation");
    if (explicit_expected != arguments.flags.end()) {
      auto parsed_expected = RequireUIntFlag(arguments, "expected-generation");
      if (!parsed_expected.has_value()) {
        fcr::cli::PrintError(parsed_expected.error());
        return fcr::cli::kExitError;
      }
      expected = parsed_expected.value();
    } else {
      JsonValue status_request = JsonValue::Obj();
      status_request.Set("op", JsonValue::Str("status"));
      auto status = call(std::move(status_request));
      if (!status.has_value()) {
        fcr::cli::PrintError(status.error());
        return fcr::cli::kExitError;
      }
      const JsonValue* generation = status.value().Find("generation");
      if (generation != nullptr && generation->IsObject()) {
        const JsonValue* number = generation->Find("number");
        if (number != nullptr && number->AsUInt().has_value()) {
          expected = number->AsUInt().value();
        }
      }
    }
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("publish"));
    request.Set("document", document.value());
    request.Set("expected_generation", JsonValue::UInt(expected));
    request.Set("dry_run", JsonValue::Bool(HasSwitch(arguments, "dry-run")));
    auto result = call(std::move(request));
    if (!result.has_value()) {
      fcr::cli::PrintError(result.error());
      if (!result.error().detail.empty()) std::cerr << result.error().detail << "\n";
      return fcr::cli::kExitNegative;
    }
    if (options.json) {
      fcr::cli::PrintJson(result.value(), true);
      return fcr::cli::kExitOk;
    }
    const JsonValue* generation = result.value().Find("generation");
    if (!options.quiet) {
      const bool dry = result.value().Find("dry_run") != nullptr &&
                       result.value().Find("dry_run")->AsBool(false);
      std::cout << (dry ? "would publish " : "published ");
      if (generation != nullptr && generation->IsObject()) {
        const JsonValue* id = generation->Find("id");
        std::cout << (id != nullptr && id->AsString() != nullptr ? *id->AsString()
                                                                 : std::string("-"));
      }
      std::cout << "\n";
    }
    return fcr::cli::kExitOk;
  }

  if (command == "query") {
    if (arguments.positional.empty()) {
      std::cerr << "error: query needs 'pair' or 'set'\n";
      return fcr::cli::kExitError;
    }
    const std::string mode = arguments.positional.front();
    JsonValue request = JsonValue::Obj();
    request.Set("explain", JsonValue::Bool(true));
    if (const auto generation = arguments.flags.find("generation");
        generation != arguments.flags.end()) {
      auto value = RequireUIntFlag(arguments, "generation");
      if (!value.has_value()) {
        fcr::cli::PrintError(value.error());
        return fcr::cli::kExitError;
      }
      request.Set("generation", JsonValue::UInt(value.value()));
    }
    if (mode == "pair") {
      auto left_path = RequireFlag(arguments, "left");
      if (!left_path.has_value()) {
        fcr::cli::PrintError(left_path.error());
        return fcr::cli::kExitError;
      }
      auto right_path = RequireFlag(arguments, "right");
      if (!right_path.has_value()) {
        fcr::cli::PrintError(right_path.error());
        return fcr::cli::kExitError;
      }
      auto left = fcr::cli::LoadJsonFile(left_path.value(), 1ull << 20);
      if (!left.has_value()) {
        fcr::cli::PrintError(left.error());
        return fcr::cli::kExitError;
      }
      auto right = fcr::cli::LoadJsonFile(right_path.value(), 1ull << 20);
      if (!right.has_value()) {
        fcr::cli::PrintError(right.error());
        return fcr::cli::kExitError;
      }
      request.Set("op", JsonValue::Str("query_pair"));
      request.Set("left", left.value());
      request.Set("right", right.value());
    } else if (mode == "set") {
      auto members_path = RequireFlag(arguments, "members");
      if (!members_path.has_value()) {
        fcr::cli::PrintError(members_path.error());
        return fcr::cli::kExitError;
      }
      auto members = fcr::cli::LoadJsonFile(members_path.value(), 4ull << 20);
      if (!members.has_value()) {
        fcr::cli::PrintError(members.error());
        return fcr::cli::kExitError;
      }
      request.Set("op", JsonValue::Str("query_set"));
      request.Set("members", members.value());
    } else {
      std::cerr << "error: unknown query mode '" << mode << "'\n";
      return fcr::cli::kExitError;
    }
    auto result = call(std::move(request));
    if (!result.has_value()) {
      fcr::cli::PrintError(result.error());
      return fcr::cli::kExitError;
    }
    const JsonValue* outcome = result.value().Find("outcome");
    const std::string outcome_text =
        outcome != nullptr && outcome->AsString() != nullptr ? *outcome->AsString() : "";
    const int code = ReportOutcome(options, result.value(), "explanation");
    if (code != fcr::cli::kExitOk) return code;
    return OutcomeExitCode(outcome_text.c_str());
  }

  if (command == "diff") {
    auto from = RequireUIntFlag(arguments, "from");
    if (!from.has_value()) {
      fcr::cli::PrintError(from.error());
      return fcr::cli::kExitError;
    }
    auto to = RequireUIntFlag(arguments, "to");
    if (!to.has_value()) {
      fcr::cli::PrintError(to.error());
      return fcr::cli::kExitError;
    }
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("diff"));
    request.Set("from", JsonValue::UInt(from.value()));
    request.Set("to", JsonValue::UInt(to.value()));
    auto result = call(std::move(request));
    if (!result.has_value()) {
      fcr::cli::PrintError(result.error());
      return fcr::cli::kExitError;
    }
    if (options.json) {
      fcr::cli::PrintJson(result.value(), true);
      return fcr::cli::kExitOk;
    }
    if (options.quiet) return fcr::cli::kExitOk;
    const auto count = [&result](const char* key) -> std::uint64_t {
      const JsonValue* value = result.value().Find(key);
      return value != nullptr ? value->AsUInt().value_or(0) : 0;
    };
    std::cout << "added:     " << count("added") << "\n";
    std::cout << "removed:   " << count("removed") << "\n";
    std::cout << "modified:  " << count("modified") << "\n";
    std::cout << "unchanged: " << count("unchanged") << "\n";
    const JsonValue* rules = result.value().Find("rules");
    if (rules != nullptr && rules->IsArray()) {
      for (const JsonValue& entry : *rules->AsArray()) {
        const JsonValue* kind = entry.Find("kind");
        if (kind == nullptr || kind->AsString() == nullptr || *kind->AsString() == "unchanged") {
          continue;
        }
        const JsonValue* rule = entry.Find("rule");
        std::cout << "  " << *kind->AsString() << " "
                  << (rule != nullptr && rule->AsString() != nullptr ? *rule->AsString()
                                                                     : std::string("-"));
        const JsonValue* fields = entry.Find("changed_fields");
        if (fields != nullptr && fields->IsArray() && !fields->AsArray()->empty()) {
          std::cout << " fields=";
          bool first = true;
          for (const JsonValue& field : *fields->AsArray()) {
            if (!first) std::cout << ",";
            first = false;
            if (field.AsString() != nullptr) std::cout << *field.AsString();
          }
        }
        std::cout << "\n";
      }
    }
    const JsonValue* taxonomy = result.value().Find("taxonomy_changes");
    if (taxonomy != nullptr && taxonomy->IsArray()) {
      for (const JsonValue& change : *taxonomy->AsArray()) {
        if (change.AsString() != nullptr) std::cout << "  taxonomy " << *change.AsString() << "\n";
      }
    }
    return fcr::cli::kExitOk;
  }

  if (command == "replay") {
    auto from = RequireUIntFlag(arguments, "from");
    if (!from.has_value()) {
      fcr::cli::PrintError(from.error());
      return fcr::cli::kExitError;
    }
    auto to = RequireUIntFlag(arguments, "to");
    if (!to.has_value()) {
      fcr::cli::PrintError(to.error());
      return fcr::cli::kExitError;
    }
    auto queries_path = RequireFlag(arguments, "queries");
    if (!queries_path.has_value()) {
      fcr::cli::PrintError(queries_path.error());
      return fcr::cli::kExitError;
    }
    auto queries = fcr::cli::LoadJsonFile(queries_path.value(), 8ull << 20);
    if (!queries.has_value()) {
      fcr::cli::PrintError(queries.error());
      return fcr::cli::kExitError;
    }
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("replay"));
    request.Set("from", JsonValue::UInt(from.value()));
    request.Set("to", JsonValue::UInt(to.value()));
    request.Set("queries", queries.value());
    auto result = call(std::move(request));
    if (!result.has_value()) {
      fcr::cli::PrintError(result.error());
      return fcr::cli::kExitError;
    }
    if (options.json) {
      fcr::cli::PrintJson(result.value(), true);
      return fcr::cli::kExitOk;
    }
    if (options.quiet) return fcr::cli::kExitOk;
    const JsonValue* compared = result.value().Find("compared");
    const JsonValue* changed = result.value().Find("changed");
    std::cout << "compared: " << (compared != nullptr ? compared->AsUInt().value_or(0) : 0) << "\n";
    std::cout << "changed:  " << (changed != nullptr ? changed->AsUInt().value_or(0) : 0) << "\n";
    const JsonValue* changes = result.value().Find("changes");
    if (changes != nullptr && changes->IsArray()) {
      for (const JsonValue& entry : *changes->AsArray()) {
        const JsonValue* before = entry.Find("before_outcome");
        const JsonValue* after = entry.Find("after_outcome");
        const JsonValue* subjects = entry.Find("subjects");
        std::cout << "  ";
        if (subjects != nullptr && subjects->IsArray()) {
          bool first = true;
          for (const JsonValue& subject : *subjects->AsArray()) {
            if (!first) std::cout << " vs ";
            first = false;
            const JsonValue* kind = subject.Find("kind");
            const JsonValue* version = subject.Find("version");
            if (kind != nullptr && kind->AsString() != nullptr) std::cout << *kind->AsString();
            if (version != nullptr && version->AsString() != nullptr) {
              std::cout << " " << *version->AsString();
            }
          }
        }
        std::cout << ": "
                  << (before != nullptr && before->AsString() != nullptr ? *before->AsString()
                                                                        : std::string("-"))
                  << " -> "
                  << (after != nullptr && after->AsString() != nullptr ? *after->AsString()
                                                                       : std::string("-"))
                  << "\n";
      }
    }
    const JsonValue* changed_count = result.value().Find("changed");
    return changed_count != nullptr && changed_count->AsUInt().value_or(0) != 0
               ? fcr::cli::kExitNegative
               : fcr::cli::kExitOk;
  }

  if (command == "provenance") {
    auto rule = RequireFlag(arguments, "rule");
    if (!rule.has_value()) {
      fcr::cli::PrintError(rule.error());
      return fcr::cli::kExitError;
    }
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("provenance"));
    request.Set("rule", JsonValue::Str(rule.value()));
    auto result = call(std::move(request));
    if (!result.has_value()) {
      fcr::cli::PrintError(result.error());
      return fcr::cli::kExitError;
    }
    return ReportOutcome(options, result.value(), "text");
  }

  if (command == "prune") {
    auto keep = RequireUIntFlag(arguments, "keep");
    if (!keep.has_value()) {
      fcr::cli::PrintError(keep.error());
      return fcr::cli::kExitError;
    }
    JsonValue request = JsonValue::Obj();
    request.Set("op", JsonValue::Str("prune"));
    request.Set("keep", JsonValue::UInt(keep.value()));
    auto result = call(std::move(request));
    if (!result.has_value()) {
      fcr::cli::PrintError(result.error());
      return fcr::cli::kExitError;
    }
    if (options.json) {
      fcr::cli::PrintJson(result.value(), true);
      return fcr::cli::kExitOk;
    }
    const JsonValue* removed = result.value().Find("removed");
    std::size_t count = 0;
    if (removed != nullptr && removed->IsArray()) count = removed->AsArray()->size();
    if (!options.quiet) std::cout << "pruned " << count << " generation(s)\n";
    return fcr::cli::kExitOk;
  }

  std::cerr << "error: unknown command '" << command << "'\n";
  PrintUsage();
  return fcr::cli::kExitError;
}
