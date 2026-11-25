// One resident workspace request. External tools run between query roots, and their successful
// results reach the graph as inputs at quiescence (#1255).
module;
#include <stdlib.h>
#include <unistd.h>

export module ycxx.build.project;

import std;
import ycxx.ast;
import ycxx.build.action;
import ycxx.build.inspection_model;
import ycxx.build.plan;
import ycxx.build.rule_protocol;
import ycxx.build.rule_snapshot;
import ycxx.build.rule_worker;
import ycxx.build.toolchain_adapter;
import ycxx.build.toolchain_inputs;
import ycxx.build.toolchain_matrix;
import ycxx.engine;
import ycxx.hash;
import ycxx.lsp;
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
    const std::filesystem::path& cwd, std::string_view target, const RuleToolchain& toolchain);

class Coordinator {
 public:
  Coordinator(pipeline::Session& session, std::string executable, std::uint64_t worker_build);
  ProjectWatchState watch_state(const std::filesystem::path& cwd, const std::string& target,
                                const std::optional<std::string>& tool_path);

  ProjectReport run(const std::filesystem::path& cwd, const std::string& target,
                    const std::optional<std::string>& tool_path, bool watch, bool probe_toolchain,
                    const std::function<void()>& refresh, const std::function<bool()>& cancelled,
                    const std::function<void(const std::function<void(engine::Db&)>&)>& apply,
                    const std::function<bool(engine::Revision, const std::function<void()>&)>& publish,
                    const std::function<void()>& tick, engine::Tracer* tracer = nullptr);

 private:
  pipeline::Session& session_;
  std::string executable_;
  std::uint64_t worker_build_;
  std::mutex mutex_;
  std::map<std::string, std::set<std::string>> generated_paths_;
  using WatchKey = std::tuple<std::filesystem::path, std::string, std::optional<std::string>>;
  std::mutex watch_mutex_;
  std::map<WatchKey, std::shared_ptr<const RuleSnapshot>> watch_snapshots_;
};

}  // namespace ycxx::build

