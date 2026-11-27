// One resident workspace request. External tools run between query roots, and their successful
// results reach the graph as inputs at quiescence (#1255).
export module ycxx.build.project;

import std;
import ycxx.execution;
import ycxx.ast;
import ycxx.build.action;
import ycxx.build.inspection_model;
import ycxx.build.module_providers;
import ycxx.build.plan;
import ycxx.build.publication;
import ycxx.build.rule_protocol;
import ycxx.build.rule_snapshot;
import ycxx.build.rule_worker;
import ycxx.build.toolchain_adapter;
import ycxx.build.toolchain_inputs;
import ycxx.build.toolchain_matrix;
import ycxx.engine;
import ycxx.hash;
import ycxx.lsp;
import ycxx.obj.archive;
import ycxx.pipeline;
import ycxx.pp;

export namespace ycxx::build {

struct ProjectReport {
  int status = 0;
  std::string output;
  std::vector<std::string> diagnostics;
  std::size_t actions_executed = 0;
  std::optional<Fp128> watch_stamp = std::nullopt;
};

std::expected<PlanInspection, std::string> inspect_built_project(
    const std::filesystem::path& cwd, std::string_view target, const RuleToolchain& toolchain,
    const ast::Target* selected_target = nullptr);

class Coordinator {
 public:
  Coordinator(pipeline::Session& session, std::string executable, std::uint64_t worker_build);
  ProjectWatchState watch_state(const std::filesystem::path& cwd, const std::string& target,
                                const std::optional<std::string>& tool_path,
                                const ast::Target* selected_target = nullptr);

  ProjectReport run(const std::filesystem::path& cwd, const std::string& target,
                    const std::optional<std::string>& tool_path, bool watch, bool probe_toolchain,
                    const std::function<void()>& refresh, const std::function<bool()>& cancelled,
                    const std::function<void(const std::function<void(engine::Db&)>&)>& apply,
                    const std::function<bool(engine::Revision, const std::function<void()>&)>& publish,
                    const std::function<void()>& tick, engine::Tracer* tracer = nullptr,
                    const ast::Target* selected_target = nullptr);

 private:
  pipeline::Session& session_;
  std::string executable_;
  std::uint64_t worker_build_;
  std::mutex mutex_;
  std::map<std::string, std::set<std::string>> generated_paths_;
  using WatchKey = std::tuple<std::filesystem::path, std::string, std::optional<std::string>, std::string_view>;
  std::mutex watch_mutex_;
  std::map<WatchKey, std::shared_ptr<const RuleSnapshot>> watch_snapshots_;
};

}  // namespace ycxx::build

namespace ycxx::build {

namespace fs = std::filesystem;
using Apply = std::function<void(const std::function<void(engine::Db&)>&)>;
using HostInputOverlay = std::shared_ptr<const pp::HostOverlay>;

struct Planned {
  Fp128 workspace;
  ProjectPlan resolved;
  std::vector<pipeline::RequestId> host_requests;
  std::vector<std::optional<Fp128>> host_overlay_keys;
};
struct [[=engine::input{engine::Durability::low}]] WorkspacePlan {
  using Key = Fp128;  // The requested executable's output path.
  using Value = Planned;
};
struct [[=engine::input{engine::Durability::low}]] ActionReceipt {
  using Key = Fp128;
  using Value = std::optional<std::vector<NamedArtifact>>;
};
struct RuleActionResult {
  using Key = Fp128;
  using Value = ActionReceipt::Value;
  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    co_return cx.fetch<ActionReceipt>(key);
  }
};
struct RulePackageBuild {
  std::vector<NamedArtifact> outputs;
  std::size_t compiler;
};
struct [[=engine::input{engine::Durability::low}]] RulePackageReceipt {
  using Key = Fp128;
  using Value = std::optional<RulePackageBuild>;
};
struct RulePackageResult {
  using Key = RulePackageReceipt::Key;
  using Value = RulePackageReceipt::Value;
  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    co_return cx.fetch<RulePackageReceipt>(key);
  }
};

struct CapabilitySupport {
  using Key = ProbeKey;
  using Value = std::optional<ProbeSupport>;
  static std::string describe(const Key& key) { return ToolchainProbeResult::describe(key); }
  static std::string show(const Value& value) {
    return value ? std::string(probe_support_name(*value)) : "Measurement needed";
  }
  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    const auto& result = co_await cx.fetch<ToolchainProbeResult>(key);
    if (result) co_return result->evidence.support;
    co_return std::nullopt;
  }
};

struct WitnessEvidence {
  int exit_status;
  std::string out;
  std::string err;
};
struct [[=engine::input{engine::Durability::low}]] RuleWitnessReceipt {
  using Key = Fp128;
  using Value = std::optional<WitnessEvidence>;
};
struct RuleWitnessResult {
  using Key = RuleWitnessReceipt::Key;
  using Value = RuleWitnessReceipt::Value;
  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    co_return cx.fetch<RuleWitnessReceipt>(key);
  }
};

struct HostWitnessTool {
  Artifact executable;
  std::vector<HostFileObservation> files;
  std::string error;
};

struct HostWitnessImage {
  using Key = pipeline::RequestId;
  using Value = HostWitnessTool;

  static engine::query_task<Value> run(engine::Ctx& cx, Key request) {
    const auto& built = co_await cx.fetch<pipeline::Build>(request);
    const auto& build = cx.fetch<pipeline::BuildIn>(request);
    std::map<std::string, std::optional<Fp128>> observed;
    for (auto unit : build.units) {
      const pp::FileId file{unit.n};
      const auto& source = cx.fetch<pp::SourceOf>(file);
      const auto* generated = pp::host_overlay_content(source.config, source.cwd, source.path);
      const bool exists = generated ? generated->has_value() : pp::source_content(cx, source, source.path).has_value();
      if (!generated) {
        auto path = (fs::path(source.cwd) / source.path).lexically_normal().generic_string<char>();
        observed.insert_or_assign(std::move(path),
            exists ? std::optional{fingerprint128(cx.fetch<pp::FileText>(file))} : std::nullopt);
      }
      if (!exists) continue;

      (void)co_await cx.fetch<pp::Pp>(file);
      // A changed include probe can leave preprocessing and the linked image equal.
      // Observe the raw reads as well so the file evidence still advances.
      for (const auto input : cx.observed_input_keys<pp::FileContent, pp::Pp>(file)) {
        const auto& contents = cx.fetch<pp::FileContent>(input);
        observed.try_emplace(pp::path_of(input), contents ? std::optional{fingerprint128(*contents)} : std::nullopt);
      }
    }
    Value result;
    for (const auto& [path, contents] : observed) result.files.push_back({path, contents});
    if (built.linked && !built.linked->image.empty()) result.executable = {built.linked->image, true};
    else {
      result.error = pipeline::Build::errors(built);
      if (result.error.empty()) result.error = "build produced no executable";
    }
    co_return result;
  }
};

struct RuleValueData {
  ProbeSupport support = ProbeSupport::indeterminate;
  std::optional<RuleFactValue> fact = std::nullopt;
  bool operator==(const RuleValueData&) const = default;
};

std::string describe_fact_name(std::string_view name) {
  const bool plain = std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.';
  });
  return plain ? std::string(name) : std::format("{:?}", name);
}

std::string describe_rule_value(const RuleValueData& value) {
  if (!value.fact) return std::string(probe_support_name(value.support));
  const auto& fact = *value.fact;
  std::string text = describe_fact_name(fact.schema);
  for (const auto& row : fact.rows) {
    text += std::format("\n{}", describe_fact_name(row.name));
    for (const auto& field : row.fields) {
      const auto name = describe_fact_name(field.name);
      if (field.number)
        text += std::format(" {}={}", name, *field.number);
      else
        text += std::format(" {}={:?}", name, *field.text);
    }
  }
  return text;
}

struct RuleValueRecord {
  RuleValueData value;
  Fp128 receipt;
  std::string origin;
};
// The stable export cell depends on the receipt and returns its semantic value.
struct [[=engine::input{engine::Durability::low}]] RuleValueReceipt {
  using Key = Fp128;
  using Value = std::optional<RuleValueRecord>;
  static std::string show(const Value& value) {
    return value ? std::format("{}\n{}", describe_rule_value(value->value), value->origin)
                 : "Observation needed";
  }
};
struct RuleValue {
  using Key = RuleValueReceipt::Key;
  using Value = std::optional<RuleValueData>;
  static std::string show(const Value& value) {
    return value ? describe_rule_value(*value) : "Observation needed";
  }
  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    const auto& receipt = cx.fetch<RuleValueReceipt>(key);
    if (receipt) co_return receipt->value;
    co_return std::nullopt;
  }
};

struct RuleValueBinding {
  std::string name;
  Fp128 value;
  bool structured;
};
struct [[=engine::input{engine::Durability::low}]] RuleValueBindings {
  using Key = Fp128;
  using Value = std::vector<RuleValueBinding>;
};
struct RuleValueArgument {
  std::string name;
  RuleValueData value;
};
struct RuleValueArguments {
  using Key = RuleValueBindings::Key;
  using Value = std::optional<std::vector<RuleValueArgument>>;
  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    std::vector<RuleValueArgument> result;
    for (const auto& binding : cx.fetch<RuleValueBindings>(key)) {
      const auto& state = co_await cx.fetch<RuleValue>(binding.value);
      if (!state || binding.structured != state->fact.has_value())
        co_return std::nullopt;
      result.push_back({binding.name, *state});
    }
    co_return result;
  }
};

Fp128 rule_value_key(Fp128 workspace, std::string_view target, std::string_view field) {
  return fingerprint(std::tie(workspace, target, field));
}

// BOOTSTRAP: GCC 16 modules: a boxed nested lambda exposes TU-local operations.
struct AdmitProbeReceipts {
  std::span<std::pair<ProbeKey, MeasuredToolchainProbe>> pending;
  void operator()(engine::Db& db) const {
    for (auto& [key, record] : pending) db.set<ToolchainProbeReceipt>(key, std::move(record));
  }
};
template<class Input>
struct AdmitRuleInput {
  Fp128 key;
  typename Input::Value value;
  void operator()(engine::Db& db) const { db.set<Input>(key, value); }
};
struct Prepared {
  Fp128 output;
  Fp128 plan;
  pipeline::RequestId request;
};
struct [[=engine::input{engine::Durability::low}]] PreparedBuild {
  using Key = Fp128;
  using Value = std::optional<Prepared>;
};

Fp128 action_key(const GeneratorPlan& generator, const std::vector<ActionInput>& inputs) {
  const auto& action = generator.action;
  const auto key = [&](const Action& recipe) {
    return fingerprint(std::tie(recipe, inputs, generator.tool_fingerprint));
  };
  const auto resolved_triple = [](const ActionArg& arg) { return !triple_arg_selector(arg.kind).empty(); };
  if (std::ranges::none_of(action.args, resolved_triple)) return key(action);

  // The argv word determines the receipt. Keep its provenance on the plan for traces.
  auto recipe = action;
  for (auto& arg : recipe.args)
    if (resolved_triple(arg)) arg.kind = ActionArg::Kind::literal;
  return key(recipe);
}

Fp128 action_key(const GeneratorPlan& generator) {
  return action_key(generator, generator.inputs);
}

using ActionOutputs = std::vector<NamedArtifact>;
using LookupActionOutputs = std::function_ref<const ActionOutputs*(std::size_t)>;

template<class Lookup>
bool append_artifact_inputs(const GeneratorPlan& generator, Lookup lookup, std::vector<ActionInput>& inputs) {
  for (const auto& edge : generator.artifact_inputs) {
    const auto* outputs = lookup(edge.producer);
    if (!outputs) return false;
    inputs.push_back({edge.name, edge.path, (*outputs)[edge.output].contents});
  }
  std::ranges::sort(inputs, {}, &ActionInput::name);
  return true;
}

struct ActionResolution {
  std::vector<std::optional<Fp128>> keys;
  std::vector<std::size_t> pending;
};

template<class Lookup>
bool append_generator_inputs(const GeneratorPlan& generator, Lookup lookup, std::span<const Artifact> host_images,
                             std::vector<ActionInput>& inputs) {
  if (generator.host_inputs.empty() && generator.artifact_inputs.empty()) return true;
  for (const auto& edge : generator.host_inputs)
    inputs.push_back({edge.name, edge.path, host_images[edge.executable]});
  return append_artifact_inputs(generator, lookup, inputs);
}

template<class Lookup>
ActionResolution resolve_actions(const ProjectPlan& plan, Lookup lookup, std::span<const Artifact> host_images) {
  const auto count = plan.generators.size();
  ActionResolution result{std::vector<std::optional<Fp128>>(count), {}};
  std::vector<const ActionOutputs*> outputs(count, nullptr);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& generator = plan.generators[i];
    Fp128 key;
    if (generator.artifact_inputs.empty() && generator.host_inputs.empty()) {
      key = action_key(generator);
    } else {
      auto inputs = generator.inputs;
      if (!append_generator_inputs(generator, [&](std::size_t dep) { return outputs[dep]; }, host_images, inputs))
        continue;
      key = action_key(generator, inputs);
    }
    result.keys[i] = key;
    outputs[i] = lookup(key);
    if (!outputs[i]) result.pending.push_back(i);
  }
  return result;
}

struct LinkArtifact {
  std::string name;
  std::span<const std::uint8_t> bytes;
};

LinkArtifact link_artifact(const ProjectPlan& plan, const LinkInputPlan& input, LookupActionOutputs output_at,
                           std::string_view native_triple = {}) {
  const auto& generator = plan.generators[input.producer];
  const auto& output = generator.action.outputs[input.output];
  return {generated_path({}, generator.name, output.filename, native_triple),
          (*output_at(input.producer))[input.output].contents.bytes};
}

Fp128 native_plan_key(const ProjectPlan& plan, const std::vector<std::optional<Fp128>>& keys,
                      LookupActionOutputs output_at) {
  std::vector<std::pair<std::string_view, Fp128>> generated;
  for (std::size_t i = 0; i < plan.generators.size(); ++i)
    if (plan.generators[i].native_outputs) generated.emplace_back(plan.generators[i].name, *keys[i]);
  std::vector<std::pair<std::string, Fp128>> archives;
  for (const auto& input : plan.link_inputs) {
    auto [name, bytes] = link_artifact(plan, input, output_at);
    archives.emplace_back(std::move(name), fingerprint128(bytes));
  }
  // Ordered archive bytes enter the link. A recipe edit with the same output
  // retains the existing native request, irrespective of the action receipt key.
  return fingerprint(std::tie(plan.sources, generated, archives));
}

