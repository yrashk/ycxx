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

Fp128 observation_key(const RuleInvocation& invocation, std::uint64_t worker_build) {
  return fingerprint(std::tie(invocation.request, invocation.reads, worker_build));
}

struct PreparedProject {
  ProjectPlan plan;
  RuleSnapshot rules;
  std::optional<Fp128> attempt_watch;
};

ProjectWatchState watch_rules(const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain,
                              const RuleSnapshot& snapshot);

using RunRuleAction = std::function<std::expected<std::vector<NamedArtifact>, std::string>(
    std::string_view, std::string_view, const Action&, std::span<const ActionInput>, Fp128,
    const std::function<bool()>&)>;
using RunRulePackage = std::function<std::expected<RulePackageBuild, std::string>(
    const RulePackageRef&, const RulePackagePlan&, Fp128, const std::function<bool()>&)>;
using UseRuleCompiler = std::function<void(const RulePackageRef&, const RulePackagePlan&, const RulePackageAction&)>;

std::expected<void, std::string> prepare_project(
    const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain,
    const RunRuleAction& run, const RunRulePackage& run_package, const UseRuleCompiler& use_compiler,
    PreparedProject& prepared, bool watch) {
  for (;;) {
    auto attempt = plan_with_rules(cwd, target, prepared.rules.expansions);
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

    auto observation = rule_invocation(cwd, demand);
    if (!observation) return std::unexpected(observation.error());
    const auto invocation_key = observation_key(*observation, toolchain.executable_build);
    const auto request = lsp::dump(lsp::to_json(observation->request));
    using enum ActionArg::Kind;
    const Action worker{
        .tool = toolchain.executable,
        .args = {{literal, "--internal-rule-worker"}, {input, "library"}, {input, "request"}, {output, "plan"}},
        .environment = {},
        .outputs = {{"plan", "plan.ycxx"}}};
    auto inputs = std::move(observation->reads);
    inputs.push_back({"library", "package/rules.so", std::move(built->outputs.front().contents)});
    inputs.push_back({"request", "request.json", text_artifact(request)});
    const Fp128 worker_key = fingerprint(std::tie(worker, inputs, toolchain.executable_build));
    auto lowered = run("RuleWorker", demand.target, worker, inputs, worker_key, [&] {
      const auto current = rule_invocation(cwd, demand);
      return current && observation_key(*current, toolchain.executable_build) == invocation_key && current_package();
    });
    if (!lowered) return std::unexpected(lowered.error());
    const auto& fragment = lowered->front().contents.bytes;
    prepared.rules.observations.push_back({demand, library_key, invocation_key});
    prepared.rules.expansions.push_back({std::move(demand), std::string(fragment.begin(), fragment.end())});
  }
}

bool current_rule(const fs::path& cwd, const RuleToolchain& toolchain, const RuleObservation& record) {
  const auto package = plan_rule_package(cwd, record.demand.package, rule_sdk_header(), toolchain);
  if (!package || package_key(*package) != record.package) return false;
  const auto observed = rule_invocation(cwd, record.demand);
  return observed && observation_key(*observed, toolchain.executable_build) == record.invocation;
}

std::optional<ProjectPlan> observe_project(
    const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain, const PreparedProject& prepared) {
  if (!std::ranges::all_of(prepared.rules.observations, [&](const auto& record) {
        return current_rule(cwd, toolchain, record);
      })) return std::nullopt;
  auto attempt = plan_with_rules(cwd, target, prepared.rules.expansions);
  if (!attempt) return std::nullopt;
  if (auto* plan = std::get_if<ProjectPlan>(&*attempt)) return std::move(*plan);
  return std::nullopt;
}

