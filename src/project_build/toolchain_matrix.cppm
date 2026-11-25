// Measure a versioned catalogue against one selected compiler profile.
export module ycxx.build.toolchain_matrix;

import std;
import ycxx.build.action;
import ycxx.build.inspection_model;
import ycxx.build.toolchain_adapter;
import ycxx.build.toolchain_inputs;
import ycxx.build.toolchain_probe;
import ycxx.engine;
import ycxx.hash;
import ycxx.pp;

export namespace ycxx::build {

struct ProbeKey {
  std::string name;
  Fp128 profile;
  bool operator==(const ProbeKey&) const = default;
};

struct MeasuredToolchainProbe {
  ProbeEvidence evidence;
  std::optional<Artifact> shared_library;
  ToolchainProbeInputs discovered_inputs;
  bool cacheable = true;
};

struct [[=engine::input{engine::Durability::low}]] ToolchainProbeReceipt {
  using Key = ProbeKey;
  using Value = std::optional<MeasuredToolchainProbe>;
};

struct ToolchainProbeResult {
  using Key = ToolchainProbeReceipt::Key;
  using Value = ToolchainProbeReceipt::Value;
  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    co_return cx.fetch<ToolchainProbeReceipt>(key);
  }
};

struct ToolchainMatrix {
  ToolchainProbeInputs inputs;
  Fp128 stamp;
  std::vector<PlanFact> facts;
};

using RunProbe = std::function<ActionResult(
    std::string_view kind, std::string_view label, const Action&, std::span<const ActionInput>)>;
using MeasureProbe = std::function<MeasuredToolchainProbe(
    ProbeKey, const std::function<MeasuredToolchainProbe()>&)>;

std::expected<ToolchainMatrix, std::string> measure_toolchain_matrix(
    const std::filesystem::path& cwd, std::string_view package, ToolchainProbeProfile profile,
    Fp128 identity, std::span<const std::string> declared_inputs,
    std::string_view worker, const RunProbe& run, const MeasureProbe& measure,
    const std::function<bool()>& cancelled);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

inline constexpr int catalogue_version = 1;

ProbeCompletion completion(const ActionResult& result) {
  switch (result.completion) {
    case ActionCompletion::exited:
      return result.exit_status == 0 ? ProbeCompletion::completed : ProbeCompletion::rejected;
    case ActionCompletion::signalled: return ProbeCompletion::signalled;
    case ActionCompletion::cancelled: return ProbeCompletion::cancelled;
    case ActionCompletion::timed_out: return ProbeCompletion::timed_out;
    case ActionCompletion::setup_failed: return ProbeCompletion::setup_failure;
    case ActionCompletion::output_failed: return ProbeCompletion::output_failure;
  }
  std::unreachable();
}

bool reusable(ProbeCompletion state) {
  return state == ProbeCompletion::completed || state == ProbeCompletion::rejected ||
         state == ProbeCompletion::output_failure;
}

MeasuredToolchainProbe unavailable(std::string reason, bool cacheable = true) {
  MeasuredToolchainProbe observation{.evidence = {}, .shared_library = {}, .discovered_inputs = {},
                                     .cacheable = cacheable};
  observation.evidence.reason = std::move(reason);
  return observation;
}

std::string_view name(ProbeSupport support) {
  constexpr std::string_view names[] = {"supported", "unsupported", "indeterminate"};
  static_assert(std::size(names) == 1 + std::to_underlying(ProbeSupport::indeterminate));
  return names[std::to_underlying(support)];
}

void add_fact(ToolchainMatrix& matrix, std::string_view package, std::string_view id,
              const ProbeEvidence& evidence) {
  auto origin = std::format("{} (exit {})", evidence.reason, evidence.exit_status);
  if (!evidence.target.empty()) origin += std::format(" · {}", evidence.target);
  if (evidence.artifact) {
    constexpr std::string_view formats[] = {"ELF", "Mach-O", "COFF"};
    constexpr std::string_view kinds[] = {"object", "executable", "shared library"};
    origin += std::format(" · {} {} {}", evidence.artifact->architecture,
                          formats[std::to_underlying(evidence.artifact->format)],
                          kinds[std::to_underlying(evidence.artifact->kind)]);
  }
  matrix.facts.push_back({"toolchain-capability", std::string(package), std::string(id),
                          std::string(name(evidence.support)), std::move(origin)});
}