struct Demand {
  ActionResolution actions;
  std::vector<Artifact> host_images;
  Fp128 native_plan;
  bool prepare = false;
  std::vector<std::uint8_t> image;
  std::vector<std::string> errors;
};

void append_module_unit(std::vector<ModuleUnitDependencies>& graph, std::map<pp::FileId, std::size_t>& units,
                        pp::FileId file, const std::string& path, const pipeline::ModuleScan& scan) {
  const auto [first, inserted] = units.try_emplace(file, graph.size());
  auto& unit = graph.emplace_back(ModuleUnitDependencies{path, scan.complete, {}});
  if (!inserted) unit.same_unit_as = first->second;
  for (const auto& directive : scan.directives)
    unit.directives.emplace_back(directive.kind, directive.name);
}

struct ModuleGraph {
  using Key = pipeline::RequestId;
  using Value = std::vector<ModuleUnitDependencies>;

  static engine::query_task<Value> run(engine::Ctx& cx, Key request) {
    const auto& build = cx.fetch<pipeline::BuildIn>(request);
    const auto files = build.units | std::views::transform([](pipeline::TuId unit) { return pp::FileId{unit.n}; });
    const auto scans = co_await cx.par_map<pipeline::ModuleDirectives>(files);
    Value result;
    std::map<pp::FileId, std::size_t> units;
    for (std::size_t i = 0; i < scans.size(); ++i) {
      const auto& source = cx.fetch<pp::SourceOf>(files[i]);
      append_module_unit(result, units, files[i], source.path, scans[i].get());
    }
    co_return result;
  }

  static std::string show(const Value& graph) {
    std::string text;
    for (const auto& unit : graph) {
      for (const auto& [kind, name] : unit.directives)
        std::format_to(std::back_inserter(text), "{}: {} {}\n", unit.source, pipeline::module_directive_name(kind), name);
      if (!unit.complete) std::format_to(std::back_inserter(text), "{}: preprocessing incomplete\n", unit.source);
    }
    if (text.empty()) return "no named module dependencies";
    text.pop_back();
    return text;
  }
};

struct ModuleProviders {
  using Key = ModuleGraph::Key;
  using Value = ModuleResolution;

  static engine::query_task<Value> run(engine::Ctx& cx, Key request) {
    co_return resolve_module_providers(co_await cx.fetch<ModuleGraph>(request));
  }

  static std::string show(const Value& resolution) {
    return std::format("{} imports, {} conflicts, {} cyclic components ({})", resolution.imports.size(),
                       resolution.conflicts.size(), resolution.cycles.size(), resolution.complete ? "complete" : "partial");
  }
};

std::string module_path(const fs::path& path, const fs::path& cwd) {
  const auto relative = path.lexically_relative(cwd);
  return (relative.empty() || *relative.begin() == ".." ? path : relative).generic_string<char>();
}

std::string module_site(const pipeline::ModuleDirective& directive, const fs::path& cwd) {
  return std::format("{}:{}:{}", module_path(directive.path, cwd), directive.line, directive.column);
}

struct ModuleUnitLabel {
  std::string path;
  std::size_t instance;
};

std::vector<ModuleUnitLabel> module_unit_labels(std::span<const ModuleUnitDependencies> graph, const fs::path& cwd) {
  std::map<std::string_view, std::size_t> instances;
  std::vector<ModuleUnitLabel> result;
  for (const auto& unit : graph)
    result.push_back({module_path(unit.source, cwd), ++instances[unit.source]});
  return result;
}

std::string module_unit_display(const ModuleUnitLabel& unit, bool quoted = false) {
  auto text = quoted ? std::format("{:?}", unit.path) : unit.path;
  if (unit.instance > 1) std::format_to(std::back_inserter(text), " #{}", unit.instance);
  return text;
}

using ModuleDirectiveAt = std::function_ref<const pipeline::ModuleDirective&(ModuleProviderSite)>;

struct ModuleBindingText {
  std::string value;
  std::string origin;
};

struct ModuleImportStatus {
  std::string_view code;
  std::string_view display;
};

ModuleImportStatus module_import_status(std::size_t candidates, bool complete, std::string_view name) {
  if (candidates > 1) return {"ambiguous", "ambiguous"};
  if (candidates == 1) return complete ? ModuleImportStatus{"resolved", "resolved"} :
                                         ModuleImportStatus{"provisional", "resolved in partial graph"};
  if (!complete) return {"pending", "no known provider in partial graph"};
  if (name.contains(':')) return {"unresolved_partition", "unresolved partition"};
  return {"external_or_unresolved", "external or unresolved"};
}

ModuleBindingText module_candidates(std::span<const ModuleUnitLabel> units,
                                    std::span<const ModuleProviderSite> providers,
                                    std::string_view status, ModuleDirectiveAt at, const fs::path& cwd) {
  std::vector<std::pair<std::string, std::string>> candidates;
  for (const auto site : providers) {
    const auto& unit = units[site.unit];
    // Quote paths that could obscure candidate boundaries or the unit instance suffix.
    const bool quoted = unit.path.find_first_of(",\"\\\n\r\t") != std::string::npos || unit.path.contains(" #");
    candidates.emplace_back(module_unit_display(unit, quoted), module_site(at(site), cwd));
  }
  std::ranges::sort(candidates);
  ModuleBindingText text{std::string(status), {}};
  for (const auto& [path, origin] : candidates) {
    text.value += text.origin.empty() ? " · " : ", ";
    text.value += path;
    if (!text.origin.empty()) text.origin += ", ";
    text.origin += origin;
  }
  return text;
}

ModuleBindingText module_binding(std::span<const ModuleUnitLabel> units, const ModuleImportResolution& import,
                                 bool complete, ModuleDirectiveAt at, const fs::path& cwd) {
  const auto& directive = at(import.importer);
  const auto status = module_import_status(import.providers.size(), complete, directive.name);
  if (import.providers.empty()) return {std::string(status.display), module_site(directive, cwd)};
  auto text = module_candidates(units, import.providers, status.display, at, cwd);
  text.origin = std::format("{} → {}", module_site(directive, cwd), text.origin);
  return text;
}

std::string module_cycle_members(std::span<const ModuleUnitLabel> units, std::span<const std::size_t> component) {
  std::vector<std::string> paths;
  for (auto unit : component) paths.push_back(module_unit_display(units[unit], true));
  std::ranges::sort(paths);
  std::string text;
  for (const auto& path : paths) {
    if (!text.empty()) text += ", ";
    text += path;
  }
  return text;
}

struct ModuleProviderReport {
  using Key = ModuleProviders::Key;
  using Value = std::string;

  static engine::query_task<Value> run(engine::Ctx& cx, Key request) {
    const auto& resolution = co_await cx.fetch<ModuleProviders>(request);
    if (resolution.imports.empty() && resolution.conflicts.empty())
      co_return resolution.complete ? "no named imports or provider conflicts" : "partial graph";
    const auto& graph = co_await cx.fetch<ModuleGraph>(request);
    const auto& build = cx.fetch<pipeline::BuildIn>(request);
    const auto files = build.units | std::views::transform([](pipeline::TuId unit) { return pp::FileId{unit.n}; });
    const fs::path cwd = cx.fetch<pp::SourceOf>(files[0]).cwd;
    const auto units = module_unit_labels(graph, cwd);
    const auto scans = co_await cx.par_map<pipeline::ModuleDirectives>(files);
    const auto at = [&](ModuleProviderSite site) -> const pipeline::ModuleDirective& {
      return scans[site.unit].get().directives[site.directive];
    };
    std::string text;
    if (!resolution.complete) text += "partial graph\n";
    for (const auto& import : resolution.imports) {
      const auto binding = module_binding(units, import, resolution.complete, at, cwd);
      const auto& directive = at(import.importer);
      std::format_to(std::back_inserter(text), "{} {} · {} · {}\n", pipeline::module_directive_name(directive.kind),
                     directive.name, binding.value, binding.origin);
    }
    for (const auto& conflict : resolution.conflicts) {
      const auto binding = module_candidates(units, conflict.providers, "ambiguous", at, cwd);
      std::format_to(std::back_inserter(text), "provider {} · {} · {}\n", conflict.name, binding.value, binding.origin);
    }
    for (const auto& cycle : resolution.cycles)
      std::format_to(std::back_inserter(text), "cycle among project units · {}\n", module_cycle_members(units, cycle));
    text.pop_back();
    co_return text;
  }

  static std::string show(const Value& text) { return text; }
};

struct WorkspaceBuild {
  using Key = WorkspacePlan::Key;
  using Value = Demand;

  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    const auto& snapshot = cx.fetch<WorkspacePlan>(key);
    const auto& plan = snapshot.resolved;
    Demand demand{};
    const auto host_builds = co_await cx.par_map<pipeline::Build>(snapshot.host_requests);
    for (const auto& [index, built] : std::views::enumerate(host_builds)) {
      const auto& host = built.get();
      if (!host.linked || host.linked->image.empty()) {
        const auto errors = pipeline::Build::errors(host);
        demand.errors.push_back(std::format("host executable '{}': {}", plan.host_executables[index].name,
            errors.empty() ? "build produced no executable" : errors));
        co_return demand;
      }
      demand.host_images.push_back({host.linked->image, true});
    }
    const auto receipt = [&](Fp128 action) -> const ActionOutputs* {
      const auto& stored = cx.fetch<ActionReceipt>(action);
      return stored ? &*stored : nullptr;
    };
    demand.actions = resolve_actions(plan, receipt, demand.host_images);
    if (!demand.actions.pending.empty()) co_return demand;
    demand.native_plan = native_plan_key(plan, demand.actions.keys, [&](std::size_t i) {
      return receipt(*demand.actions.keys[i]);
    });
    const auto& prepared = cx.fetch<PreparedBuild>(snapshot.workspace);
    if (!prepared || prepared->output != key || prepared->plan != demand.native_plan) {
      demand.prepare = true;
      co_return demand;
    }
    (void)co_await cx.fetch<ModuleProviderReport>(prepared->request);
    const auto& native = co_await cx.fetch<pipeline::Build>(prepared->request);
    if (native.linked) {
      demand.image = native.linked->image;
      demand.errors = native.linked->errors;
    }
    if (demand.image.empty()) {
      std::string errors = pipeline::Build::errors(native);
      if (!errors.empty() && demand.errors.empty()) demand.errors.push_back(std::move(errors));
    }
    co_return demand;
  }
};

