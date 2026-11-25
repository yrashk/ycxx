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
  Fp128 profile;
  std::uint32_t row;
  bool operator==(const ProbeKey&) const = default;
};

struct MeasuredToolchainProbe {
  ProbeEvidence evidence;
  std::optional<Artifact> artifact;
  ToolchainProbeInputs discovered_inputs;
  std::optional<Fp128> discovery_stamp = std::nullopt;
  bool cacheable = true;
};

struct [[=engine::input{engine::Durability::low}]] ToolchainProbeReceipt {
  using Key = ProbeKey;
  using Value = std::optional<MeasuredToolchainProbe>;
};

struct ToolchainProbeResult {
  using Key = ToolchainProbeReceipt::Key;
  using Value = ToolchainProbeReceipt::Value;
  static std::string describe(const Key& key) {
    const auto rows = baseline_toolchain_probes();
    constexpr std::string_view synthetic[] = {"driver.search-paths", "link.host-shared.load", "link.hosted.run"};
    const auto id = key.row < rows.size() ? rows[key.row].id : synthetic[key.row - rows.size()];
    return std::format("{} [{:08x}]", id, static_cast<std::uint32_t>(key.profile.lo));
  }
  static std::string show(const Value& value);
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
      if (result.exit_status == 126 || result.exit_status == 127) return ProbeCompletion::setup_failure;
      if (result.exit_status >= 128) return ProbeCompletion::signalled;
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
         state == ProbeCompletion::output_failure || state == ProbeCompletion::timed_out;
}

MeasuredToolchainProbe unavailable(std::string reason, bool cacheable = true) {
  MeasuredToolchainProbe observation{.evidence = {}, .artifact = {}, .discovered_inputs = {},
                                     .cacheable = cacheable};
  observation.evidence.reason = std::move(reason);
  return observation;
}

std::string_view name(ProbeSupport support) {
  constexpr std::string_view names[] = {"supported", "unsupported", "indeterminate"};
  static_assert(std::size(names) == 1 + std::to_underlying(ProbeSupport::indeterminate));
  return names[std::to_underlying(support)];
}

void add_diagnostic(ProbeEvidence& evidence, std::string_view stderr_text) {
  if (evidence.support == ProbeSupport::supported) return;
  if (const auto first = stderr_text.find_first_not_of(" \r\n\t"); first != stderr_text.npos) {
    stderr_text.remove_prefix(first);
    evidence.reason += std::format(" · {}", stderr_text.substr(0, std::min(stderr_text.find('\n'), std::size_t{240})));
  }
}

void add_fact(ToolchainMatrix& matrix, std::string_view package, std::string_view id,
              const ProbeEvidence& evidence) {
  auto origin = evidence.reason;
  if (evidence.exit_status) origin += std::format(" (exit {})", *evidence.exit_status);
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
                                .artifact = {}, .discovered_inputs = {}, .cacheable = reusable(state)};
  add_diagnostic(record.evidence, result.err);
  if ((probe.phase == ToolchainProbePhase::shared_link || probe.id == "link.hosted") &&
      record.evidence.support == ProbeSupport::supported)
    record.artifact = std::move(result.outputs.front().contents);
  return record;
}

MeasuredToolchainProbe discover_inputs(const ToolchainProbeProfile& profile, const RunProbe& run) {
  static constexpr std::string_view include_args[] = {"-E", "-x", "c++", "-v"};
  static constexpr std::string_view search_args[] = {"-print-search-dirs"};
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
  result.discovery_stamp = fingerprint_toolchain_probe_inputs(result.discovered_inputs);
  return result;
}

MeasuredToolchainProbe run_host_witness(const ToolchainProbeProfile& profile, std::string_view worker,
                                       const MeasuredToolchainProbe& linked, const RunProbe& run, bool shared) {
  using enum ActionArg::Kind;
  if (!linked.artifact)
    return unavailable("preceding link did not produce a verified artifact", linked.cacheable);
  auto observed = unavailable("artifact target cannot run on this host");
  observed.evidence.target = linked.evidence.target;
  observed.evidence.artifact = linked.evidence.artifact;
  const auto* host = pp::native_target;
  if (!host || !linked.evidence.artifact || !linked.evidence.artifact->host_runnable ||
      !toolchain_probe_host_matches(linked.evidence.target, host->triple))
    return observed;
  const Action action{.tool = std::string(worker),
      .args = {{literal, shared ? "--internal-probe-shared" : "--internal-probe-executable"}, {input, "binary"}},
      .environment = profile.environment, .outputs = {}};
  const ActionInput artifact{"binary", shared ? "probe.so" : "probe", *linked.artifact};
  const auto result = run(shared ? "ToolchainProbeLoad" : "ToolchainProbeRun",
                          std::format("{} via {}", shared ? "link.host-shared.load" : "link.hosted.run",
                                      profile.executable), action, std::span{&artifact, 1});
  const auto state = completion(result);
  observed.cacheable = reusable(state);
  observed.evidence.exit_status = result.exit_status;
  observed.evidence.reason = "host witness did not complete";
  if (state == ProbeCompletion::completed) {
    observed.evidence.support = ProbeSupport::supported;
    observed.evidence.reason = shared ? "loaded library and called ycxx_rule_plan_v1" : "ran executable on the host";
  } else if (state == ProbeCompletion::rejected) {
    observed.evidence.support = ProbeSupport::unsupported;
    observed.evidence.reason = shared ? "host loader or rule entry point rejected the library"
                                      : "executable returned an unexpected status";
  }
  add_diagnostic(observed.evidence, result.err);
  return observed;
}

}  // namespace