namespace ycxx::build {

namespace fs = std::filesystem;
using Apply = std::function<void(const std::function<void(engine::Db&)>&)>;

struct Planned {
  Fp128 workspace;
  ProjectPlan resolved;
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

struct RuleValueRecord {
  ProbeSupport support;
  Fp128 receipt;
  std::string origin;
};
// The stable export cell depends on the whole receipt and returns only its support state.
struct [[=engine::input{engine::Durability::low}]] RuleValueReceipt {
  using Key = Fp128;
  using Value = std::optional<RuleValueRecord>;
  static std::string show(const Value& value) {
    return value ? std::format("{}\n{}", probe_support_name(value->support), value->origin)
                 : "Observation needed";
  }
};
struct RuleValue {
  using Key = RuleValueReceipt::Key;
  using Value = std::optional<ProbeSupport>;
  static std::string show(const Value& value) {
    return value ? std::string(probe_support_name(*value)) : "Observation needed";
  }
  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    const auto& receipt = cx.fetch<RuleValueReceipt>(key);
    if (receipt) co_return receipt->support;
    co_return std::nullopt;
  }
};

struct RuleValueBinding {
  std::string name;
  Fp128 value;
};
struct [[=engine::input{engine::Durability::low}]] RuleValueBindings {
  using Key = Fp128;
  using Value = std::vector<RuleValueBinding>;
};
struct RuleValueArguments {
  using Key = RuleValueBindings::Key;
  using Value = std::optional<std::vector<RuleCapabilityValue>>;
  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    std::vector<RuleCapabilityValue> result;
    for (const auto& binding : cx.fetch<RuleValueBindings>(key)) {
      const auto& state = co_await cx.fetch<RuleValue>(binding.value);
      if (!state) co_return std::nullopt;
      result.push_back({binding.name, static_cast<std::uint32_t>(*state)});
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

Fp128 action_key(const GeneratorPlan& generator) {
  return fingerprint(std::tie(generator.action, generator.inputs, generator.tool_fingerprint));
}

struct Demand {
  std::vector<std::size_t> pending;
  bool prepare = false;
  std::vector<std::uint8_t> image;
  std::vector<std::string> errors;
};

struct WorkspaceBuild {
  using Key = WorkspacePlan::Key;
  using Value = Demand;

  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    const auto& snapshot = cx.fetch<WorkspacePlan>(key);
    const auto& plan = snapshot.resolved;
    Demand demand;
    for (std::size_t i = 0; i < plan.generators.size(); ++i)
      if (!cx.fetch<ActionReceipt>(action_key(plan.generators[i]))) demand.pending.push_back(i);
    if (!demand.pending.empty()) co_return demand;
    const auto& prepared = cx.fetch<PreparedBuild>(snapshot.workspace);
    if (!prepared || prepared->output != key || prepared->plan != fingerprint(plan)) {
      demand.prepare = true;
      co_return demand;
    }
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

std::string native(const fs::path& path) { return path.generic_native_encoded_string(); }

pipeline::RequestId register_native(pipeline::Session& session, const std::string& cwd, const std::string& target,
                                    const ProjectPlan& plan) {
  pipeline::BuildKey build;
  for (const auto& source : plan.sources) {
    pp::Config config;
    config.view = pp::View::build;
    config.target = pp::native_target;
    config.freestanding = config.target == &ast::x86_64_linux;
    config.angle_dirs = source.includes;
    config.system_from = config.angle_dirs.size();
    const std::string object = source.path + ".o";
    build.units.push_back(session.unit({source.path, cwd, std::move(config), object}));
  }
  build.program = session.program({.tus = build.units, .objects = {}, .identifier = target});
  return session.request(std::move(build));
}

std::expected<fs::path, std::string> build_directory(const fs::path& workspace, std::string_view name) {
  fs::path path = workspace;
  for (const auto part : {std::string_view(".ycxx"), name}) {
    path /= part;
    std::error_code ec;
    auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found) {
      fs::create_directory(path, ec);
      if (!ec) status = fs::symlink_status(path, ec);
    }
    if (ec || !fs::is_directory(status))
      return std::unexpected(std::format("expected a build directory at '{}'", native(path)));
  }
  return path;
}

struct Scratch {
  fs::path path;
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::optional<std::string> install(const fs::path& output, std::span<const std::uint8_t> image) {
  std::error_code ec;
  const auto existing = fs::symlink_status(output, ec);
  if (fs::is_regular_file(existing) && (existing.permissions() & fs::perms::owner_exec) != fs::perms::none) {
    std::ifstream file(output, std::ios::binary);
    if (file && std::ranges::equal(std::span(reinterpret_cast<const char*>(image.data()), image.size()),
                                   std::ranges::subrange(std::istreambuf_iterator<char>(file),
                                                         std::istreambuf_iterator<char>()))) return std::nullopt;
  }
  auto path = native(output.parent_path() / ".publish-XXXXXX");
  if (!::mkdtemp(path.data())) return "cannot create temporary build output directory";
  const Scratch scratch{fs::path(path)};
  const fs::path temporary = scratch.path / "image";
  std::ofstream file(temporary, std::ios::binary | std::ios::noreplace);
  file.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
  file.close();
  if (!file) return "cannot write executable";
  using enum fs::perms;
  fs::permissions(temporary, owner_read | owner_write | owner_exec | group_read | group_exec | others_read | others_exec,
                  fs::perm_options::replace, ec);
  if (ec) return std::format("cannot set executable permissions: {}", ec.message());
  fs::rename(temporary, output, ec);
  if (ec) return std::format("cannot publish executable: {}", ec.message());
  return std::nullopt;
}

struct RuleInvocation {
  RuleWorkerRequest request;
  std::vector<ActionInput> reads;
};

Artifact text_artifact(std::string_view text) {
  return {std::vector<std::uint8_t>(text.begin(), text.end()), false};
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
    auto staged = std::format("reads/{}", invocation.reads.size());
    invocation.request.reads.push_back({path, std::format("inputs/{}", staged)});
    invocation.reads.push_back({staged, staged, text_artifact(bytes)});
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

struct PreparedProject {
  std::optional<Fp128> manifest;
  ProjectPlan plan;
  RuleSnapshot rules;
  std::optional<Fp128> attempt_watch;
};

struct ProjectedCapabilities {
  std::vector<RuleCapabilityValue> values;
  std::vector<RuleProbeSnapshot> profiles;
  std::vector<PlanFact> facts;
};

struct MeasuredProfile {
  RuleProbeSnapshot snapshot;
  std::vector<ToolchainObservation> observations;
};

ProjectWatchState watch_rules(const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain,
                              const RuleSnapshot& snapshot);

using RunRuleAction = std::function<std::expected<std::vector<NamedArtifact>, std::string>(
    std::string_view, std::string_view, const Action&, std::span<const ActionInput>, Fp128,
    std::function_ref<bool()>)>;
using RunRulePackage = std::function<std::expected<RulePackageBuild, std::string>(
    const RulePackageRef&, const RulePackagePlan&, Fp128, std::function_ref<bool()>)>;
using RunRuleWitness = std::function<std::expected<WitnessEvidence, std::string>(
    std::string_view, const GeneratorPlan&, Fp128, std::function_ref<bool()>)>;
using AdmitRuleValue = std::function<void(std::string_view, std::string_view, RuleValueRecord)>;
using UseRuleCompiler = std::function<void(const RulePackageRef&, const RulePackagePlan&, const RulePackageAction&)>;
using ResolveCapabilities = std::function<std::expected<ProjectedCapabilities, std::string>(
    const RuleDemand&, RuleSnapshot&)>;

bool current_rule(const fs::path& cwd, const RuleToolchain& toolchain, const RuleObservation& record,
                  const RuleExpansion& expansion);

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
    const AdmitRuleValue& admit_value, const UseRuleCompiler& use_compiler,
    const ResolveCapabilities& resolve_capabilities, PreparedProject& prepared, bool watch, bool& superseded) {
  for (;;) {
    std::optional<Fp128> definition;
    auto attempt = plan_with_rules(cwd, target, prepared.rules.expansions, &definition);
    // All expansions in one attempt must belong to a single workspace definition.
    if (definition != prepared.manifest) {
      superseded = true;
      return std::unexpected("build.ycxx changed during rule resolution");
    }
    if (!attempt) return std::unexpected(attempt.error());
    if (auto* plan = std::get_if<ProjectPlan>(&*attempt)) {
      prepared.plan = std::move(*plan);
      return {};
    }
    auto& demand = std::get<RuleDemand>(*attempt);
    if (std::ranges::any_of(prepared.rules.expansions, [&](const auto& entry) { return entry.demand == demand; }))
      return std::unexpected(std::format("rule '{}' did not resolve its declaration", demand.target));
    if (toolchain.executable.empty()) return std::unexpected("cannot locate the rule worker executable");
    if (watch) prepared.attempt_watch = watch_rules(cwd, target, toolchain, prepared.rules).stamp;

    auto package = plan_rule_package(cwd, demand.package, rule_sdk_header(), toolchain);
    if (!package) return std::unexpected(package.error());
    const auto library_key = package_key(*package);
    const auto current_package = [&] {
      const auto current = plan_rule_package(cwd, demand.package, rule_sdk_header(), toolchain);
      return current && package_key(*current) == library_key;
    };
    auto built = run_package(demand.package, *package, library_key, current_package);
    if (!built) return std::unexpected(built.error());
    use_compiler(demand.package, *package, package->candidates[built->compiler]);

    auto capabilities = resolve_capabilities(demand, prepared.rules);
    if (!capabilities) return std::unexpected(capabilities.error());
    if (watch && !capabilities->profiles.empty()) {
      prepared.rules.pending_projections = capabilities->profiles;
      prepared.attempt_watch = watch_rules(cwd, target, toolchain, prepared.rules).stamp;
    }
    auto observation = rule_invocation(cwd, demand);
    if (!observation) return std::unexpected(observation.error());
    // Profile identity is revalidated separately. Only the support states enter the worker's key.
    const auto invocation_key = observation_key(*observation, toolchain.executable_build);
    if (!capabilities->values.empty()) observation->request.capabilities = std::move(capabilities->values);
    const bool consumes_values = std::ranges::any_of(demand.capabilities, [](const auto& binding) {
      return binding.value.has_value();
    });
    const auto current_invocation = [&] {
      const auto current = rule_invocation(cwd, demand);
      if (manifest_stamp(cwd) != prepared.manifest ||
          !current || observation_key(*current, toolchain.executable_build) != invocation_key ||
          !current_package() || !std::ranges::all_of(capabilities->profiles, [&](const auto& profile) {
            return current_projection(cwd, toolchain, profile);
          })) return false;
      if (consumes_values)
        for (std::size_t i = 0; i < prepared.rules.observations.size(); ++i)
          if (!current_rule(cwd, toolchain, prepared.rules.observations[i], prepared.rules.expansions[i]))
            return false;
      return true;
    };
    const auto& library = built->outputs.front().contents;
    auto lowered = invoke_rule_worker(run, toolchain, "RuleWorker", demand.target, library,
                                      *observation, current_invocation);
    if (!lowered) return std::unexpected(std::move(lowered.error()));
    if (consumes_values && !current_invocation()) {
      superseded = true;
      return std::unexpected(std::format("value inputs for '{}' changed during rule resolution", demand.target));
    }
    RuleExpansion expansion{demand, std::move(*lowered), std::move(capabilities->facts)};
    std::optional<Fp128> witness_key;
    if (demand.value_export) {
      const auto& field = *demand.value_export;
      const auto label = std::format("{}.{}", demand.target, field);
      if (watch) {
        prepared.rules.pending_witness = expansion;
        prepared.attempt_watch = watch_rules(cwd, target, toolchain, prepared.rules).stamp;
      }
      auto witness = plan_rule_witness(cwd, expansion);
      if (!witness) return std::unexpected(std::move(witness.error()));
      witness_key = action_key(*witness);
      const auto current_witness = [&] {
        if (!current_invocation()) return false;
        const auto current = plan_rule_witness(cwd, expansion);
        return current && action_key(*current) == *witness_key;
      };
      auto evidence = run_witness(label, *witness, *witness_key, current_witness);
      if (!evidence) return std::unexpected(std::move(evidence.error()));
      observation->request.witness = RuleWitnessReport{
          field, evidence->exit_status, std::move(evidence->out), std::move(evidence->err)};
      const auto receipt = fingerprint(std::tie(library_key, invocation_key, *witness_key, observation->request));
      auto classified = invoke_rule_worker(run, toolchain, "RuleClassifier", label, library,
                                           std::move(*observation), current_witness);
      if (!classified) return std::unexpected(std::move(classified.error()));
      if (!current_witness()) {
        superseded = true;
        return std::unexpected(std::format("witness '{}' changed during classification", label));
      }
      auto parsed = lsp::parse(*classified);
      if (!parsed) return std::unexpected(std::format("invalid classification for '{}'", label));
      const auto reply = lsp::from_json<RuleCapabilityValue>(*parsed);
      if (!reply || reply->name != field || reply->support > std::to_underlying(ProbeSupport::indeterminate))
        return std::unexpected(std::format("invalid classification for '{}'", label));
      const auto support = static_cast<ProbeSupport>(reply->support);
      const auto origin = std::format("{} via {} · {} · witness exit {} · receipt {:016x}{:016x}", label,
          witness->action.tool, demand.package.target, evidence->exit_status, receipt.lo, receipt.hi);
      expansion.capability_facts.push_back(
          {"value-export", demand.target, field, std::string(probe_support_name(support)), origin});
      admit_value(demand.target, field, RuleValueRecord{support, receipt, origin});
      prepared.rules.pending_witness.reset();
    }
    prepared.rules.observations.push_back(
        {std::move(demand), library_key, invocation_key, std::move(capabilities->profiles), witness_key});
    prepared.rules.expansions.push_back(std::move(expansion));
    prepared.rules.pending_projections.clear();
  }
}

bool current_rule(const fs::path& cwd, const RuleToolchain& toolchain, const RuleObservation& record,
                  const RuleExpansion& expansion) {
  const auto package = plan_rule_package(cwd, record.demand.package, rule_sdk_header(), toolchain);
  if (!package || package_key(*package) != record.package) return false;
  const auto observed = rule_invocation(cwd, record.demand);
  if (!observed || observation_key(*observed, toolchain.executable_build) != record.invocation ||
      !std::ranges::all_of(record.projections, [&](const auto& profile) {
        return current_projection(cwd, toolchain, profile);
      })) return false;
  if (!record.witness) return true;
  const auto witness = plan_rule_witness(cwd, expansion);
  return witness && action_key(*witness) == *record.witness;
}

bool current_rules(const fs::path& cwd, const RuleToolchain& toolchain, const PreparedProject& prepared) {
  if (manifest_stamp(cwd) != prepared.manifest) return false;
  for (std::size_t i = 0; i < prepared.rules.observations.size(); ++i)
    if (!current_rule(cwd, toolchain, prepared.rules.observations[i], prepared.rules.expansions[i]))
      return false;
  return true;
}

std::optional<ProjectPlan> observe_project(
    const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain, const PreparedProject& prepared) {
  if (!current_rules(cwd, toolchain, prepared)) return std::nullopt;
  std::optional<Fp128> definition;
  auto attempt = plan_with_rules(cwd, target, prepared.rules.expansions, &definition);
  if (!attempt || definition != prepared.manifest) return std::nullopt;
  if (auto* plan = std::get_if<ProjectPlan>(&*attempt)) return std::move(*plan);
  return std::nullopt;
}

ProjectWatchState watch_rules(const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain,
                              const RuleSnapshot& snapshot) {
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
  const auto observe_witness = [&](const RuleExpansion& expansion) -> std::optional<Fp128> {
    std::vector<std::string> witnessed_paths;
    const auto plan = plan_rule_witness(cwd, expansion, &witnessed_paths);
    paths.append_range(std::move(witnessed_paths));
    observations.push_back(plan ? action_key(*plan) : fingerprint128(plan.error()));
    if (plan) return observations.back();
    return std::nullopt;
  };
  for (std::size_t i = 0; i < snapshot.observations.size(); ++i) {
    const auto& record = snapshot.observations[i];
    const auto& expansion = snapshot.expansions[i];
    const auto state = observe(record.demand);
    bool profiles_current = true;
    for (const auto& probe : record.projections)
      if (!observe_probe(probe)) profiles_current = false;
    const bool witness_current = !record.witness || observe_witness(expansion) == record.witness;
    if (profiles_current && witness_current && state == std::pair{record.package, record.invocation})
      current.push_back(expansion);
  }
  for (const auto& probe : snapshot.probes) (void)observe_probe(probe);
  for (const auto& probe : snapshot.pending_projections) (void)observe_probe(probe);
  if (snapshot.pending_witness) (void)observe_witness(*snapshot.pending_witness);
  auto watch = watch_project(cwd, target, current);
  if (watch.pending && std::ranges::none_of(snapshot.observations, [&](const auto& record) {
        return record.demand == *watch.pending;
      })) (void)observe(*watch.pending);
  watch.stamp = fingerprint(std::tie(watch.stamp, observations));
  watch.paths.append_range(std::move(paths));
  std::ranges::sort(watch.paths);
  auto duplicates = std::ranges::unique(watch.paths);
  watch.paths.erase(duplicates.begin(), duplicates.end());
  return watch;
}

}  // namespace

std::expected<PlanInspection, std::string> inspect_built_project(
    const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain) {
  auto snapshot = read_rule_snapshot(cwd, target);
  std::vector<RuleExpansion> current;
  if (snapshot) {
    for (std::size_t i = 0; i < snapshot->observations.size(); ++i) {
      if (current_rule(cwd, toolchain, snapshot->observations[i], snapshot->expansions[i]))
        current.push_back(std::move(snapshot->expansions[i]));
    }
  }
  auto inspection = inspect_project(cwd, target, current);
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
                                            const std::optional<std::string>& tool_path) {
  std::shared_ptr<const RuleSnapshot> snapshot;
  {
    std::lock_guard lock(watch_mutex_);
    const auto entry = watch_snapshots_.find({cwd, target, tool_path});
    if (entry != watch_snapshots_.end()) snapshot = entry->second;
  }
  static const RuleSnapshot empty;
  return watch_rules(cwd, target, {executable_, worker_build_, tool_path}, snapshot ? *snapshot : empty);
}

ProjectReport Coordinator::run(const fs::path& cwd, const std::string& target,
                               const std::optional<std::string>& tool_path, bool watch, bool probe_toolchain,
                               const std::function<void()>& refresh, const std::function<bool()>& cancelled,
                               const Apply& apply,
                               const std::function<bool(engine::Revision, const std::function<void()>&)>& publish,
                               const std::function<void()>& tick, engine::Tracer* tracer) {
  std::lock_guard lock(mutex_);
  ProjectReport report;
  const auto output = cwd / ".ycxx" / "out" / target;
  const auto key = fingerprint128(native(output));
  const std::string workspace_path = native(cwd);
  const auto workspace = fingerprint128(workspace_path);
  const RuleToolchain toolchain{executable_, worker_build_, tool_path};
  bool superseded = false;
  const auto fail = [&](std::string diagnostic) {
    report.status = 1;
    report.diagnostics.push_back(std::move(diagnostic));
    return report;
  };

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
    std::inplace_stop_source stop;
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
      if (witness_exit && tracer->values()) event.value = std::format("exit status {}", result.exit_status);
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
  const AdmitRuleValue admit_value = [&](std::string_view producer, std::string_view field, RuleValueRecord record) {
    const auto id = rule_value_key(workspace, producer, field);
    if (const auto* prior = session_.db().peek<RuleValueReceipt>(id); prior && *prior &&
        (**prior).receipt == record.receipt && (**prior).support == record.support &&
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

  const auto* native_target = pp::native_target;
  if (!native_target) return fail("project building is unavailable on this host");
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

  const auto saved = read_rule_snapshot(cwd, target);
  const ResolveCapabilities resolve_capabilities = [&](const RuleDemand& demand, RuleSnapshot& snapshot)
      -> std::expected<ProjectedCapabilities, std::string> {
    auto& profiles = snapshot.probes;
    std::map<std::string, std::vector<std::string>, std::less<>> requests;
    std::vector<RuleValueBinding> values;
    ProjectedCapabilities result;
    for (const auto& binding : demand.capabilities) {
      if (binding.value) {
        const auto& ref = *binding.value;
        const auto producer = std::ranges::find_if(snapshot.expansions, [&](const RuleExpansion& expansion) {
          return expansion.demand.target == ref.target && expansion.demand.value_export == ref.field;
        });
        if (producer == snapshot.expansions.end())
          return std::unexpected(std::format("missing value '{}.{}'", ref.target, ref.field));
        const auto fact = std::ranges::find_if(producer->capability_facts, [&](const PlanFact& entry) {
          return entry.kind == "value-export" && entry.name == ref.field;
        });
        if (fact == producer->capability_facts.end())
          return std::unexpected(std::format("missing value '{}.{}'", ref.target, ref.field));
        values.push_back({binding.name, rule_value_key(workspace, ref.target, ref.field)});
        result.facts.push_back({"capability-input", demand.target, binding.name, fact->value, fact->origin});
        continue;
      }
      if (!known_toolchain_probe(binding.probe))
        return std::unexpected(std::format("rule '{}': unknown toolchain probe '{}'", demand.target, binding.probe));
      auto& names = requests[binding.package];
      if (!std::ranges::contains(names, binding.probe)) names.push_back(binding.probe);
    }

    if (!values.empty()) {
      const auto consumer = fingerprint(std::tie(workspace, demand.target));
      const auto* prior = session_.db().peek<RuleValueBindings>(consumer);
      if (!prior || fingerprint(*prior) != fingerprint(values))
        apply(AdmitRuleInput<RuleValueBindings>{consumer, std::move(values)});
      auto observed = session_.run<RuleValueArguments>(consumer, poll, nullptr, tracer);
      if (!observed || cancelled()) return std::unexpected("build cancelled");
      if (!*observed) return std::unexpected(std::format("rule '{}' has unresolved values", demand.target));
      result.values = std::move(**observed);
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
  while (!cancelled()) {
    refresh();
    superseded = false;
    PreparedProject planned;
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
      auto measured = measure_profile(package, plan, selected, {});
      if (measured) planned.rules.probes.push_back(std::move(measured->snapshot));
      else if (!cancelled() && !superseded) report.diagnostics.push_back(std::move(measured.error()));
    };
    auto prepared = prepare_project(cwd, target, toolchain, run_rule, run_package, run_witness, admit_value,
                                    use_compiler, resolve_capabilities, planned, watch, superseded);
    if (cancelled()) return fail("build cancelled");
    if (superseded || !current_rules(cwd, toolchain, planned)) continue;
    write_rule_snapshot(cwd, target, planned.rules);
    if (watch) {
      report.watch_stamp = prepared ? watch_rules(cwd, target, toolchain, planned.rules).stamp : planned.attempt_watch;
      auto snapshot = std::make_shared<RuleSnapshot>(planned.rules);
      std::lock_guard lock(watch_mutex_);
      watch_snapshots_.insert_or_assign(WatchKey{cwd, target, tool_path}, std::move(snapshot));
    }
    if (!prepared) return fail(prepared.error());
    const ProjectPlan& plan = planned.plan;
    const Fp128 plan_id = fingerprint(plan);
    if (const auto* prior = session_.db().peek<WorkspacePlan>(key); !prior || fingerprint(prior->resolved) != plan_id)
      apply([&](engine::Db& db) { db.set<WorkspacePlan>(key, Planned{workspace, plan}); });

    for (;;) {
      const auto before = session_.db().revision();
      auto demand = session_.run<WorkspaceBuild>(key, poll, nullptr, tracer);
      tick();
      if (!demand) return fail("build cancelled");

      if (!demand->pending.empty()) {
        std::set<Fp128> attempted;
        for (const auto index : demand->pending) {
          if (cancelled()) return fail("build cancelled");
          const auto& generator = plan.generators[index];
          const auto id = action_key(generator);
          // Two targets in a plan can describe the same execution.
          if (!attempted.insert(id).second) continue;
          auto result = run_action("ExternalAction", generator.name, generator.action, generator.inputs);
          const auto observed = observe_project(cwd, target, toolchain, planned);
          if (!observed || std::ranges::none_of(observed->generators, [&](const auto& current) {
                return current.name == generator.name && action_key(current) == id;
              })) break;
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
        const auto request = register_native(session_, workspace_path, target, plan);
        std::map<std::string, std::string> generated;
        for (const auto& generator : plan.generators) {
          const auto& receipt = *session_.db().peek<ActionReceipt>(action_key(generator));
          for (std::size_t i = 0; i < generator.action.outputs.size(); ++i) {
            const auto& output = generator.action.outputs[i];
            const auto& bytes = (*receipt)[i].contents.bytes;
            generated.emplace(generated_path(cwd, generator.name, output.filename),
                              std::string(bytes.begin(), bytes.end()));
          }
        }
        auto& previous = generated_paths_[workspace_path];
        apply([&](engine::Db& db) {
          for (const auto& path : previous)
            if (!generated.contains(path)) session_.set_generated(path, std::nullopt);
          for (const auto& [path, bytes] : generated) session_.set_generated(path, bytes);
          db.set<PreparedBuild>(workspace, Prepared{key, plan_id, request});
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
      std::optional<std::string> error;
      if (!publish(before, [&] {
            if (auto directory = build_directory(cwd, "out")) error = install(output, demand->image);
            else error = std::move(directory.error());
          })) break;
      if (error) return fail(std::move(*error));
      report.output = native(output);
      return report;
    }
  }
  return fail("build cancelled");
}

}  // namespace ycxx::build