namespace {

std::string native(const fs::path& path) { return path.generic_string<char>(); }

std::string_view explicit_triple(const ast::Target* target) {
  return target ? target->triple : std::string_view{};
}

template<class Visit>
void for_each_native_output(const ProjectPlan& plan, const fs::path& cwd, std::string_view native_triple,
                            Visit visit) {
  for (std::size_t producer = 0; producer < plan.generators.size(); ++producer) {
    const auto& generator = plan.generators[producer];
    if (!generator.native_outputs) continue;
    for (std::size_t output = 0; output < generator.action.outputs.size(); ++output) {
      const auto& filename = generator.action.outputs[output].filename;
      visit(ArtifactOutputPlan{producer, output}, generated_path(cwd, generator.name, filename, native_triple));
    }
  }
}

std::string normalized_include_directory(const fs::path& path) {
  return native(path.has_filename() || path == path.root_path() ? path : path.parent_path());
}

pipeline::TuId register_project_unit(pipeline::Session& session, const std::string& cwd,
                                     const SourcePlan& source, const ast::Target* target, pp::View view,
                                     HostInputOverlay host_overlay = {}) {
  pp::Config config;
  config.view = view;
  config.target = target;
  config.freestanding = target == &ast::x86_64_linux;
  config.host_overlay = std::move(host_overlay);
  for (const auto& include : source.includes) config.angle_dirs.push_back(normalized_include_directory(include));
  config.system_from = config.angle_dirs.size();
  return session.unit({source.path, cwd, std::move(config), source.path + ".o"});
}

HostInputOverlay host_overlay_for(const std::vector<std::string>& directories, const HostInputOverlay& staged) {
  // An empty view still owns misses in the reserved host directory.
  static const auto empty = std::make_shared<const pp::HostOverlay>(pp::HostOverlay::Files{});
  if (!staged) return empty;
  pp::HostOverlay::Files files;
  for (const auto& [path, bytes] : staged->files())
    if (std::ranges::contains(directories, native(fs::path(path).parent_path())))
      files.emplace(path, bytes);
  if (files.empty()) return empty;
  if (files.size() == staged->files().size()) return staged;
  return std::make_shared<const pp::HostOverlay>(std::move(files));
}

pipeline::RequestId register_host_executable(pipeline::Session& session, const std::string& cwd,
                                             const HostExecutablePlan& executable, const ast::Target* host_target,
                                             const HostInputOverlay& overlay) {
  pipeline::BuildKey build;
  const auto generated_main_directory = [&](const SourcePlan& source) -> std::optional<std::string> {
    if (overlay && overlay->files().contains(source.path)) return native(fs::path(source.path).parent_path());
    return std::nullopt;
  };
  std::vector<std::string> own_generated_directories;
  for (const auto& source : std::span(executable.sources).subspan(executable.first_own_source))
    if (auto directory = generated_main_directory(source))
      own_generated_directories.push_back(std::move(*directory));
  std::map<std::vector<std::string>, HostInputOverlay> overlays;
  for (const auto& [index, source] : std::views::enumerate(executable.sources)) {
    const bool own_source = static_cast<std::size_t>(index) >= executable.first_own_source;
    auto visible = overlay;
    if (overlay || !own_source) {
      // Visible siblings do not change the translation unit's include search.
      auto directories = source.includes;
      if (own_source) directories.append_range(own_generated_directories);
      else if (auto directory = generated_main_directory(source)) directories.push_back(std::move(*directory));
      auto [view, inserted] = overlays.try_emplace(std::move(directories));
      if (inserted) view->second = host_overlay_for(view->first, overlay);
      visible = view->second;
    }
    build.units.push_back(register_project_unit(session, cwd, source, host_target, pp::View::disk, std::move(visible)));
  }
  build.program = session.program({.tus = build.units, .objects = {}, .identifier = executable.name});
  return session.request(std::move(build));
}

pipeline::RequestId register_native(pipeline::Session& session, const std::string& cwd, const std::string& target,
                                    const ast::Target* native_target, const ProjectPlan& plan,
                                    LookupActionOutputs output_at, std::string_view native_triple) {
  pipeline::BuildKey build;
  const auto view = native_triple.empty() ? pp::View::build : pp::View::explicit_target_build;
  for (const auto& source : plan.sources)
    build.units.push_back(register_project_unit(session, cwd, source, native_target, view));
  std::optional<pipeline::ArchiveLinkInputs> archives;
  if (!plan.link_inputs.empty()) {
    using enum pipeline::LinkOperand::Kind;
    const auto format = native_target == &ast::x86_64_linux ? obj::target::x86_64_linux : obj::target::aarch64_macos;
    archives.emplace(pipeline::ArchiveLinkInputs{.target = format, .operands = {}, .archives = {}});
    for (std::size_t i = 0; i < build.units.size(); ++i) archives->operands.push_back({source, i});

    std::map<std::pair<std::size_t, std::size_t>, std::size_t> positions;
    for (const auto& input : plan.link_inputs) {
      const auto [position, inserted] = positions.try_emplace({input.producer, input.output}, archives->archives.size());
      if (inserted) {
        auto [name, bytes] = link_artifact(plan, input, output_at, native_triple);
        archives->archives.emplace_back(std::move(name), std::string(bytes.begin(), bytes.end()));
      }
      archives->operands.push_back({archive, position->second});
    }
  }
  build.program = session.program({.tus = build.units, .objects = {}, .identifier = target,
                                   .archive_link = std::move(archives)});
  return session.request(std::move(build));
}

std::string_view module_directive_kind(pipeline::ModuleDirectiveKind kind) {
  using Kind = pipeline::ModuleDirectiveKind;
  switch (kind) {
    case Kind::interface_unit: return "interface_unit";
    case Kind::implementation_unit: return "implementation_unit";
    case Kind::import: return "import";
    case Kind::exported_import: return "exported_import";
  }
  std::unreachable();
}

ModuleSourceLocation module_location(const pipeline::ModuleDirective& directive, const fs::path& cwd) {
  return {module_path(directive.path, cwd), directive.line, directive.column};
}

std::vector<ModuleCandidateInspection> module_candidate_ids(std::span<const ModuleProviderSite> sites) {
  std::vector<ModuleCandidateInspection> result;
  for (const auto site : sites) result.push_back({site.unit, site.directive});
  return result;
}

void inspect_modules(PlanInspection& inspection, const ProjectPlan& plan, const fs::path& cwd,
                     const ast::Target* target, std::string_view native_triple) {
  auto& report = inspection.module_graph.emplace(ModuleGraphInspection{.native_target = std::string(target->triple)});
  if (plan.sources.empty()) return;
  pipeline::Session session;
  auto& db = session.db();
  std::set<engine::CellId> unavailable;
  for_each_native_output(plan, cwd, native_triple, [&](ArtifactOutputPlan, const std::string& path) {
    const auto id = pp::path_id(pp::Files::normalize(path));
    unavailable.insert(db.cell_of<pp::GeneratedFile>(id).id);
  });
  const auto uses_generated = [&](const auto& dependencies) {
    return std::ranges::any_of(dependencies, [&](const auto& group) {
      return std::ranges::any_of(group, [&](const auto& ref) { return unavailable.contains(ref.id); });
    });
  };
  const std::string workspace = native(cwd);
  std::map<std::string, std::size_t> source_instances;
  std::map<pp::FileId, std::size_t> units;
  std::vector<ModuleUnitDependencies> graph;
  std::vector<pipeline::ModuleScan> scans;
  std::vector<std::vector<std::string>> fact_names;
  for (const auto& source : plan.sources) {
    const auto shown_source = module_unit_display({module_path(source.path, cwd), ++source_instances[source.path]}, true);
    const auto view = native_triple.empty() ? pp::View::build : pp::View::explicit_target_build;
    const auto unit = register_project_unit(session, workspace, source, target, view);
    const auto file = pp::FileId{unit.n};
    auto scan = *session.run<pipeline::ModuleDirectives>(file);
    std::string_view scan_status = "complete";
    // A declared output can change the answer to a negative include probe. An undeclared
    // path stays absent in the live overlay too.
    if (!unavailable.empty() && (uses_generated(db.dependencies<pipeline::ModuleDirectives>(file)) ||
                                 uses_generated(db.dependencies<pp::Pp>(file)))) {
      inspection.facts.push_back({"module-scan-pending", inspection.target, shown_source,
                                  "generated inputs are available in the build trace", {}});
      scan = {};
      scan_status = "generated_inputs_unavailable";
    } else if (!scan.complete) {
      inspection.facts.push_back({"module-scan-pending", inspection.target, shown_source,
                                  "preprocessing incomplete", {}});
      scan_status = "preprocessing_incomplete";
    }
    const auto unit_id = graph.size();
    append_module_unit(graph, units, file, source.path, scan);
    const bool first_unit = !graph.back().same_unit_as;
    if (first_unit) {
      const auto path = module_path(source.path, cwd);
      ModuleUnitConfiguration configuration{logical_module_path(path, native_triple), {}};
      for (const auto& include : source.includes)
        configuration.include_dirs.push_back(
            logical_module_path(module_path(normalized_include_directory(include), cwd), native_triple));
      report.units.push_back({unit_id, path, std::string(scan_status), {}, std::move(configuration)});
    }
    auto& names = fact_names.emplace_back();
    std::map<std::pair<pipeline::ModuleDirectiveKind, std::string_view>, std::size_t> occurrences;
    for (std::size_t index = 0; index < scan.directives.size(); ++index) {
      const auto& directive = scan.directives[index];
      using Kind = pipeline::ModuleDirectiveKind;
      const bool declaration = directive.kind == Kind::interface_unit || directive.kind == Kind::implementation_unit;
      if (first_unit && declaration)
        report.units.back().declarations.push_back({index, std::string(module_directive_kind(directive.kind)),
                                                    directive.name, module_location(directive, cwd)});
      auto name = std::format("{} · {} {}", shown_source, pipeline::module_directive_name(directive.kind), directive.name);
      const auto ordinal = ++occurrences[{directive.kind, directive.name}];
      if (ordinal > 1) std::format_to(std::back_inserter(name), " #{}", ordinal);
      names.push_back(name);
      inspection.facts.push_back({declaration ? "module-unit" : "module-import", inspection.target, std::move(name),
                                  directive.name, module_site(directive, cwd)});
    }
    scans.push_back(std::move(scan));
  }
  auto resolution = resolve_module_providers(graph);
  report.complete = resolution.complete;
  if (resolution.imports.empty() && resolution.conflicts.empty()) return;
  const auto labels = module_unit_labels(graph, cwd);
  if (!resolution.complete)
    inspection.facts.push_back({"module-resolution-pending", inspection.target, "module graph",
                                "provider resolution is provisional", "one or more module scans are pending"});
  const auto at = [&](ModuleProviderSite site) -> const pipeline::ModuleDirective& {
    return scans[site.unit].directives[site.directive];
  };
  for (const auto& import : resolution.imports) {
    auto text = module_binding(labels, import, resolution.complete, at, cwd);
    const auto [unit, directive] = import.importer;
    inspection.facts.push_back({"module-provider", inspection.target, std::move(fact_names[unit][directive]),
                                std::move(text.value), std::move(text.origin)});
    if (!graph[unit].same_unit_as) {
      const auto& occurrence = at(import.importer);
      const auto status = module_import_status(import.providers.size(), resolution.complete, occurrence.name);
      report.imports.push_back({unit, directive, std::string(module_directive_kind(occurrence.kind)), occurrence.name,
                                module_location(occurrence, cwd), std::string(status.code),
                                module_candidate_ids(import.providers)});
    }
  }
  for (const auto& conflict : resolution.conflicts) {
    auto text = module_candidates(labels, conflict.providers, "ambiguous", at, cwd);
    inspection.facts.push_back({"module-provider-conflict", inspection.target, conflict.name,
                                std::move(text.value), std::move(text.origin)});
    report.conflicts.push_back({conflict.name, module_candidate_ids(conflict.providers)});
  }
  // Each name identifies a strongly connected component, never an implied traversal order.
  for (const auto& cycle : resolution.cycles)
    inspection.facts.push_back({"module-cycle", inspection.target, module_cycle_members(labels, cycle),
                                resolution.complete ? "cycle among project units" :
                                                      "cycle among known project units (partial graph)", {}});
  report.cycles = std::move(resolution.cycles);
}

struct RuleInvocation {
  RuleWorkerRequest request;
  std::vector<ActionInput> reads;
};

Artifact text_artifact(std::string_view text) {
  return {std::vector<std::uint8_t>(text.begin(), text.end()), false};
}

void add_rule_read(RuleInvocation& invocation, const std::string& name, Artifact contents) {
  const auto staged = std::format("reads/{}", invocation.reads.size());
  invocation.request.reads.push_back({name, std::format("inputs/{}", staged)});
  invocation.reads.push_back({staged, staged, std::move(contents)});
}

std::expected<RuleInvocation, std::string> rule_invocation(const fs::path& cwd, const RuleDemand& demand) {
  RuleInvocation invocation{
      .request = {.target = demand.target, .rule = demand.rule, .definition = demand.definition,
                  .attributes = demand.attributes, .reads = {}},
      .reads = {}};
  for (const auto& path : demand.reads) {
    std::error_code ec;
    const auto input = cwd / path;
    const auto status = fs::status(input, ec);
    if (status.type() == fs::file_type::not_found) {
      invocation.request.reads.push_back({path, std::nullopt});
      continue;
    }
    if (ec || !fs::is_regular_file(status))
      return std::unexpected(std::format("cannot read rule input '{}'", path));
    std::ifstream stream(input, std::ios::binary);
    if (!stream) return std::unexpected(std::format("cannot read rule input '{}'", path));
    const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
    if (stream.bad()) return std::unexpected(std::format("cannot read rule input '{}'", path));
    add_rule_read(invocation, path, text_artifact(bytes));
  }
  return invocation;
}

Fp128 package_key(const RulePackagePlan& plan) {
  return fingerprint(std::tie(plan.inputs, plan.candidates));
}

Fp128 probe_identity(const RulePackagePlan& plan, const RulePackageAction& selected, std::uint64_t worker_build) {
  return fingerprint(std::tie(selected.action.tool, selected.tool_fingerprint, selected.native,
                              selected.action.environment, plan.probe_flags, plan.probe_inputs, worker_build));
}

Fp128 probe_snapshot_key(const RulePackagePlan& plan, const RulePackageAction& selected, std::uint64_t worker_build) {
  return fingerprint(std::tuple{package_key(plan), probe_identity(plan, selected, worker_build)});
}

bool current_probe_profile(const fs::path& cwd, const RuleToolchain& toolchain, const RuleProbeSnapshot& probe) {
  const auto plan = plan_rule_package(cwd, probe.package, rule_sdk_header(), toolchain);
  return plan && std::ranges::any_of(plan->candidates, [&](const auto& candidate) {
    return candidate.cxx && probe_snapshot_key(*plan, candidate, toolchain.executable_build) == probe.profile;
  });
}

bool current_projection(const fs::path& cwd, const RuleToolchain& toolchain, const RuleProbeSnapshot& probe) {
  return current_probe_profile(cwd, toolchain, probe) &&
         fingerprint_toolchain_probe_inputs(probe.inputs) == probe.stamp;
}

Fp128 observation_key(const RuleInvocation& invocation, std::uint64_t worker_build) {
  return fingerprint(std::tie(invocation.request, invocation.reads, worker_build));
}

std::optional<Fp128> manifest_stamp(const fs::path& cwd) {
  const auto path = cwd / "build.ycxx";
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return std::nullopt;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::nullopt;
  const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
  if (stream.bad()) return std::nullopt;
  return fingerprint128(bytes);
}

struct DeferredHostPlanning {
  RuleObservation observation;
  std::size_t upstream_count;
};

struct PreparedProject {
  std::optional<Fp128> manifest;
  ProjectPlan plan;
  RuleSnapshot rules;
  std::optional<Fp128> attempt_watch;
  std::string_view native_triple;
  std::map<std::string, DeferredHostPlanning> deferred_host_planning;
};

struct ProjectedRuleArguments {
  std::vector<RuleCapabilityValue> values;
  std::vector<RuleFactBinding> structured;
  std::vector<RuleProbeSnapshot> profiles;
  std::vector<PlanFact> facts;
};

struct MeasuredProfile {
  RuleProbeSnapshot snapshot;
  std::vector<ToolchainObservation> observations;
};

ProjectWatchState watch_rules(const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain,
                              const RuleSnapshot& snapshot, std::string_view native_triple,
                              const ProjectWatchState* observed = nullptr);

using RunRuleAction = std::function<std::expected<std::vector<NamedArtifact>, std::string>(
    std::string_view, std::string_view, const Action&, std::span<const ActionInput>, Fp128,
    std::function_ref<bool()>)>;
using RunRulePackage = std::function<std::expected<RulePackageBuild, std::string>(
    const RulePackageRef&, const RulePackagePlan&, Fp128, std::function_ref<bool()>)>;
using RunRuleWitness = std::function<std::expected<WitnessEvidence, std::string>(
    std::string_view, const GeneratorPlan&, Fp128, std::function_ref<bool()>)>;
struct BuiltHostTools {
  std::vector<Artifact> images;
  std::vector<HostFileObservation> files;
  std::string error;
};

using BuildHostTools = std::function<std::optional<BuiltHostTools>(
    const ProjectPlan&, std::span<const std::size_t>, std::span<const HostInputOverlay>)>;
using FindActionReceipt = std::function_ref<const ActionOutputs*(Fp128)>;
using AdmitRuleValue = std::function<void(std::string_view, std::string_view, RuleValueRecord)>;
using UseRuleCompiler = std::function<void(const RulePackageRef&, const RulePackagePlan&, const RulePackageAction&)>;
using ResolveRuleArguments = std::function<std::expected<ProjectedRuleArguments, std::string>(
    const RuleDemand&, RuleSnapshot&)>;

bool current_rules(const fs::path& cwd, const RuleToolchain& toolchain, const PreparedProject& prepared);

Fp128 witness_stamp(const RuleWitnessPlan& plan) {
  if (plan.producers.generators.empty() && plan.producers.host_executables.empty() && !plan.schema)
    return action_key(plan.witness);
  return fingerprint(std::tie(plan.witness, plan.producers, plan.schema));
}

std::optional<Fp128> host_file_contents(std::string_view path) {
  const auto contents = pp::read_disk(pp::path_id(path)).value;
  return contents ? std::optional{fingerprint128(*contents)} : std::nullopt;
}

using ObservedHostFiles = std::map<std::string, std::optional<Fp128>>;

std::optional<Fp128> observe_host_file(ObservedHostFiles& observed, const std::string& path) {
  const auto [entry, inserted] = observed.try_emplace(path);
  if (inserted) entry->second = host_file_contents(path);
  return entry->second;
}

bool current_host_files(std::span<const HostFileObservation> files, ObservedHostFiles* observed = nullptr) {
  return std::ranges::all_of(files, [&](const auto& file) {
    const auto contents = observed ? observe_host_file(*observed, file.path) : host_file_contents(file.path);
    return contents == file.contents;
  });
}

std::optional<RuleValueData> classify_value(std::string_view output, std::string_view field,
                                            const std::optional<std::string>& schema) {
  const auto parsed = lsp::parse(output);
  if (parsed) {
    if (schema) {
      auto reply = lsp::from_json<RuleFactBinding>(*parsed);
      if (reply && reply->name == field && reply->value.schema == *schema && canonical_rule_fact(*reply))
        return RuleValueData{.fact = std::move(reply->value)};
    } else {
      const auto reply = lsp::from_json<RuleCapabilityValue>(*parsed);
      if (reply && reply->name == field && reply->support <= std::to_underlying(ProbeSupport::indeterminate))
        return RuleValueData{.support = static_cast<ProbeSupport>(reply->support)};
    }
  }
  return std::nullopt;
}

struct StagedProducers {
  std::vector<ActionOutputs> outputs;
  std::vector<Fp128> keys;

