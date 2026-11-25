// One resident workspace request. External tools run between query roots, and their successful
// results reach the graph as inputs at quiescence (#1255).
module;
#include <stdlib.h>
#include <unistd.h>

export module ycxx.build.project;

import std;
import ycxx.ast;
import ycxx.build.action;
import ycxx.build.plan;
import ycxx.build.rule_protocol;
import ycxx.build.rule_worker;
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
};

class Coordinator {
 public:
  Coordinator(pipeline::Session& session, std::string executable, std::uint64_t worker_build);

  ProjectReport run(const std::filesystem::path& cwd, const std::string& target,
                    const std::optional<std::string>& tool_path,
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

Fp128 observation_key(const RuleInvocation& invocation) {
  return fingerprint(std::tie(invocation.request, invocation.reads));
}

struct RuleObservation {
  RuleDemand demand;
  Fp128 package;
  Fp128 invocation;
};
struct PreparedProject {
  ProjectPlan plan;
  std::vector<RuleExpansion> expansions;
  std::vector<RuleObservation> observations;
};

using RunRuleAction = std::function<std::expected<std::vector<NamedArtifact>, std::string>(
    std::string_view, std::string_view, const Action&, std::span<const ActionInput>, Fp128,
    const std::function<bool()>&)>;
using RunRulePackage = std::function<std::expected<std::vector<NamedArtifact>, std::string>(
    const RulePackageRef&, const RulePackagePlan&, Fp128, const std::function<bool()>&)>;

std::expected<PreparedProject, std::string> prepare_project(
    const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain,
    const RunRuleAction& run, const RunRulePackage& run_package) {
  PreparedProject prepared;
  for (;;) {
    auto attempt = plan_with_rules(cwd, target, prepared.expansions);
    if (!attempt) return std::unexpected(attempt.error());
    if (auto* plan = std::get_if<ProjectPlan>(&*attempt)) {
      prepared.plan = std::move(*plan);
      return prepared;
    }
    auto& demand = std::get<RuleDemand>(*attempt);
    if (std::ranges::any_of(prepared.expansions, [&](const auto& entry) { return entry.demand == demand; }))
      return std::unexpected(std::format("rule '{}' did not resolve its declaration", demand.target));
    if (toolchain.executable.empty()) return std::unexpected("cannot locate the rule worker executable");

    auto package = plan_rule_package(cwd, demand.package, rule_sdk_header(), toolchain);
    if (!package) return std::unexpected(package.error());
    const auto library_key = package_key(*package);
    auto built = run_package(demand.package, *package, library_key, [&] {
      const auto current = plan_rule_package(cwd, demand.package, rule_sdk_header(), toolchain);
      return current && package_key(*current) == library_key;
    });
    if (!built) return std::unexpected(built.error());

    auto observation = rule_invocation(cwd, demand);
    if (!observation) return std::unexpected(observation.error());
    const auto invocation_key = observation_key(*observation);
    const auto request = lsp::dump(lsp::to_json(observation->request));
    using enum ActionArg::Kind;
    const Action worker{
        .tool = toolchain.executable,
        .args = {{literal, "--internal-rule-worker"}, {input, "library"}, {input, "request"}, {output, "plan"}},
        .environment = {},
        .outputs = {{"plan", "plan.ycxx"}}};
    auto inputs = std::move(observation->reads);
    inputs.push_back({"library", "package/rules.so", std::move(built->front().contents)});
    inputs.push_back({"request", "request.json", text_artifact(request)});
    const Fp128 worker_key = fingerprint(std::tie(worker, inputs, toolchain.executable_build));
    auto lowered = run("RuleWorker", demand.target, worker, inputs, worker_key, [] { return true; });
    if (!lowered) return std::unexpected(lowered.error());
    const auto& fragment = lowered->front().contents.bytes;
    prepared.observations.push_back({demand, library_key, invocation_key});
    prepared.expansions.push_back({std::move(demand), std::string(fragment.begin(), fragment.end())});
  }
}

std::optional<ProjectPlan> observe_project(
    const fs::path& cwd, std::string_view target, const RuleToolchain& toolchain, const PreparedProject& prepared) {
  for (const auto& record : prepared.observations) {
    const auto package = plan_rule_package(cwd, record.demand.package, rule_sdk_header(), toolchain);
    if (!package || package_key(*package) != record.package) return std::nullopt;
    const auto observed = rule_invocation(cwd, record.demand);
    if (!observed || observation_key(*observed) != record.invocation) return std::nullopt;
  }
  auto attempt = plan_with_rules(cwd, target, prepared.expansions);
  if (!attempt) return std::nullopt;
  if (auto* plan = std::get_if<ProjectPlan>(&*attempt)) return std::move(*plan);
  return std::nullopt;
}

}  // namespace

Coordinator::Coordinator(pipeline::Session& session, std::string executable, std::uint64_t worker_build)
    : session_(session), executable_(std::move(executable)), worker_build_(worker_build) {
  using enum engine::Durability;
  session_.db().set_provider<ActionReceipt>([](const Fp128&) -> engine::provided<ActionReceipt::Value> {
    return {std::nullopt, low};
  });
  session_.db().set_provider<PreparedBuild>([](const Fp128&) -> engine::provided<PreparedBuild::Value> {
    return {std::nullopt, low};
  });
}

ProjectReport Coordinator::run(const fs::path& cwd, const std::string& target,
                               const std::optional<std::string>& tool_path,
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
                              std::span<const ActionInput> inputs) -> ActionResult {
    const auto scratch = build_directory(cwd, "jobs");
    if (!scratch) return {1, {}, scratch.error(), {}};
    std::inplace_stop_source stop;
    std::jthread monitor([&](std::stop_token finished) {
      while (!finished.stop_requested()) {
        if (cancelled()) {
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
    if (tracer) {
      event.outcome = result.exit_status == 0 ? engine::Outcome::executed : engine::Outcome::failed;
      event.end = std::chrono::steady_clock::now();
      if (result.exit_status != 0) event.errors = result.err;
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
    if (!still_current()) return std::unexpected(std::format("{} '{}' changed during execution", kind, label));
    if (result.exit_status != 0)
      return std::unexpected(std::format("{} '{}': {}", kind, label,
          result.err.empty() ? std::format("exit status {}", result.exit_status) : result.err));
    if (!result.err.empty()) report.diagnostics.push_back(std::move(result.err));
    apply([&](engine::Db& db) { db.set<ActionReceipt>(id, result.outputs); });
    return std::move(result.outputs);
  };
  const RunRulePackage run_package = [&](const RulePackageRef& package, const RulePackagePlan& plan, Fp128 id,
                                         const std::function<bool()>& still_current)
      -> std::expected<std::vector<NamedArtifact>, std::string> {
    auto known = session_.run<RuleActionResult>(id, poll, nullptr, tracer);
    if (!known || cancelled()) return std::unexpected("build cancelled");
    if (*known) return std::move(**known);
    if (plan.candidates.empty())
      return std::unexpected(std::format("rule package '{}': no configured C++ compiler is available", package.target));

    std::string errors;
    for (const auto& candidate : plan.candidates) {
      const auto label = std::format("{} via {}", package.target, candidate.action.tool);
      const auto attempt = fingerprint(std::tie(candidate, plan.inputs));
      auto compiled = run_rule("RulePackage", label, candidate.action, plan.inputs, attempt, still_current);
      if (compiled) {
        apply([&](engine::Db& db) { db.set<ActionReceipt>(id, *compiled); });
        return compiled;
      }
      if (cancelled() || !still_current()) return std::unexpected(compiled.error());
      if (!errors.empty()) errors += '\n';
      errors += compiled.error();
    }
    return std::unexpected(std::move(errors));
  };

  const auto* native_target = pp::native_target;
  if (!native_target) return fail("project building is unavailable on this host");
  while (!cancelled()) {
    refresh();
    auto planned = prepare_project(cwd, target, toolchain, run_rule, run_package);
    if (!planned) return fail(planned.error());
    const ProjectPlan& plan = planned->plan;
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
          const auto observed = observe_project(cwd, target, toolchain, *planned);
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
      const auto observed = observe_project(cwd, target, toolchain, *planned);
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