std::string ToolchainProbeResult::show(const Value& value) {
  if (!value) return "Measurement needed";
  const auto& evidence = value->evidence;
  if (!evidence.target.empty())
    return std::format("{}\n{}\nTarget {}\n", name(evidence.support), evidence.reason, evidence.target);
  return std::format("{}\n{}\n", name(evidence.support), evidence.reason);
}

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
    const auto sdk = std::ranges::find_if(profile.environment, [](const auto& variable) {
      return variable.first == "SDKROOT";
    });
    merge_inputs(matrix.inputs, {pp::all_system_dirs(sdk == profile.environment.end() ? "" : sdk->second), {}});
  }
  const auto configured_inputs = matrix.inputs;
  const auto configured = fingerprint_toolchain_probe_inputs(configured_inputs);
  const auto catalogue = baseline_toolchain_probes();
  const auto& target_probe = catalogue.front();
  auto default_target = stage_toolchain_probe(target_probe, profile);
  if (!default_target) return std::unexpected(default_target.error());
  const auto default_key = fingerprint(std::tie(catalogue_version, identity, configured,
                                                default_target->action, default_target->inputs));
  const auto target = measure({default_key, 0},
                              [&] { return execute_probe(target_probe, *default_target, run); });
  if (target.evidence.support == ProbeSupport::supported) profile.observed_target = target.evidence.target;

  std::optional<MeasuredToolchainProbe> discovery;
  if (!profile.native && !cancelled()) {
    const auto search_key = fingerprint(std::tie(catalogue_version, identity, configured, profile.observed_target));
    discovery = measure({search_key, static_cast<std::uint32_t>(catalogue.size())},
                        [&] { return discover_inputs(profile, run); });
    merge_inputs(matrix.inputs, discovery->discovered_inputs);
  }
  matrix.stamp = fingerprint_toolchain_probe_inputs(matrix.inputs);
  matrix.facts.push_back({"toolchain-compiler", std::string(package), "driver", profile.executable,
                          profile.native ? "Selected native compiler" : "Selected host compiler"});
  add_fact(matrix, package, target_probe.id, target.evidence);

  for (std::uint32_t row = 1; row < catalogue.size(); ++row) {
    const auto& probe = catalogue[row];
    if (cancelled()) return std::unexpected("build cancelled");
    auto staged = stage_toolchain_probe(probe, profile);
    if (!staged) {
      add_fact(matrix, package, probe.id, unavailable(staged.error()).evidence);
      continue;
    }
    const auto key = fingerprint(std::tie(catalogue_version, identity, matrix.stamp, staged->action,
                                         staged->inputs, staged->expected_target));
    const auto result = measure({key, row}, [&] { return execute_probe(probe, *staged, run); });
    add_fact(matrix, package, probe.id, result.evidence);
    const bool shared = probe.phase == ToolchainProbePhase::shared_link;
    if ((shared || probe.id == "link.hosted") && !cancelled()) {
      const auto witness_key = fingerprint(std::tie(key, worker, result.evidence, result.artifact));
      const auto witness = measure({witness_key, static_cast<std::uint32_t>(catalogue.size() + (shared ? 1 : 2))},
                                   [&] { return run_host_witness(profile, worker, result, run, shared); });
      add_fact(matrix, package, shared ? "link.host-shared.load" : "link.hosted.run", witness.evidence);
    }
  }
  if (matrix.stamp != fingerprint_toolchain_probe_inputs(matrix.inputs) ||
      configured != fingerprint_toolchain_probe_inputs(configured_inputs) ||
      (discovery && discovery->discovery_stamp != fingerprint_toolchain_probe_inputs(discovery->discovered_inputs)))
    return std::unexpected("toolchain inputs changed while measuring the profile");
  return matrix;
}

}  // namespace ycxx::build