  const ActionOutputs* output_at(std::size_t i) const { return &outputs.at(i); }
};

struct ProducerClosure {
  std::vector<std::size_t> generators;
  std::vector<std::size_t> hosts;
};

template<class Inputs>
ProducerClosure producer_closure(const ProjectPlan& plan, const Inputs& requested,
                                  std::span<const HostArtifactInputPlan> direct = {}) {
  // Other rule expansions can have added nodes to the plan. Follow only the requested outputs.
  std::vector<bool> needed(plan.generators.size());
  for (const auto& input : requested) needed[input.producer] = true;
  std::set<std::size_t> hosts;
  for (const auto& input : direct) hosts.insert(input.executable);
  ProducerClosure closure;
  for (auto i = needed.size(); i != 0; --i) {
    if (!needed[i - 1]) continue;
    closure.generators.push_back(i - 1);
    const auto& generator = plan.generators[i - 1];
    for (const auto& input : generator.artifact_inputs) needed[input.producer] = true;
    for (const auto& input : generator.host_inputs) hosts.insert(input.executable);
  }
  std::ranges::reverse(closure.generators);
  closure.hosts.append_range(hosts);
  return closure;
}

std::expected<StagedProducers, std::string> stage_producers(
    const ProjectPlan& plan, std::span<const std::size_t> needed, std::span<const Artifact> host_images,
    const RunRuleAction& run, std::function_ref<bool(std::size_t, Fp128)> current) {
  const auto count = plan.generators.size();
  StagedProducers staged{std::vector<ActionOutputs>(count), std::vector<Fp128>(count)};
  const auto output_at = [&](std::size_t i) { return staged.output_at(i); };
  for (const auto i : needed) {
    const auto& producer = plan.generators[i];
    auto inputs = producer.inputs;
    (void)append_generator_inputs(producer, output_at, host_images, inputs);
    const auto key = action_key(producer, inputs);
    auto result = run("ExternalAction", producer.name, producer.action, inputs, key, [&] { return current(i, key); });
    if (!result) return std::unexpected(std::move(result.error()));
    staged.outputs[i] = std::move(*result);
    staged.keys[i] = key;
  }
  return staged;
}

ProducerClosure host_input_closure(const ProjectPlan& plan, std::span<const std::size_t> needed) {
  std::vector<ArtifactOutputPlan> requested;
  for (const auto index : needed) requested.append_range(plan.host_executables[index].generated_inputs);
  return producer_closure(plan, requested);
}

HostInputOverlay host_input_overlay(const fs::path& cwd, const ProjectPlan& plan, std::size_t index,
                                       LookupActionOutputs output_at) {
  std::set<std::size_t> directories;
  pp::HostOverlay::Files files;
  for (const auto& input : plan.host_executables[index].generated_inputs) {
    if (!directories.insert(input.producer).second) continue;
    const auto* contents = output_at(input.producer);
    if (!contents) return {};
    const auto& generator = plan.generators[input.producer];
    // Quoted includes can see the producer's declared siblings too.
    for (const auto& [output, artifact] : std::views::zip(generator.action.outputs, *contents)) {
      const auto& bytes = artifact.contents.bytes;
      files.emplace(host_generated_path(cwd, generator.name, output.filename),
                    std::string(bytes.begin(), bytes.end()));
    }
  }
  if (files.empty()) return {};
  return std::make_shared<const pp::HostOverlay>(std::move(files));
}

std::expected<std::vector<HostInputOverlay>, std::string> stage_host_inputs(
    const fs::path& cwd, const ProjectPlan& plan, std::span<const std::size_t> needed,
    const RunRuleAction& run, std::function_ref<bool(std::size_t, Fp128)> current_action,
    std::function_ref<bool()> current_recipe, std::function_ref<std::unexpected<std::string>()> changed) {
  std::vector<HostInputOverlay> overlays(needed.size());
  const auto closure = host_input_closure(plan, needed);
  if (closure.generators.empty()) return overlays;
  if (!closure.hosts.empty()) return std::unexpected("generated host inputs require host-independent producers");
  if (!current_recipe()) return changed();
  auto staged = stage_producers(plan, closure.generators, {}, run, current_action);
  if (!staged) return std::unexpected(std::move(staged.error()));
  if (!current_recipe()) return changed();

  const auto output_at = [&](std::size_t index) { return staged->output_at(index); };
  for (auto&& [index, overlay] : std::views::zip(needed, overlays))
    overlay = host_input_overlay(cwd, plan, index, output_at);
  return overlays;
}

std::optional<Fp128> current_producer_key(const ProjectPlan& plan, const ProducerClosure& closure,
                                          std::size_t index, std::span<const Artifact> host_images,
                                          FindActionReceipt receipt) {
  std::vector<const ActionOutputs*> outputs(plan.generators.size());
  const auto output_at = [&](std::size_t i) { return outputs[i]; };
  for (const auto i : closure.generators) {
    const auto& generator = plan.generators[i];
    auto inputs = generator.inputs;
    if (!append_generator_inputs(generator, output_at, host_images, inputs)) {
      if (i == index) return std::nullopt;
      continue;
    }
    const auto key = action_key(generator, inputs);
    if (i == index) return key;
    outputs[i] = receipt(key);
  }
  return std::nullopt;
}

bool same_host_images(std::span<const Artifact> left, std::span<const Artifact> right) {
  return std::ranges::equal(left, right, [](const Artifact& a, const Artifact& b) {
    return a.executable == b.executable && a.bytes == b.bytes;
  });
}

bool planning_uses_hosts(const RuleInputPlan& plan) {
  return std::ranges::any_of(plan.inputs, [&](const auto& input) {
    return plan.producers.generators[input.producer].host_dependent;
  });
}

struct HostProducerStage {
  const fs::path& cwd;
  const ProjectPlan& plan;
  ProducerClosure closure;
  const RunRuleAction& run;
  const BuildHostTools& build;
  FindActionReceipt receipt;
  std::shared_ptr<const BuiltHostTools> hosts = {};
  std::optional<std::vector<HostInputOverlay>> overlays = std::nullopt;

  bool current() const {
    return closure.hosts.empty() || (hosts && current_host_files(hosts->files));
  }

  std::span<const Artifact> images() const {
    if (hosts) return hosts->images;
    return {};
  }