void merge_inputs(ToolchainProbeInputs& into, ToolchainProbeInputs from) {
  into.trees.append_range(std::move(from.trees));
  into.files.append_range(std::move(from.files));
  for (auto* paths : {&into.trees, &into.files}) {
    std::ranges::sort(*paths);
    auto duplicates = std::ranges::unique(*paths);
    paths->erase(duplicates.begin(), duplicates.end());
  }
}

MeasuredToolchainProbe execute_probe(const ToolchainProbe& probe, const StagedToolchainProbe& staged,
                                    const RunProbe& run) {
  const auto label = std::format("{} via {}", probe.id, staged.action.tool);
  auto result = run("ToolchainProbeAction", label, staged.action, staged.inputs);
  const auto state = completion(result);
  MeasuredToolchainProbe record{.evidence = classify_toolchain_probe(probe, staged.expected_target, result, state),
                                .shared_library = {}, .discovered_inputs = {}, .cacheable = reusable(state)};
  if (probe.phase == ToolchainProbePhase::shared_link && record.evidence.support == ProbeSupport::supported)
    record.shared_library = std::move(result.outputs.front().contents);
  return record;
}

MeasuredToolchainProbe discover_inputs(const ToolchainProbeProfile& profile, const RunProbe& run) {
  constexpr std::string_view include_args[] = {"-E", "-x", "c++", "-v"};
  constexpr std::string_view search_args[] = {"-print-search-dirs"};
  constexpr ToolchainProbe includes{"driver.include-search", "Compiler include search", ToolchainProbePhase::syntax,
                                     "// Search paths only.\n", include_args, {}, {}};
  constexpr ToolchainProbe search{"driver.tool-search", "Compiler tool search", ToolchainProbePhase::syntax,
                                   {}, search_args, {}, {}};
  const auto capture = [&](const ToolchainProbe& probe) -> ActionResult {
    auto staged = stage_toolchain_probe(probe, profile);
    if (!staged)
      return {.exit_status = 1, .out = {}, .err = staged.error(), .outputs = {},
              .completion = ActionCompletion::setup_failed};
    return run("ToolchainProbeAction", std::format("{} via {}", probe.id, profile.executable),
               staged->action, staged->inputs);
  };
  const auto include = capture(includes);
  const auto directories = include.completion == ActionCompletion::cancelled ? ActionResult{} : capture(search);
  auto result = unavailable("reported compiler search paths", reusable(completion(include)) &&
                                                                reusable(completion(directories)));
  result.discovered_inputs = reported_toolchain_probe_inputs(include.err, directories.out);
  return result;
}

MeasuredToolchainProbe load_shared(const ToolchainProbeProfile& profile, std::string_view worker,
                                  const MeasuredToolchainProbe& linked, const RunProbe& run) {
  using enum ActionArg::Kind;
  if (!linked.shared_library)
    return unavailable("shared library link did not produce a verified artifact", linked.cacheable);
  const auto* host = pp::native_target;
  if (!host || !toolchain_probe_host_matches(linked.evidence.target, host->triple))
    return unavailable("shared library target cannot be loaded on this host");
  const Action action{.tool = std::string(worker),
      .args = {{literal, "--internal-probe-shared"}, {input, "library"}},
      .environment = profile.environment, .outputs = {}};
  const ActionInput artifact{"library", "probe.so", *linked.shared_library};
  const auto result = run("ToolchainProbeLoad", std::format("link.host-shared.load via {}", profile.executable),
                          action, std::span{&artifact, 1});
  const auto state = completion(result);
  auto loaded = unavailable("host loader did not complete the rule entry point witness", reusable(state));
  loaded.evidence.target = linked.evidence.target;
  loaded.evidence.artifact = linked.evidence.artifact;
  loaded.evidence.exit_status = result.exit_status;
  if (state == ProbeCompletion::completed) {
    loaded.evidence.support = ProbeSupport::supported;
    loaded.evidence.reason = "loaded library and called ycxx_rule_plan_v1";
  } else if (state == ProbeCompletion::rejected) {
    loaded.evidence.support = ProbeSupport::unsupported;
    loaded.evidence.reason = "host loader or rule entry point rejected the library";
  }
  return loaded;
}

}  // namespace