ProjectWatchState watch_rules(const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain,
                              const RuleSnapshot& snapshot) {
  std::vector<RuleExpansion> current;
  std::vector<Fp128> observations;
  std::vector<std::string> paths;
  std::map<std::pair<RulePackageRef::Kind, std::string>, std::pair<bool, Fp128>> packages;
  const auto observe = [&](const RuleDemand& demand) -> std::optional<std::pair<Fp128, Fp128>> {
    const auto key = std::pair{demand.package.kind, demand.package.target};
    auto package = packages.find(key);
    if (package == packages.end()) {
      std::vector<std::string> failed_paths;
      const auto plan = plan_rule_package(cwd, demand.package, rule_sdk_header(), toolchain, &failed_paths);
      paths.append_range(plan ? plan->observed_paths : failed_paths);
      const auto stamp = plan ? package_key(*plan) : fingerprint128(plan.error());
      package = packages.emplace(key, std::pair{plan.has_value(), stamp}).first;
    }
    for (const auto& path : demand.reads) paths.push_back(native(cwd / path));
    const auto invocation = rule_invocation(cwd, demand);
    const auto [package_ok, package_stamp] = package->second;
    const auto invocation_stamp = invocation ? observation_key(*invocation, toolchain.executable_build)
                                             : fingerprint128(invocation.error());
    observations.push_back(fingerprint(std::tuple{package_ok, package_stamp, invocation.has_value(), invocation_stamp}));
    if (!package_ok || !invocation) return std::nullopt;
    return std::pair{package_stamp, invocation_stamp};
  };
  for (std::size_t i = 0; i < snapshot.observations.size(); ++i) {
    const auto& record = snapshot.observations[i];
    const auto state = observe(record.demand);
    if (state == std::pair{record.package, record.invocation}) current.push_back(snapshot.expansions[i]);
  }
  for (const auto& probe : snapshot.probes) {
    const auto matches = current_probe_profile(cwd, toolchain, probe);
    const auto stamp = matches ? fingerprint_toolchain_probe_inputs(probe.inputs) : Fp128{};
    observations.push_back(fingerprint(std::tie(matches, stamp)));
    if (matches) {
      paths.append_range(probe.inputs.files);
      paths.append_range(probe.inputs.trees);
    }
  }
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
      if (current_rule(cwd, toolchain, snapshot->observations[i]))
        current.push_back(std::move(snapshot->expansions[i]));
    }
  }
  auto inspection = inspect_project(cwd, target, current);
  if (inspection && snapshot)
    for (auto& probe : snapshot->probes) {
      const bool expanded = std::ranges::any_of(current, [&](const auto& expansion) {
        return expansion.demand.package == probe.package;
      });
      if (expanded && current_probe_profile(cwd, toolchain, probe) &&
          fingerprint_toolchain_probe_inputs(probe.inputs) == probe.stamp)
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
                              std::chrono::milliseconds timeout = {}) -> ActionResult {
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
    auto result = execute(action, inputs, *scratch, stop.get_token());
    monitor.request_stop();
    monitor.join();
    if (timed_out && result.completion == ActionCompletion::cancelled) {
      result.completion = ActionCompletion::timed_out;
      result.err = "compiler capability probe timed out";
    }
    if (tracer) {
      event.outcome = result.exit_status == 0 ? engine::Outcome::executed : engine::Outcome::failed;
      event.end = std::chrono::steady_clock::now();
      if (result.exit_status != 0)
        event.errors = timeout.count() == 0 ? result.err : result.err.substr(0, 4096);
      tracer->event(std::move(event));
    }
    tick();
    return result;
  };
  const RunRuleAction run_rule = [&](std::string_view kind, std::string_view label, const Action& action,
                                   std::span<const ActionInput> inputs, Fp128 id,
                                   const std::function<bool()>& still_current)
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
  const RunRulePackage run_package = [&](const RulePackageRef& package, const RulePackagePlan& plan, Fp128 id,
                                         const std::function<bool()>& still_current)
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
  const auto saved = read_rule_snapshot(cwd, target);
  const RunProbe run_probe = [&](std::string_view kind, std::string_view label, const Action& action,
                                 std::span<const ActionInput> inputs) {
    return run_action(kind, label, action, inputs, std::chrono::seconds(10));
  };
  std::vector<RulePackageRef> measured;
  while (!cancelled()) {
    refresh();
    superseded = false;
    PreparedProject planned;
    measured.clear();
    const UseRuleCompiler use_compiler = [&](const RulePackageRef& package, const RulePackagePlan& plan,
                                             const RulePackageAction& selected) {
      if (!selected.cxx || std::ranges::contains(measured, package) || cancelled()) return;
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
      std::vector<std::pair<ProbeKey, MeasuredToolchainProbe>> pending;
      const MeasureProbe measure = [&](ProbeKey key, const std::function<MeasuredToolchainProbe()>& execute) {
        const auto known = session_.run<ToolchainProbeResult>(key, poll, nullptr, tracer);
        if (known && *known) return **known;
        auto fresh = execute();
        if (fresh.cacheable && !cancelled()) pending.emplace_back(std::move(key), fresh);
        return fresh;
      };
      ToolchainProbeProfile profile{selected.action.tool, {}, {}, selected.native,
                                    plan.probe_flags, selected.action.environment};
      auto matrix = measure_toolchain_matrix(cwd, package.target, std::move(profile),
          probe_identity(plan, selected, worker_build_), plan.probe_inputs, executable_, run_probe, measure, cancelled);
      if (cancelled()) return;
      if (!matrix) {
        report.diagnostics.push_back(std::format("toolchain profile '{}' could not be measured: {}",
                                                 package.target, matrix.error()));
        return;
      }
      RuleProbeSnapshot captured{package, profile_key, std::move(matrix->inputs), matrix->stamp,
                                 std::move(matrix->facts)};
      if (!current_probe_profile(cwd, toolchain, captured)) return;
      if (!pending.empty()) {
        apply([&](engine::Db& db) {
          for (auto& [key, record] : pending) db.set<ToolchainProbeReceipt>(key, std::move(record));
        });
        for (const auto& [key, record] : pending)
          (void)session_.run<ToolchainProbeResult>(key, poll, nullptr, tracer);
      }
      planned.rules.probes.push_back(std::move(captured));
    };
    auto prepared = prepare_project(cwd, target, toolchain, run_rule, run_package, use_compiler, planned, watch);
    if (cancelled()) return fail("build cancelled");
    if (superseded) continue;
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