  std::expected<StagedProducers, std::string> stage(
      std::function_ref<bool()> current_recipe,
      std::function_ref<void(const std::vector<HostFileObservation>&)> observe,
      std::function_ref<std::unexpected<std::string>()> changed) {
    if (!overlays) {
      auto staged = stage_host_inputs(cwd, plan, closure.hosts, run,
          [&](std::size_t, Fp128) { return current_recipe(); }, current_recipe, changed);
      if (!staged) return std::unexpected(std::move(staged.error()));
      overlays = std::move(*staged);
    }
    const auto refresh = [&] {
      if (!current_recipe()) return false;
      if (current()) return true;
      auto updated = build(plan, closure.hosts, *overlays);
      if (!updated) return false;
      hosts = std::make_shared<const BuiltHostTools>(std::move(*updated));
      observe(hosts->files);
      return current_recipe() && (hosts->error.empty() || current_host_files(hosts->files));
    };
    for (;;) {
      if (!refresh()) return changed();
      if (hosts && !hosts->error.empty()) return std::unexpected(hosts->error);
      const auto staged_hosts = hosts;
      const auto same_images = [&] {
        return hosts == staged_hosts || same_host_images(images(), staged_hosts->images);
      };
      // Keep the bytes of the executing generator immutable. Its guard may
      // refresh host images and look up receipts, but cannot stage a generator.
      auto staged = stage_producers(plan, closure.generators, images(), run, [&](std::size_t index, Fp128 key) {
        if (!plan.generators[index].host_dependent) return current_recipe();
        if (!refresh() || !hosts->error.empty()) return false;
        return same_images() || current_producer_key(plan, closure, index, images(), receipt) == key;
      });
      if (!staged) return std::unexpected(std::move(staged.error()));
      if (!refresh()) return changed();
      if (same_images()) return staged;
    }
  }
};

bool agree_on_shared_host_files(const BuiltHostTools& first, const BuiltHostTools& second) {
  return std::ranges::all_of(first.files, [&](const auto& file) {
    const auto other = std::ranges::lower_bound(second.files, file.path, {}, &HostFileObservation::path);
    return other == second.files.end() || other->path != file.path || other->contents == file.contents;
  });
}

std::expected<std::string, std::string> invoke_rule_worker(
    const RunRuleAction& run, const RuleToolchain& toolchain, std::string_view kind, std::string_view label,
    const Artifact& library, RuleInvocation invocation, std::function_ref<bool()> current) {
  const auto request = lsp::dump(lsp::to_json(invocation.request));
  using enum ActionArg::Kind;
  const Action worker{
      .tool = toolchain.executable,
      .args = {{literal, "--internal-rule-worker"}, {input, "library"}, {input, "request"}, {output, "plan"}},
      .environment = {},
      .outputs = {{"plan", "plan.ycxx"}}};
  auto& inputs = invocation.reads;
  inputs.push_back({"library", "package/rules.so", library});
  inputs.push_back({"request", "request.json", text_artifact(request)});
  const auto key = fingerprint(std::tie(worker, inputs, toolchain.executable_build));
  auto result = run(kind, label, worker, inputs, key, current);
  if (!result) return std::unexpected(std::move(result.error()));
  const auto& bytes = result->front().contents.bytes;
  return std::string(bytes.begin(), bytes.end());
}

std::expected<void, std::string> prepare_project(
    const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain,
    const RunRuleAction& run, const RunRulePackage& run_package, const RunRuleWitness& run_witness,
    const BuildHostTools& build_host_tools, FindActionReceipt receipt,
    const AdmitRuleValue& admit_value, const UseRuleCompiler& use_compiler,
    const ResolveRuleArguments& resolve_arguments, PreparedProject& prepared, bool watch, bool& superseded) {
  const auto native_triple = prepared.native_triple;
  std::vector<RuleDemand> deferred;
  std::optional<RuleDemand> next;
  for (;;) {
    RuleDemand demand;
    if (next) {
      demand = std::move(*next);
      next.reset();
      prepared.deferred_host_planning.erase(demand.target);
    } else {
      std::optional<Fp128> definition;
      ProjectWatchState observed;
      auto attempt = plan_with_rules(cwd, target, prepared.rules.expansions, &definition,
                                     watch ? &observed : nullptr, native_triple);
      // All expansions in one attempt must belong to a single workspace definition.
      if (definition != prepared.manifest) {
        superseded = true;
        return std::unexpected("build.ycxx changed during rule resolution");
      }
      if (!attempt) {
        if (watch)
          prepared.attempt_watch = watch_rules(cwd, target, toolchain, prepared.rules, native_triple, &observed).stamp;
        return std::unexpected(attempt.error());
      }
      if (auto* plan = std::get_if<ProjectPlan>(&*attempt)) {
        prepared.plan = std::move(*plan);
        return {};
      }
      demand = std::move(std::get<RuleDemand>(*attempt));
    }
    if (std::ranges::any_of(prepared.rules.expansions, [&](const auto& entry) { return entry.demand == demand; }))
      return std::unexpected(std::format("rule '{}' did not resolve its declaration", demand.target));
    if (toolchain.executable.empty()) return std::unexpected("cannot locate the rule worker executable");
    if (watch) prepared.attempt_watch = watch_rules(cwd, target, toolchain, prepared.rules, native_triple).stamp;

    auto package = plan_rule_package(cwd, demand.package, rule_sdk_header(), toolchain);
    if (!package) return std::unexpected(package.error());
    const auto library_key = package_key(*package);
    const auto current_package = [&] {
      const auto current = plan_rule_package(cwd, demand.package, rule_sdk_header(), toolchain);
      return current && package_key(*current) == library_key;
    };
    auto built = run_package(demand.package, *package, library_key, [&] {
      return current_package() && current_rules(cwd, toolchain, prepared);
    });
    if (!built) return std::unexpected(built.error());
    use_compiler(demand.package, *package, package->candidates[built->compiler]);

    const auto changed_rule_inputs = [&] {
      superseded = true;
      return std::unexpected(std::format("inputs for rule '{}' changed during resolution", demand.target));
    };
    if (std::ranges::any_of(demand.capabilities, [](const auto& binding) { return !binding.value; }) &&
        !current_rules(cwd, toolchain, prepared)) return changed_rule_inputs();
    auto arguments = resolve_arguments(demand, prepared.rules);
    if (!arguments) return std::unexpected(arguments.error());
    if (watch && !arguments->profiles.empty()) {
      prepared.rules.pending_projections.insert_or_assign(demand.target, arguments->profiles);
      prepared.attempt_watch = watch_rules(cwd, target, toolchain, prepared.rules, native_triple).stamp;
    }
    auto observation = rule_invocation(cwd, demand);
    if (!observation) return std::unexpected(observation.error());
    // Keep the ordinary reads and the generated producer graph independently observable.
    const auto invocation_key = observation_key(*observation, toolchain.executable_build);
    std::optional<RuleInputPlan> planning_plan;
    std::optional<Fp128> planning_stamp;
    if (!demand.plan_inputs.empty()) {
      auto captured = plan_rule_inputs(cwd, demand, prepared.rules.expansions, native_triple);
      if (!captured) return std::unexpected(std::move(captured.error()));
      planning_stamp = fingerprint(*captured);
      planning_plan = std::move(*captured);
    }
    if (!arguments->values.empty()) observation->request.capabilities = std::move(arguments->values);
    if (!arguments->structured.empty()) observation->request.facts = std::move(arguments->structured);
    const bool consumes_values = !demand.facts.empty() || std::ranges::any_of(demand.capabilities, [](const auto& binding) {
      return binding.value.has_value();
    });
    const auto current_invocation = [&] {
      if (!current_rules(cwd, toolchain, prepared)) return false;
      const auto current = rule_invocation(cwd, demand);
      if (!current || observation_key(*current, toolchain.executable_build) != invocation_key ||
          !current_package() || !std::ranges::all_of(arguments->profiles, [&](const auto& profile) {
            return current_projection(cwd, toolchain, profile);
          })) return false;
      if (!planning_stamp) return true;
      const auto current_inputs = plan_rule_inputs(cwd, demand, prepared.rules.expansions, native_triple);
      return current_inputs && fingerprint(*current_inputs) == *planning_stamp;
    };
    const auto watch_hosts = [&](auto& pending, const std::vector<HostFileObservation>& files) {
      if (!watch) return;
      pending.insert_or_assign(demand.target, files);
      prepared.attempt_watch = watch_rules(cwd, target, toolchain, prepared.rules, native_triple).stamp;
    };
    std::optional<std::string> staging_error;
    const auto staging_failure = [&](std::string diagnostic) {
      auto error = std::exchange(staging_error, std::nullopt);
      if (!error) return std::unexpected(std::move(diagnostic));
      // A guard rejected admission because a producer actually failed. Do not
      // run the same failed action again as if its inputs were superseded.
      superseded = false;
      return std::unexpected(std::move(*error));
    };
    std::optional<HostProducerStage> planning_producers;
    std::optional<StagedProducers> planning_staged;
    const auto stage_planning = [&] {
      return planning_producers->stage(current_invocation, [&](const auto& files) {
        watch_hosts(prepared.rules.pending_planning_host_files, files);
      }, changed_rule_inputs);
    };
    if (planning_plan) {
      auto closure = producer_closure(planning_plan->producers, planning_plan->inputs);
      if (!closure.hosts.empty())
        planning_producers.emplace(cwd, planning_plan->producers, std::move(closure), run, build_host_tools, receipt);
      auto staged = planning_producers ? stage_planning()
          : stage_producers(planning_plan->producers, closure.generators, {}, run,
                            [&](std::size_t, Fp128) { return current_invocation(); });
      if (!staged) return std::unexpected(std::move(staged.error()));
      for (const auto& input : planning_plan->inputs) {
        const auto& selected = staged->outputs[input.producer][input.output].contents;
        add_rule_read(*observation, input.name, {selected.bytes, false});
      }
      planning_staged.emplace(std::move(*staged));
    }
    const auto current_planning = [&] {
      if (!current_invocation()) return false;
      if (!planning_producers || planning_producers->current()) return true;
      auto staged = stage_planning();
      if (!staged) {
        if (!superseded) staging_error = std::move(staged.error());
        return false;
      }
      // Logical names and positions come from the checked recipe. Workers can
      // read the selected bytes, independent of mode and producer receipt ID.
      for (const auto& input : planning_plan->inputs)
        if (planning_staged->outputs[input.producer][input.output].contents.bytes !=
            staged->outputs[input.producer][input.output].contents.bytes) return false;
      planning_staged = std::move(*staged);
      return true;
    };
    const auto& library = built->outputs.front().contents;
    auto lowered = invoke_rule_worker(run, toolchain, "RuleWorker", demand.target, library,
                                      *observation, current_planning);
    if (!lowered) return staging_failure(std::move(lowered.error()));
    // A cached worker result bypasses the execution guard.
    if ((consumes_values || planning_producers) && !current_planning())
      return staging_failure(changed_rule_inputs().error());
    RuleExpansion expansion{demand, std::move(*lowered), std::move(arguments->facts)};
    std::optional<Fp128> witness_key;
    std::optional<std::vector<HostFileObservation>> host_files;
    std::optional<PlanFact> exported;
    if (demand.value_export) {
      const auto& field = *demand.value_export;
      const auto label = std::format("{}.{}", demand.target, field);
      if (watch) {
        auto& pending = prepared.rules.pending_witnesses;
        if (auto entry = std::ranges::find(pending, demand, &RuleExpansion::demand); entry != pending.end())
          *entry = expansion;
        else
          pending.push_back(expansion);
        prepared.attempt_watch = watch_rules(cwd, target, toolchain, prepared.rules, native_triple).stamp;
      }
      auto witness_plan = plan_rule_witness(cwd, expansion, prepared.rules.expansions, native_triple);
      if (!witness_plan) {
        if (planning_plan && !current_planning()) return staging_failure(changed_rule_inputs().error());
        return std::unexpected(std::move(witness_plan.error()));
      }
      if (auto* upstream = std::get_if<RuleDemand>(&*witness_plan)) {
        if (planning_plan && !current_planning()) return staging_failure(changed_rule_inputs().error());
        if (upstream->target == demand.target || std::ranges::contains(deferred, upstream->target, &RuleDemand::target))
          return std::unexpected(std::format("witness dependency cycle at target '{}'", upstream->target));
        if (planning_producers)
          prepared.deferred_host_planning.insert_or_assign(demand.target, DeferredHostPlanning{
              {demand, library_key, invocation_key, arguments->profiles, std::nullopt, planning_stamp,
               std::nullopt, planning_producers->hosts->files}, prepared.rules.expansions.size()});
        deferred.push_back(std::move(demand));
        next = std::move(*upstream);
        continue;
      }
      auto& plan = std::get<RuleWitnessPlan>(*witness_plan);
      witness_key = witness_stamp(plan);
      HostProducerStage witness_producers{cwd, plan.producers,
          producer_closure(plan.producers, plan.witness.artifact_inputs, plan.witness.host_inputs),
          run, build_host_tools, receipt};
      const auto current_witness_recipe = [&] {
        if (!current_invocation()) return false;
        const auto current = plan_rule_witness(cwd, expansion, prepared.rules.expansions, native_triple);
        const auto* recipe = current ? std::get_if<RuleWitnessPlan>(&*current) : nullptr;
        return recipe && witness_stamp(*recipe) == *witness_key;
      };
      const auto changed_witness = [&] {
        superseded = true;
        return std::unexpected(std::format("witness '{}' changed during classification", label));
      };
      const auto coherent_host_files = [&] {
        return !planning_producers || !witness_producers.hosts ||
               agree_on_shared_host_files(*planning_producers->hosts, *witness_producers.hosts);
      };
      const auto stage_witness = [&]() -> std::expected<StagedProducers, std::string> {
        if (planning_producers && !current_planning()) return staging_failure(changed_witness().error());
        const auto current_staging_recipe = [&] {
          // The producer guard can invalidate an obsolete expansion while the
          // read restaging itself stays outside the guard.
          return current_witness_recipe() && (!planning_producers || planning_producers->current());
        };
        auto staged = witness_producers.stage(current_staging_recipe, [&](const auto& files) {
          watch_hosts(prepared.rules.pending_host_files, files);
        }, changed_witness);
        if (staged || superseded || !planning_producers) return staged;
        // A worker's earlier expansion may select a generator that fails only
        // under obsolete planning bytes. Recheck outside the producer guard.
        if (!current_planning()) return staging_failure(changed_witness().error());
        // A prerequisite can fail before there is any host image to recheck.
        if (!current_witness_recipe() || (witness_producers.hosts && !witness_producers.current()) ||
            !coherent_host_files())
          return changed_witness();
        return staged;
      };
      const auto staged_inputs = [&](const StagedProducers& staged) {
        auto inputs = plan.witness.inputs;
        (void)append_generator_inputs(plan.witness, [&](std::size_t i) { return staged.output_at(i); },
                                      witness_producers.images(), inputs);
        return inputs;
      };
      auto compiled = stage_witness();
      if (!compiled) return std::unexpected(std::move(compiled.error()));
      auto invocation = plan.witness;
      invocation.inputs = staged_inputs(*compiled);
      const auto witness_action = action_key(invocation);
      const auto current_witness = [&] {
        for (;;) {
          if (planning_producers && !current_planning()) return false;
          if (!current_witness_recipe()) return false;
          if (witness_producers.current()) return coherent_host_files();
          // Restage outside either producer guard. Recheck planning after a
          // witness producer ran so the classifier sees one coherent set of reads.
          auto staged = stage_witness();
          if (!staged) {
            if (!superseded) staging_error = std::move(staged.error());
            return false;
          }
          if (action_key(plan.witness, staged_inputs(*staged)) != witness_action) return false;
          compiled = std::move(staged);
        }
      };
      if (!current_witness()) return staging_failure(changed_witness().error());
      auto evidence = run_witness(label, invocation, witness_action, current_witness);
      if (!evidence) return staging_failure(std::move(evidence.error()));
      observation->request.witness = RuleWitnessReport{
          field, evidence->exit_status, std::move(evidence->out), std::move(evidence->err), plan.schema};
      const auto classification_key = observation_key(*observation, toolchain.executable_build);
      const auto value_receipt = fingerprint(std::tie(library_key, witness_action, classification_key));
      auto classified = invoke_rule_worker(run, toolchain, "RuleClassifier", label, library,
                                           std::move(*observation), current_witness);
      if (!classified) return staging_failure(std::move(classified.error()));
      if (!current_witness()) return staging_failure(changed_witness().error());
      auto value = classify_value(*classified, field, plan.schema);
      if (!value) return std::unexpected(std::format("invalid classification for '{}'", label));
      auto origin = std::format("{} via {} · {} · witness exit {} · receipt {:016x}{:016x}", label,
          plan.witness.action.tool, demand.package.target, evidence->exit_status, value_receipt.lo, value_receipt.hi);
      for (const auto& input : plan.witness.artifact_inputs) {
        const auto key = compiled->keys[input.producer];
        origin += std::format(" · artifact {} receipt {:016x}{:016x}", input.name, key.lo, key.hi);
      }
      exported.emplace("value-export", demand.target, field, describe_rule_value(*value), origin);
      admit_value(demand.target, field, RuleValueRecord{std::move(*value), value_receipt, std::move(origin)});
      std::erase_if(prepared.rules.pending_witnesses, [&](const auto& entry) { return entry.demand == demand; });
      prepared.rules.pending_host_files.erase(demand.target);
      if (witness_producers.hosts) host_files = witness_producers.hosts->files;
    }
    if (planning_plan)
      for (const auto& input : planning_plan->inputs) {
        const auto& producer = planning_plan->producers.generators[input.producer];
        const auto& output = producer.action.outputs[input.output];
        const auto action = planning_staged->keys[input.producer];
        expansion.capability_facts.push_back({"plan-input", demand.target, input.name,
            generated_path({}, producer.name, output.filename, native_triple),
            std::format("artifact({:?}, {:?}) · receipt {:016x}{:016x}", producer.name, output.name,
                        action.lo, action.hi)});
      }
    if (exported) expansion.capability_facts.push_back(std::move(*exported));
    std::optional<std::vector<HostFileObservation>> planning_host_files;
    if (planning_producers) planning_host_files = planning_producers->hosts->files;
    prepared.rules.pending_projections.erase(demand.target);
    prepared.rules.pending_planning_host_files.erase(demand.target);
    prepared.rules.observations.push_back(
        {std::move(demand), library_key, invocation_key, std::move(arguments->profiles), witness_key, planning_stamp,
         std::move(host_files), std::move(planning_host_files)});
    prepared.rules.expansions.push_back(std::move(expansion));
    if (!deferred.empty()) {
      next = std::move(deferred.back());
      deferred.pop_back();
    }
  }
}

// Share observations only within a single validation pass. A later effect or
// publication must reobserve the filesystem even if the receipt keys are unchanged.
using ObservedPackageStamps = std::map<std::pair<RulePackageRef::Kind, std::string>, std::optional<Fp128>>;

bool current_rule_inputs(const fs::path& cwd, const RuleToolchain& toolchain, const RuleObservation& record,
                         std::span<const RuleExpansion> upstream, ObservedPackageStamps& packages,
                         ObservedHostFiles& files, std::string_view native_triple) {
  const auto& ref = record.demand.package;
  const auto [cached, inserted] = packages.try_emplace({ref.kind, ref.target});
  if (inserted)
    if (const auto package = plan_rule_package(cwd, ref, rule_sdk_header(), toolchain))
      cached->second = package_key(*package);
  if (cached->second != record.package) return false;
  const auto observed = rule_invocation(cwd, record.demand);
  if (!observed || observation_key(*observed, toolchain.executable_build) != record.invocation ||
      !std::ranges::all_of(record.projections, [&](const auto& profile) {
        return current_projection(cwd, toolchain, profile);
      })) return false;
  if (record.planning) {
    const auto planning = plan_rule_inputs(cwd, record.demand, upstream, native_triple);
    if (!planning || fingerprint(*planning) != *record.planning ||
        record.planning_host_files.has_value() != planning_uses_hosts(*planning) ||
        (record.planning_host_files && !current_host_files(*record.planning_host_files, &files))) return false;
  }
  return true;
}

bool current_rule(const fs::path& cwd, const RuleToolchain& toolchain, const RuleObservation& record,
                  const RuleExpansion& expansion, std::span<const RuleExpansion> upstream,
                  ObservedPackageStamps& packages, ObservedHostFiles& files, std::string_view native_triple) {
  if (!current_rule_inputs(cwd, toolchain, record, upstream, packages, files, native_triple)) return false;
  if (!record.witness) return true;
  const auto witness = plan_rule_witness(cwd, expansion, upstream, native_triple);
  const auto* recipe = witness ? std::get_if<RuleWitnessPlan>(&*witness) : nullptr;
  if (!recipe || witness_stamp(*recipe) != *record.witness ||
      record.host_files.has_value() != recipe->witness.host_dependent) return false;
  return !record.host_files || current_host_files(*record.host_files, &files);
}

bool current_rules(const fs::path& cwd, const RuleToolchain& toolchain, const PreparedProject& prepared) {
  if (manifest_stamp(cwd) != prepared.manifest) return false;
  ObservedPackageStamps packages;
  ObservedHostFiles files;
  for (std::size_t i = 0; i < prepared.rules.observations.size(); ++i)
    if (!current_rule(cwd, toolchain, prepared.rules.observations[i], prepared.rules.expansions[i],
                      std::span(prepared.rules.expansions).first(i), packages, files, prepared.native_triple))
      return false;
  // An upstream action can belong to a witness demand whose planning inputs
  // changed while the demand was deferred.
  return std::ranges::all_of(prepared.deferred_host_planning, [&](const auto& entry) {
    const auto& deferred = entry.second;
    return current_rule_inputs(cwd, toolchain, deferred.observation,
        std::span(prepared.rules.expansions).first(deferred.upstream_count), packages, files, prepared.native_triple);
  });
}

std::optional<ProjectPlan> observe_project(
    const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain, const PreparedProject& prepared) {
  if (!current_rules(cwd, toolchain, prepared)) return std::nullopt;
  std::optional<Fp128> definition;
  auto attempt = plan_with_rules(cwd, target, prepared.rules.expansions, &definition, nullptr, prepared.native_triple);
  if (!attempt || definition != prepared.manifest) return std::nullopt;
  if (auto* plan = std::get_if<ProjectPlan>(&*attempt)) return std::move(*plan);
  return std::nullopt;
}

ProjectWatchState watch_rules(const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain,
                              const RuleSnapshot& snapshot, std::string_view native_triple,
                              const ProjectWatchState* observed) {
  std::vector<RuleExpansion> current;
  std::vector<Fp128> observations;
  std::vector<std::string> paths;
  struct PackageObservation {
    bool available;
    Fp128 package;
    Fp128 configuration;
  };
  std::map<std::pair<RulePackageRef::Kind, std::string>, PackageObservation> packages;
  const auto observe_package = [&](const RulePackageRef& reference) {
    const auto key = std::pair{reference.kind, reference.target};
    auto package = packages.find(key);
    if (package == packages.end()) {
      std::vector<std::string> failed_paths;
      const auto plan = plan_rule_package(cwd, reference, rule_sdk_header(), toolchain, &failed_paths);
      paths.append_range(plan ? plan->observed_paths : failed_paths);
      const auto stamp = plan ? package_key(*plan) : fingerprint128(plan.error());
      const auto config = plan ? fingerprint(std::tuple{
          plan->probe_flags, plan->probe_inputs, fingerprint_toolchain_probe_inputs({plan->probe_inputs, {}})})
          : Fp128{};
      package = packages.emplace(key, PackageObservation{plan.has_value(), stamp, config}).first;
    }
    return package->second;
  };
  const auto observe = [&](const RuleDemand& demand) -> std::optional<std::pair<Fp128, Fp128>> {
    const auto package = observe_package(demand.package);
    for (const auto& binding : demand.capabilities) {
      if (binding.value) continue;
      const auto reference = capability_package(binding.package);
      if (reference) observations.push_back(fingerprint(observe_package(*reference)));
    }
    for (const auto& path : demand.reads) paths.push_back(native(cwd / path));
    const auto invocation = rule_invocation(cwd, demand);
    const auto invocation_stamp = invocation ? observation_key(*invocation, toolchain.executable_build)
                                             : fingerprint128(invocation.error());
    observations.push_back(fingerprint(std::tuple{package.available, package.package, invocation.has_value(),
                                                    invocation_stamp}));
    if (!package.available || !invocation) return std::nullopt;
    return std::pair{package.package, invocation_stamp};
  };
  const auto observe_probe = [&](const RuleProbeSnapshot& probe) {
    const auto matches = current_probe_profile(cwd, toolchain, probe);
    const auto stamp = matches ? fingerprint_toolchain_probe_inputs(probe.inputs) : Fp128{};
    observations.push_back(fingerprint(std::tie(matches, stamp)));
    if (matches) {
      paths.append_range(probe.inputs.files);
      paths.append_range(probe.inputs.trees);
    }
    return matches && stamp == probe.stamp;
  };
  const auto observe_planning = [&](const RuleDemand& demand, std::optional<bool> has_host_files = std::nullopt)
      -> std::optional<Fp128> {
    if (demand.plan_inputs.empty()) return std::nullopt;
    std::vector<std::string> input_paths;
    const auto plan = plan_rule_inputs(cwd, demand, current, native_triple, &input_paths);
    paths.append_range(std::move(input_paths));
    if (!plan) {
      observations.push_back(fingerprint128(plan.error()));
      return std::nullopt;
    }
    const auto stamp = fingerprint(*plan);
    observations.push_back(stamp);
    if (has_host_files && planning_uses_hosts(*plan) != *has_host_files) return std::nullopt;
    return stamp;
  };
  const auto observe_witness = [&](const RuleExpansion& expansion) -> std::optional<Fp128> {
    std::vector<std::string> witnessed_paths;
    const auto plan = plan_rule_witness(cwd, expansion, current, native_triple, &witnessed_paths);
    paths.append_range(std::move(witnessed_paths));
    if (!plan) {
      observations.push_back(fingerprint128(plan.error()));
      return std::nullopt;
    }
    if (const auto* demand = std::get_if<RuleDemand>(&*plan)) {
      (void)observe(*demand);
      (void)observe_planning(*demand);
      observations.push_back(fingerprint(*demand));
      return std::nullopt;
    }
    const auto stamp = witness_stamp(std::get<RuleWitnessPlan>(*plan));
    observations.push_back(stamp);
    return stamp;
  };
  ObservedHostFiles host_reads;
  const auto observe_host_files = [&](std::span<const HostFileObservation> files) {
    bool current = true;
    for (const auto& file : files) {
      const auto contents = observe_host_file(host_reads, file.path);
      observations.push_back(fingerprint(std::tie(file.path, contents)));
      paths.push_back(file.path);
      current = current && contents == file.contents;
    }
    return current;
  };
  for (std::size_t i = 0; i < snapshot.observations.size(); ++i) {
    const auto& record = snapshot.observations[i];
    const auto& expansion = snapshot.expansions[i];
    const auto state = observe(record.demand);
    bool profiles_current = true;
    for (const auto& probe : record.projections)
      if (!observe_probe(probe)) profiles_current = false;
    const bool planning_current = !record.planning ||
        observe_planning(record.demand, record.planning_host_files.has_value()) == record.planning;
    const bool witness_current = !record.witness || observe_witness(expansion) == record.witness;
    const bool host_current = !record.host_files || observe_host_files(*record.host_files);
    const bool planning_host_current = !record.planning_host_files || observe_host_files(*record.planning_host_files);
    if (profiles_current && planning_current && witness_current && host_current && planning_host_current &&
        state == std::pair{record.package, record.invocation})
      current.push_back(expansion);
  }
  for (const auto& probe : snapshot.probes) (void)observe_probe(probe);
  for (const auto& [name, profiles] : snapshot.pending_projections)
    for (const auto& probe : profiles) (void)observe_probe(probe);
  for (const auto& witness : snapshot.pending_witnesses) (void)observe_witness(witness);
  for (const auto& entry : snapshot.pending_host_files) (void)observe_host_files(entry.second);
  for (const auto& entry : snapshot.pending_planning_host_files) (void)observe_host_files(entry.second);
  auto watch = observed ? *observed : watch_project(cwd, target, current, native_triple);
  if (watch.pending && std::ranges::none_of(snapshot.observations, [&](const auto& record) {
        return record.demand == *watch.pending;
      })) {
    (void)observe(*watch.pending);
    (void)observe_planning(*watch.pending);
  }
  watch.stamp = fingerprint(std::tie(watch.stamp, observations, native_triple));
  watch.paths.append_range(std::move(paths));
  std::ranges::sort(watch.paths);
  auto duplicates = std::ranges::unique(watch.paths);
  watch.paths.erase(duplicates.begin(), duplicates.end());
  return watch;
}

}  // namespace