std::expected<ToolchainMatrix, std::string> measure_toolchain_matrix(
    const std::filesystem::path& cwd, std::string_view package, ToolchainProbeProfile profile,
    Fp128 identity, std::span<const std::string> declared_inputs,
    std::string_view worker, const RunProbe& run, const MeasureProbe& measure,
    const std::function<bool()>& cancelled) {
  auto normalized = normalize_toolchain_probe_profile(std::move(profile));
  if (!normalized) return std::unexpected(normalized.error());
  profile = std::move(*normalized);

  ToolchainMatrix matrix{.inputs = configured_toolchain_probe_inputs(cwd, profile.executable, profile.flags,
                                                                   profile.environment, declared_inputs),
                         .stamp = {}, .facts = {}};
  if (profile.native) {
    const auto sdk = std::ranges::find(profile.environment, "SDKROOT", &std::pair<std::string, std::string>::first);
    merge_inputs(matrix.inputs, {pp::all_system_dirs(sdk == profile.environment.end() ? "" : sdk->second), {}});
  }
  const auto configured = fingerprint_toolchain_probe_inputs(matrix.inputs);
  const auto catalogue = baseline_toolchain_probes();
  const auto& target_probe = catalogue.front();
  auto default_target = stage_toolchain_probe(target_probe, profile);
  if (!default_target) return std::unexpected(default_target.error());
  const auto default_key = fingerprint(std::tie(catalogue_version, identity, configured,
                                                default_target->action, default_target->inputs));
  const auto target = measure({std::string(target_probe.id), default_key},
                              [&] { return execute_probe(target_probe, *default_target, run); });
  if (target.evidence.support == ProbeSupport::supported) profile.observed_target = target.evidence.target;

  if (!profile.native && !cancelled()) {
    const auto search_key = fingerprint(std::tie(catalogue_version, identity, configured, profile.observed_target));
    auto discovery = measure({"driver.search-paths", search_key}, [&] { return discover_inputs(profile, run); });
    merge_inputs(matrix.inputs, std::move(discovery.discovered_inputs));
  }
  matrix.stamp = fingerprint_toolchain_probe_inputs(matrix.inputs);
  matrix.facts.push_back({"toolchain-compiler", std::string(package), "driver", profile.executable,
                          profile.native ? "Selected native compiler" : "Selected host compiler"});
  add_fact(matrix, package, target_probe.id, target.evidence);

  for (const auto& probe : catalogue.subspan(1)) {
    if (cancelled()) return std::unexpected("build cancelled");
    auto staged = stage_toolchain_probe(probe, profile);
    if (!staged) {
      add_fact(matrix, package, probe.id, unavailable(staged.error()).evidence);
      continue;
    }
    const auto key = fingerprint(std::tie(catalogue_version, identity, matrix.stamp, staged->action,
                                         staged->inputs, staged->expected_target));
    const auto result = measure({std::string(probe.id), key}, [&] { return execute_probe(probe, *staged, run); });
    add_fact(matrix, package, probe.id, result.evidence);
    if (probe.phase == ToolchainProbePhase::shared_link && !cancelled()) {
      constexpr std::string_view load = "link.host-shared.load";
      const auto load_key = fingerprint(std::tie(key, worker, result.evidence, result.shared_library));
      const auto loaded = measure({std::string(load), load_key}, [&] { return load_shared(profile, worker, result, run); });
      add_fact(matrix, package, load, loaded.evidence);
    }
  }
  if (matrix.stamp != fingerprint_toolchain_probe_inputs(matrix.inputs))
    return std::unexpected("toolchain inputs changed while measuring the profile");
  return matrix;
}

}  // namespace ycxx::build