std::expected<PlanInspection, std::string> inspect_built_project(
    const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain, const ast::Target* selected_target) {
  const auto native_triple = explicit_triple(selected_target);
  auto snapshot = read_rule_snapshot(cwd, target, native_triple);
  std::vector<RuleExpansion> current;
  if (snapshot) {
    ObservedPackageStamps packages;
    ObservedHostFiles files;
    for (std::size_t i = 0; i < snapshot->observations.size(); ++i) {
      if (current_rule(cwd, toolchain, snapshot->observations[i], snapshot->expansions[i], current,
                       packages, files, native_triple))
        current.push_back(std::move(snapshot->expansions[i]));
    }
  }
  ProjectPlan plan;
  auto inspection = inspect_project(cwd, target, current, native_triple, plan);
  const auto* native_target = selected_target ? selected_target : pp::native_target;
  if (inspection && native_target) {
    inspection->facts.push_back({"native-target", std::string(target), "triple", std::string(native_target->triple),
                                 selected_target ? "--target" : "host default"});
    if (!std::ranges::contains(inspection->facts, unresolved_fact_kind, &PlanFact::kind))
      inspect_modules(*inspection, plan, cwd, native_target, native_triple);
  }
  if (inspection && snapshot)
    for (auto& probe : snapshot->probes) {
      const bool expanded = std::ranges::any_of(current, [&](const auto& expansion) {
        return expansion.demand.package == probe.package;
      });
      const bool projected = std::ranges::any_of(snapshot->observations, [&](const auto& record) {
        return std::ranges::any_of(record.projections, [&](const auto& projection) {
          return projection.package == probe.package;
        }) && std::ranges::any_of(inspection->facts, [&](const auto& fact) {
          return fact.kind == "capability-input" && fact.target == record.demand.target;
        });
      });
      if ((expanded || projected) && current_projection(cwd, toolchain, probe))
        inspection->facts.append_range(std::move(probe.facts));
    }
  return inspection;
}

Coordinator::Coordinator(pipeline::Session& session, std::string executable, std::uint64_t worker_build)
    : session_(session), executable_(std::move(executable)), worker_build_(worker_build) {
  using enum engine::Durability;
  session_.db().set_provider<ActionReceipt>([](const Fp128&) -> engine::provided<ActionReceipt::Value> {
    return {std::nullopt, low};
  });
  session_.db().set_provider<RulePackageReceipt>([](const Fp128&) -> engine::provided<RulePackageReceipt::Value> {
    return {std::nullopt, low};
  });
  session_.db().set_provider<ToolchainProbeReceipt>(
      [](const ProbeKey&) -> engine::provided<ToolchainProbeReceipt::Value> { return {std::nullopt, low}; });
  session_.db().set_provider<RuleWitnessReceipt>([](const Fp128&) -> engine::provided<RuleWitnessReceipt::Value> {
    return {std::nullopt, low};
  });
  session_.db().set_provider<RuleValueReceipt>([](const Fp128&) -> engine::provided<RuleValueReceipt::Value> {
    return {std::nullopt, low};
  });
  session_.db().set_provider<RuleValueBindings>([](const Fp128&) -> engine::provided<RuleValueBindings::Value> {
    return {{}, low};
  });
  session_.db().set_provider<PreparedBuild>([](const Fp128&) -> engine::provided<PreparedBuild::Value> {
    return {std::nullopt, low};
  });
}

ProjectWatchState Coordinator::watch_state(const fs::path& cwd, const std::string& target,
                                            const std::optional<std::string>& tool_path,
                                            const ast::Target* selected_target) {
  const auto native_triple = explicit_triple(selected_target);
  std::shared_ptr<const RuleSnapshot> snapshot;
  {
    std::lock_guard lock(watch_mutex_);
    const auto entry = watch_snapshots_.find({cwd, target, tool_path, native_triple});
    if (entry != watch_snapshots_.end()) snapshot = entry->second;
  }
  static const RuleSnapshot empty;
  return watch_rules(cwd, target, {executable_, worker_build_, tool_path}, snapshot ? *snapshot : empty, native_triple);
}

ProjectReport Coordinator::run(const fs::path& cwd, const std::string& target,
                               const std::optional<std::string>& tool_path, bool watch, bool probe_toolchain,
                               const std::function<void()>& refresh, const std::function<bool()>& cancelled,
                               const Apply& apply,
                               const std::function<bool(engine::Revision, const std::function<void()>&)>& publish,
                               const std::function<void()>& tick, engine::Tracer* tracer,
                               const ast::Target* selected_target) {
  std::lock_guard lock(mutex_);
  ProjectReport report;
  const auto native_triple = explicit_triple(selected_target);
  const auto state_path = project_state_path(cwd, native_triple);
  const auto output = state_path / "out" / target;
  const auto key = fingerprint128(native(output));
  const std::string workspace_path = native(cwd);
  // Native overlays have one active plan per workspace and target configuration.
  const auto native_workspace_path = native(state_path);
  const auto native_workspace = fingerprint128(native_workspace_path);
  const RuleToolchain toolchain{executable_, worker_build_, tool_path};
  bool superseded = false;
  const auto fail = [&](std::string diagnostic) {
    report.status = 1;
    report.diagnostics.push_back(std::move(diagnostic));
    return report;
  };
  const auto* host_target = pp::native_target;
  if (!host_target) return fail("project building is unavailable on this host");
  const auto* native_target = selected_target ? selected_target : host_target;
  // A witness can now observe different inputs for different target triples. Its
  // logical export is scoped by triple, while action receipts still key actual inputs.
  const auto value_scope = fingerprint(std::tie(workspace_path, native_target->triple));
  if (tracer) {
    engine::TraceEvent event;
    event.id = session_.db().reserve_trace_id();
    event.kind = "ProjectTarget";
    event.key = native_target->triple;
    event.start = event.end = std::chrono::steady_clock::now();
    event.outcome = engine::Outcome::executed;
    if (tracer->values()) event.value = selected_target ? "--target" : "host default";
    tracer->event(std::move(event));
  }

  const auto poll = [&] {
    tick();
    return cancelled();
  };
  const auto run_action = [&](std::string_view kind, std::string_view label, const Action& action,
                              std::span<const ActionInput> inputs,
                              std::chrono::milliseconds timeout = {},
                              std::optional<std::size_t> log_limit = std::nullopt) -> ActionResult {
    const auto scratch = build_directory(cwd, "jobs");
    if (!scratch) return {1, {}, scratch.error(), {}, ActionCompletion::setup_failed};
    ycxx::exec::inplace_stop_source stop;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timed_out = false;
    std::jthread monitor([&](std::stop_token finished) {
      while (!finished.stop_requested()) {
        timed_out = timeout.count() != 0 && std::chrono::steady_clock::now() >= deadline;
        if (timed_out || cancelled()) {
          stop.request_stop();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
    ++report.actions_executed;
    engine::TraceEvent event;
    if (tracer) {
      event.id = session_.db().reserve_trace_id();
      event.kind = kind;
      event.key = label;
      event.start = event.end = std::chrono::steady_clock::now();
      if (tracer->starts()) tracer->started(event);
    }
    tick();
    auto result = execute(action, inputs, *scratch, stop.get_token(), log_limit);
    monitor.request_stop();
    monitor.join();
    if (timed_out && result.completion == ActionCompletion::cancelled) {
      result.completion = ActionCompletion::timed_out;
      result.err = kind == "RuleWitness" ? "witness timed out" : "compiler capability probe timed out";
    }
    if (tracer) {
      const bool witness_exit = kind == "RuleWitness" && result.completion == ActionCompletion::exited;
      event.outcome = result.exit_status == 0 || witness_exit ? engine::Outcome::executed : engine::Outcome::failed;
      event.end = std::chrono::steady_clock::now();
      if (tracer->values()) {
        if (witness_exit) event.value = std::format("exit status {}", result.exit_status);
        for (const auto& [index, arg] : std::views::enumerate(action.args)) {
          const auto selector = triple_arg_selector(arg.kind);
          if (selector.empty()) continue;
          if (!event.value.empty()) event.value += '\n';
          event.value += std::format("args[{}]: {} = {}", index, selector, arg.value);
        }
      }
      if (event.outcome == engine::Outcome::failed)
        event.errors = timeout.count() == 0 ? result.err : result.err.substr(0, 4096);
      tracer->event(std::move(event));
    }
    tick();
    return result;
  };
  const RunRuleAction run_rule = [&](std::string_view kind, std::string_view label, const Action& action,
                                   std::span<const ActionInput> inputs, Fp128 id,
                                   std::function_ref<bool()> still_current)
      -> std::expected<std::vector<NamedArtifact>, std::string> {
    auto known = session_.run<RuleActionResult>(id, poll, nullptr, tracer);
    if (!known || cancelled()) return std::unexpected("build cancelled");
    if (*known) return std::move(**known);
    if (!still_current()) {
      superseded = true;
      return std::unexpected(std::format("{} '{}' changed before execution", kind, label));
    }
    auto result = run_action(kind, label, action, inputs);
    if (!still_current()) {
      superseded = true;
      return std::unexpected(std::format("{} '{}' changed during execution", kind, label));
    }
    if (result.exit_status != 0)
      return std::unexpected(std::format("{} '{}': {}", kind, label,
          result.err.empty() ? std::format("exit status {}", result.exit_status) : result.err));
    if (!result.err.empty()) report.diagnostics.push_back(std::move(result.err));
    apply([&](engine::Db& db) { db.set<ActionReceipt>(id, result.outputs); });
    return std::move(result.outputs);
  };
  const RunRuleWitness run_witness = [&](std::string_view label, const GeneratorPlan& plan, Fp128 id,
                                         std::function_ref<bool()> still_current)
      -> std::expected<WitnessEvidence, std::string> {
    const auto known = session_.run<RuleWitnessResult>(id, poll, nullptr, tracer);
    if (!known || cancelled()) return std::unexpected("build cancelled");
    if (*known) return **known;
    auto result = run_action("RuleWitness", label, plan.action, plan.inputs, std::chrono::seconds(10),
                             rule_witness_stream_limit);
    if (cancelled()) return std::unexpected("build cancelled");
    if (!still_current()) {
      superseded = true;
      return std::unexpected(std::format("witness '{}' changed during execution", label));
    }
    if (result.completion != ActionCompletion::exited)
      return std::unexpected(std::format("witness '{}': {}", label,
          result.err.empty() ? std::format("did not exit normally (status {})", result.exit_status) : result.err));
    WitnessEvidence evidence{result.exit_status, std::move(result.out), std::move(result.err)};
    apply(AdmitRuleInput<RuleWitnessReceipt>{id, evidence});
    return evidence;
  };
  const std::function<void()> check_revision = [] {};
  const BuildHostTools build_host_tools = [&](const ProjectPlan& plan, std::span<const std::size_t> needed,
                                             std::span<const HostInputOverlay> overlays)
      -> std::optional<BuiltHostTools> {
    std::vector<pipeline::RequestId> requests;
    for (const auto& [i, overlay] : std::views::zip(needed, overlays))
      requests.push_back(register_host_executable(session_, workspace_path, plan.host_executables[i],
                                                  host_target, overlay));
    while (!cancelled()) {
      refresh();
      const auto before = session_.db().revision();
      BuiltHostTools batch;
      batch.images.resize(plan.host_executables.size());
      std::map<std::uint32_t, HostWitnessTool> built;
      std::map<std::string, std::optional<Fp128>> observed;
      bool consistent = true;
      for (const auto& [i, request] : std::views::zip(needed, requests)) {
        auto [tool, first] = built.try_emplace(request.n);
        if (first) {
          auto image = session_.run<HostWitnessImage>(request, poll, nullptr, tracer);
          if (!image) return std::nullopt;
          tool->second = std::move(*image);
          for (const auto& file : tool->second.files) {
            const auto [prior, inserted] = observed.try_emplace(file.path, file.contents);
            if (!inserted && prior->second != file.contents) consistent = false;
          }
        }
        batch.images[i] = tool->second.executable;
        if (batch.error.empty() && !tool->second.error.empty())
          batch.error = std::format("host executable '{}': {}", plan.host_executables[i].name, tool->second.error);
      }
      for (const auto& [path, contents] : observed) batch.files.push_back({path, contents});
      if (consistent && publish(before, check_revision) && current_host_files(batch.files)) return batch;
    }
    return std::nullopt;
  };
  const AdmitRuleValue admit_value = [&](std::string_view producer, std::string_view field, RuleValueRecord record) {
    const auto id = rule_value_key(value_scope, producer, field);
    if (const auto* prior = session_.db().peek<RuleValueReceipt>(id); prior && *prior &&
        (**prior).receipt == record.receipt && (**prior).value == record.value &&
        (**prior).origin == record.origin) return;
    apply(AdmitRuleInput<RuleValueReceipt>{id, std::move(record)});
  };
  const RunRulePackage run_package = [&](const RulePackageRef& package, const RulePackagePlan& plan, Fp128 id,
                                         std::function_ref<bool()> still_current)
      -> std::expected<RulePackageBuild, std::string> {
    auto known = session_.run<RulePackageResult>(id, poll, nullptr, tracer);
    if (!known || cancelled()) return std::unexpected("build cancelled");
    if (*known) return std::move(**known);
    if (plan.candidates.empty())
      return std::unexpected(std::format("rule package '{}': no configured C++ compiler is available", package.target));

    std::string errors;
    for (std::size_t index = 0; index < plan.candidates.size(); ++index) {
      const auto& candidate = plan.candidates[index];
      const auto label = std::format("{} via {}", package.target, candidate.action.tool);
      const auto attempt = fingerprint(std::tie(candidate, plan.inputs));
      auto compiled = run_rule("RulePackage", label, candidate.action, plan.inputs, attempt, still_current);
      if (compiled) {
        if (!still_current()) {
          superseded = true;
          return std::unexpected(std::format("rule package '{}' changed during resolution", package.target));
        }
        RulePackageBuild selected{std::move(*compiled), index};
        apply([&](engine::Db& db) { db.set<RulePackageReceipt>(id, selected); });
        return selected;
      }
      if (cancelled() || superseded) return std::unexpected(compiled.error());
      if (!errors.empty()) errors += '\n';
      errors += compiled.error();
    }
    return std::unexpected(std::move(errors));
  };

  const auto measure_profile = [&](const RulePackageRef& package, const RulePackagePlan& plan,
                                   const RulePackageAction& selected, std::span<const std::string> requested)
      -> std::expected<MeasuredProfile, std::string> {
    if (!selected.cxx)
      return std::unexpected(std::format("package '{}' needs a cxx() tool for compiler probes", package.target));
    std::vector<std::pair<ProbeKey, MeasuredToolchainProbe>> pending;
    const auto measure = [&](ProbeKey key, std::function_ref<MeasuredToolchainProbe()> execute) {
      const auto known = session_.run<ToolchainProbeResult>(key, poll, nullptr, tracer);
      if (known && *known &&
          (!(**known).discovery_stamp ||
           (**known).discovery_stamp == fingerprint_toolchain_probe_inputs((**known).discovered_inputs)))
        return **known;
      auto fresh = execute();
      if (fresh.cacheable && !cancelled()) pending.emplace_back(key, fresh);
      return fresh;
    };
    ToolchainProbeProfile profile{selected.action.tool, {}, {}, selected.native,
                                  plan.probe_flags, selected.action.environment};
    // A staged compiler has a single witness to check. Keep it from using the whole
    // machine's worker pool by default when several build requests share the host.
    if (profile.native && std::ranges::none_of(profile.environment, [](const auto& variable) {
          return variable.first == "YCXX_THREADS";
        })) profile.environment.emplace_back("YCXX_THREADS", "2");
    const auto run_probe = [&](std::string_view kind, std::string_view label, const Action& action,
                               std::span<const ActionInput> inputs) {
      return run_action(kind, label, action, inputs, std::chrono::seconds(selected.native ? 3 : 10));
    };
    auto matrix = measure_toolchain_matrix(cwd, package.target, std::move(profile),
        probe_identity(plan, selected, worker_build_), plan.probe_inputs, executable_, run_probe, measure,
        cancelled, requested, &superseded);
    if (cancelled()) return std::unexpected("build cancelled");
    if (!matrix)
      return std::unexpected(std::format("toolchain profile '{}' could not be measured: {}",
                                         package.target, matrix.error()));

    RuleProbeSnapshot captured{package, probe_snapshot_key(plan, selected, worker_build_),
                               std::move(matrix->inputs), matrix->stamp, std::move(matrix->facts)};
    if (!current_probe_profile(cwd, toolchain, captured)) {
      superseded = true;
      return std::unexpected(std::format("toolchain profile '{}' changed during measurement", package.target));
    }
    if (!pending.empty()) {
      apply(AdmitProbeReceipts{pending});
      for (const auto& [key, record] : pending)
        (void)session_.run<ToolchainProbeResult>(key, poll, nullptr, tracer);
    }
    return MeasuredProfile{std::move(captured), std::move(matrix->observations)};
  };

  const auto saved = read_rule_snapshot(cwd, target, native_triple);
  const ResolveRuleArguments resolve_arguments = [&](const RuleDemand& demand, RuleSnapshot& snapshot)
      -> std::expected<ProjectedRuleArguments, std::string> {
    auto& profiles = snapshot.probes;
    std::map<std::string, std::vector<std::string>, std::less<>> requests;
    std::vector<RuleValueBinding> values;
    ProjectedRuleArguments result;
    const auto bind = [&](const std::string& name, const RuleValueReference& ref, bool structured) {
      const auto producer = std::ranges::find_if(snapshot.expansions, [&](const RuleExpansion& expansion) {
        return expansion.demand.target == ref.target && expansion.demand.value_export == ref.field;
      });
      if (producer == snapshot.expansions.end()) return false;
      const auto fact = std::ranges::find_if(producer->capability_facts, [&](const PlanFact& entry) {
        return entry.kind == "value-export" && entry.name == ref.field;
      });
      if (fact == producer->capability_facts.end()) return false;
      values.push_back({name, rule_value_key(value_scope, ref.target, ref.field), structured});
      result.facts.push_back({structured ? "fact-input" : "capability-input", demand.target, name,
                              fact->value, fact->origin});
      return true;
    };
    for (const auto& binding : demand.capabilities) {
      if (binding.value) {
        const auto& ref = *binding.value;
        if (!bind(binding.name, ref, false))
          return std::unexpected(std::format("missing value '{}.{}'", ref.target, ref.field));
        continue;
      }
      if (!known_toolchain_probe(binding.probe))
        return std::unexpected(std::format("rule '{}': unknown toolchain probe '{}'", demand.target, binding.probe));
      auto& names = requests[binding.package];
      if (!std::ranges::contains(names, binding.probe)) names.push_back(binding.probe);
    }
    for (const auto& binding : demand.facts)
      if (!bind(binding.name, binding.value, true))
        return std::unexpected(std::format("missing value '{}.{}'", binding.value.target, binding.value.field));

    if (!values.empty()) {
      const auto consumer = fingerprint(std::tie(value_scope, demand.target));
      const auto* prior = session_.db().peek<RuleValueBindings>(consumer);
      if (!prior || fingerprint(*prior) != fingerprint(values))
        apply(AdmitRuleInput<RuleValueBindings>{consumer, std::move(values)});
      auto observed = session_.run<RuleValueArguments>(consumer, poll, nullptr, tracer);
      if (!observed || cancelled()) return std::unexpected("build cancelled");
      if (!*observed) return std::unexpected(std::format("rule '{}' has unresolved values", demand.target));
      for (auto& [name, value] : **observed) {
        if (value.fact)
          result.structured.push_back({std::move(name), std::move(*value.fact)});
        else
          result.values.push_back({std::move(name), static_cast<std::uint32_t>(value.support)});
      }
    }
    for (const auto& [spelling, names] : requests) {
      const auto package = capability_package(spelling);
      if (!package) return std::unexpected(package.error());
      const auto plan = plan_rule_package(cwd, *package, rule_sdk_header(), toolchain);
      if (!plan) return std::unexpected(plan.error());
      if (std::ranges::none_of(plan->candidates, [](const auto& candidate) { return candidate.cxx; }))
        return std::unexpected(std::format("package '{}' needs an available cxx() tool for compiler probes", spelling));
      const auto key = package_key(*plan);
      const auto winner = run_package(*package, *plan, key, [&] {
        const auto current = plan_rule_package(cwd, *package, rule_sdk_header(), toolchain);
        return current && package_key(*current) == key;
      });
      if (!winner) return std::unexpected(winner.error());
      const auto& selected = plan->candidates[winner->compiler];
      const bool recorded = std::ranges::any_of(profiles, [&](const auto& profile) {
        return profile.package == *package;
      });
      const bool full = probe_toolchain && !recorded;
      const std::span<const std::string> requested = full ? std::span<const std::string>{} : names;
      auto measured = measure_profile(*package, *plan, selected, requested);
      if (!measured) return std::unexpected(measured.error());
      if (full) profiles.push_back(measured->snapshot);
      else if (!recorded && saved) {
        const auto prior = std::ranges::find_if(saved->probes, [&](const auto& profile) {
          return profile.package == *package && profile.profile == measured->snapshot.profile;
        });
        if (prior != saved->probes.end()) profiles.push_back(*prior);
      }
      for (const auto& binding : demand.capabilities) {
        if (binding.value || binding.package != spelling) continue;
        const auto observed = std::ranges::find_if(measured->observations, [&](const auto& entry) {
          return entry.name == binding.probe;
        });
        if (observed == measured->observations.end())
          return std::unexpected(std::format("rule '{}': missing observation for '{}'", demand.target, binding.probe));
        // A retryable launch failure can still supply typed indeterminate evidence in this request.
        const auto projected = session_.run<CapabilitySupport>(observed->key, poll, nullptr, tracer);
        if (!projected || cancelled()) return std::unexpected("build cancelled");
        const auto support = projected->value_or(observed->evidence.support);
        const auto name = probe_support_name(support);
        result.values.push_back({binding.name, static_cast<std::uint32_t>(support)});
        const auto query = ToolchainProbeResult::describe(observed->key);
        const auto& profile = measured->snapshot.profile;
        auto origin = std::format("{} via {} · {} · profile {:016x}{:016x} · {}", query, selected.action.tool,
                                  spelling, profile.lo, profile.hi, describe_probe_evidence(observed->evidence));
        if (tracer) {
          engine::TraceEvent event;
          event.id = session_.db().reserve_trace_id();
          event.kind = "CapabilityProjection";
          event.key = std::format("{}.{} ← {} via {}", demand.target, binding.name, query, selected.action.tool);
          event.outcome = engine::Outcome::input;
          event.start = event.end = std::chrono::steady_clock::now();
          if (tracer->values()) event.value = std::format("{}\n{}", name, origin);
          tracer->event(std::move(event));
          tick();
        }
        result.facts.push_back({"capability-input", demand.target, binding.name, std::string(name), std::move(origin)});
      }
      measured->snapshot.facts.clear();
      result.profiles.push_back(std::move(measured->snapshot));
    }
    std::ranges::sort(result.values, {}, &RuleCapabilityValue::name);
    std::ranges::sort(result.facts, {}, &PlanFact::name);
    return result;
  };

  std::vector<RulePackageRef> measured;
  const auto receipt = [&](Fp128 id) -> const ActionOutputs* {
    const auto* entry = session_.db().peek<ActionReceipt>(id);
    return entry && *entry ? &**entry : nullptr;
  };
  struct CompletedHostAction {
    Fp128 key;
    ActionResult result;
  };
  std::optional<CompletedHostAction> completed_host_action;
  while (!cancelled()) {
    refresh();
    superseded = false;
    PreparedProject planned;
    planned.native_triple = native_triple;
    planned.manifest = manifest_stamp(cwd);
    measured.clear();
    const UseRuleCompiler use_compiler = [&](const RulePackageRef& package, const RulePackagePlan& plan,
                                             const RulePackageAction& selected) {
      if (!selected.cxx || std::ranges::contains(measured, package) || cancelled() ||
          std::ranges::any_of(planned.rules.probes, [&](const auto& profile) { return profile.package == package; }))
        return;
      measured.push_back(package);
      const auto profile_key = probe_snapshot_key(plan, selected, worker_build_);
      if (!probe_toolchain) {
        if (saved)
          if (const auto found = std::ranges::find_if(saved->probes, [&](const auto& prior) {
                return prior.package == package && prior.profile == profile_key;
              }); found != saved->probes.end())
            planned.rules.probes.push_back(*found);
        return;
      }
      if (!current_rules(cwd, toolchain, planned)) {
        superseded = true;
        return;
      }
      auto measured = measure_profile(package, plan, selected, {});
      if (measured) planned.rules.probes.push_back(std::move(measured->snapshot));
      else if (!cancelled() && !superseded) report.diagnostics.push_back(std::move(measured.error()));
    };
    auto prepared = prepare_project(cwd, target, toolchain, run_rule, run_package, run_witness, build_host_tools,
                                    receipt, admit_value,
                                    use_compiler, resolve_arguments, planned, watch, superseded);
    if (cancelled()) return fail("build cancelled");
    if (superseded || !current_rules(cwd, toolchain, planned)) continue;
    write_rule_snapshot(cwd, target, planned.rules, native_triple);
    if (watch) {
      report.watch_stamp =
          prepared ? watch_rules(cwd, target, toolchain, planned.rules, native_triple).stamp : planned.attempt_watch;
      auto snapshot = std::make_shared<RuleSnapshot>(planned.rules);
      std::lock_guard lock(watch_mutex_);
      watch_snapshots_.insert_or_assign(WatchKey{cwd, target, tool_path, native_triple}, std::move(snapshot));
    }
    if (!prepared) return fail(prepared.error());
    const ProjectPlan& plan = planned.plan;
    const Fp128 plan_id = fingerprint(plan);
    const bool has_generated_host_inputs = std::ranges::any_of(plan.host_executables, [](const auto& executable) {
      return !executable.generated_inputs.empty();
    });
    std::vector<std::optional<Fp128>> host_overlay_keys;
    if (const auto* prior = session_.db().peek<WorkspacePlan>(key); !prior || fingerprint(prior->resolved) != plan_id) {
      std::vector<std::size_t> needed;
      needed.append_range(std::views::iota(std::size_t{0}, plan.host_executables.size()));
      const auto current_recipe = [&] {
        const auto observed = observe_project(cwd, target, toolchain, planned);
        return observed && fingerprint(*observed) == plan_id;
      };
      const auto current_host_input_action = [&](std::size_t index, Fp128 id) {
        const auto observed = observe_project(cwd, target, toolchain, planned);
        if (!observed) return false;
        if (fingerprint(*observed) == plan_id) return true;
        const auto action = std::ranges::find(observed->generators, plan.generators[index].name, &GeneratorPlan::name);
        if (action == observed->generators.end()) return false;
        std::vector<std::size_t> observed_hosts;
        observed_hosts.append_range(std::views::iota(std::size_t{0}, observed->host_executables.size()));
        const auto closure = host_input_closure(*observed, observed_hosts);
        if (!closure.hosts.empty()) return false;
        const auto at = static_cast<std::size_t>(action - observed->generators.begin());
        return current_producer_key(*observed, closure, at, {}, receipt) == id;
      };
      auto overlays = stage_host_inputs(cwd, plan, needed, run_rule, current_host_input_action, current_recipe, [&] {
        superseded = true;
        return std::unexpected(std::string("generated host inputs changed during preparation"));
      });
      if (!overlays) {
        if (superseded) continue;
        return fail(std::move(overlays.error()));
      }
      std::vector<pipeline::RequestId> host_requests;
      for (const auto& [executable, overlay] : std::views::zip(plan.host_executables, *overlays)) {
        host_requests.push_back(register_host_executable(session_, workspace_path, executable, host_target, overlay));
        host_overlay_keys.push_back(overlay ? std::optional{overlay->fingerprint()} : std::nullopt);
      }
      apply([&](engine::Db& db) {
        db.set<WorkspacePlan>(key, Planned{native_workspace, plan, std::move(host_requests), host_overlay_keys});
      });
    } else {
      host_overlay_keys = prior->host_overlay_keys;
    }

    for (;;) {
      const auto before = session_.db().revision();
      auto demand = session_.run<WorkspaceBuild>(key, poll, nullptr, tracer);
      tick();
      if (!demand) return fail("build cancelled");

      const auto output_at = [&](std::size_t index) -> const ActionOutputs* {
        const auto& id = demand->actions.keys[index];
        return id ? receipt(*id) : nullptr;
      };
      if (!demand->actions.pending.empty()) {
        std::set<Fp128> attempted;
        const auto host_plan = fingerprint(plan.host_executables);
        const auto current_action = [&](const GeneratorPlan& generator, Fp128 id) {
          auto revision = before;
          std::span<const Artifact> images = demand->host_images;
          std::optional<Demand> rechecked;
          while (!cancelled()) {
            const auto observed = observe_project(cwd, target, toolchain, planned);
            if (!observed || fingerprint(observed->host_executables) != host_plan) return false;
            const auto current = std::ranges::find(observed->generators, generator.name, &GeneratorPlan::name);
            if (current == observed->generators.end()) return false;
            if (!current->host_dependent || publish(revision, [] {})) {
              const auto at = static_cast<std::size_t>(current - observed->generators.begin());
              const auto actions = resolve_actions(*observed, receipt, images);
              if (actions.keys[at] != id) return false;
              if (!current->host_dependent || !has_generated_host_inputs || fingerprint(*observed) == plan_id)
                return true;
              // Only the reachable host overlays affect this action. Check
              // bytes from known receipts without starting a producer.
              const auto hosts = producer_closure(*observed, current->artifact_inputs, current->host_inputs).hosts;
              const auto known_output = [&](std::size_t producer) -> const ActionOutputs* {
                const auto& key = actions.keys[producer];
                return key ? receipt(*key) : nullptr;
              };
              return std::ranges::all_of(hosts, [&](std::size_t host) {
                if (!host_overlay_keys[host]) return true;
                const auto overlay = host_input_overlay(cwd, *observed, host, known_output);
                return overlay && overlay->fingerprint() == *host_overlay_keys[host];
              });
            }
            // The plan only describes host source paths. Re-demand the images so an
            // unrelated edit, or one that leaves the staged bytes equal, keeps the receipt.
            revision = session_.db().revision();
            rechecked = session_.run<WorkspaceBuild>(key, poll, nullptr, tracer);
            if (!rechecked || rechecked->host_images.size() != plan.host_executables.size()) return false;
            images = rechecked->host_images;
          }
          return false;
        };
        for (const auto index : demand->actions.pending) {
          if (cancelled()) return fail("build cancelled");
          const auto& generator = plan.generators[index];
          const auto id = *demand->actions.keys[index];
          // Two targets in a plan can describe the same execution.
          if (!attempted.insert(id).second) continue;
          std::vector<ActionInput> staged;
          std::span<const ActionInput> inputs = generator.inputs;
          if (!generator.artifact_inputs.empty() || !generator.host_inputs.empty()) {
            staged = generator.inputs;
            if (!append_generator_inputs(generator, output_at, demand->host_images, staged)) break;
            inputs = staged;
          }
          if (generator.host_dependent &&
              (has_generated_host_inputs ? !current_action(generator, id) : !publish(before, [] {}))) break;
          const bool completed = completed_host_action && completed_host_action->key == id;
          auto result = completed ? std::move(completed_host_action->result)
                                  : run_action("ExternalAction", generator.name, generator.action, inputs);
          if (completed) completed_host_action.reset();
          if (!current_action(generator, id)) {
            // The new host input recipe may need to run first. Retain this
            // effect until the next plan can prove its exact staged inputs.
            if (has_generated_host_inputs && generator.host_dependent)
              completed_host_action.emplace(id, std::move(result));
            break;
          }
          if (result.exit_status != 0) {
            report.status = result.exit_status;
            report.diagnostics.push_back(std::format("generator '{}': {}", generator.name,
                result.err.empty() ? std::format("exit status {}", result.exit_status) : result.err));
            return report;
          }
          if (!result.err.empty()) report.diagnostics.push_back(std::move(result.err));
          apply([&](engine::Db& db) { db.set<ActionReceipt>(id, std::move(result.outputs)); });
        }
        break;  // Reobserve disk inputs before admitting a native build.
      }

      if (demand->prepare) {
        const auto request = register_native(session_, workspace_path, target, native_target, plan, output_at,
                                             native_triple);
        std::map<std::string, std::string> generated;
        for_each_native_output(plan, cwd, native_triple, [&](ArtifactOutputPlan output, std::string path) {
          const auto& bytes = (*output_at(output.producer))[output.output].contents.bytes;
          generated.emplace(std::move(path), std::string(bytes.begin(), bytes.end()));
        });
        auto& previous = generated_paths_[native_workspace_path];
        apply([&](engine::Db& db) {
          for (const auto& path : previous)
            if (!generated.contains(path)) session_.set_generated(path, std::nullopt);
          for (const auto& [path, bytes] : generated) session_.set_generated(path, bytes);
          db.set<PreparedBuild>(native_workspace, Prepared{key, demand->native_plan, request});
        });
        previous.clear();
        for (const auto& [path, bytes] : generated) previous.insert(path);
        continue;
      }

      if (cancelled()) return fail("build cancelled");
      const auto observed = observe_project(cwd, target, toolchain, planned);
      if (!observed || fingerprint(*observed) != plan_id) break;
      if (demand->image.empty()) {
        if (!publish(before, [] {})) break;
        report.status = 1;
        report.diagnostics.append_range(std::move(demand->errors));
        if (report.diagnostics.empty()) report.diagnostics.push_back("build produced no executable");
        return report;
      }
      const auto started = std::chrono::steady_clock::now();
      std::expected<bool, std::string> publication = false;
      if (!publish(before, [&] {
            std::vector<RuntimeAssetOutput> assets;
            for (const auto& asset : plan.runtime_assets) {
              const auto& name = plan.generators[asset.producer].action.outputs[asset.output].filename;
              assets.push_back({name, (*output_at(asset.producer))[asset.output].contents});
            }
            auto staged = PreparedPublication::stage(cwd, target, assets, demand->image, native_triple);
            publication = staged ? std::move(*staged).commit() : std::unexpected(std::move(staged.error()));
          })) break;
      if (tracer && (!publication || *publication)) {
        engine::TraceEvent event;
        event.id = session_.db().reserve_trace_id();
        event.kind = "ProjectPublication";
        event.key = target;
        event.start = started;
        event.end = std::chrono::steady_clock::now();
        event.outcome = publication ? engine::Outcome::executed : engine::Outcome::failed;
        if (!publication) event.errors = publication.error();
        else if (tracer->values()) event.value = std::format("published {} runtime assets", plan.runtime_assets.size());
        tracer->event(std::move(event));
      }
      if (!publication) return fail(std::move(publication.error()));
      report.output = native(output);
      return report;
    }
  }
  return fail("build cancelled");
}

}  // namespace ycxx::build
