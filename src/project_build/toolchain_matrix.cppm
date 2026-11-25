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
  static std::string describe(const Key& key);
  static std::string show(const Value& value);
  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    co_return cx.fetch<ToolchainProbeReceipt>(key);
  }
};

struct ToolchainObservation {
  std::string name;
  ProbeEvidence evidence;
  ProbeKey key;
};

struct ToolchainMatrix {
  ToolchainProbeInputs inputs;
  Fp128 stamp;
  std::vector<PlanFact> facts;
  std::vector<ToolchainObservation> observations;
};

using RunProbe = std::function<ActionResult(
    std::string_view kind, std::string_view label, const Action&, std::span<const ActionInput>)>;
using MeasureProbe = std::function<MeasuredToolchainProbe(
    ProbeKey, const std::function<MeasuredToolchainProbe()>&)>;

bool known_toolchain_probe(std::string_view id);
std::string_view probe_support_name(ProbeSupport support);
std::string describe_probe_evidence(const ProbeEvidence& evidence);

std::expected<ToolchainMatrix, std::string> measure_toolchain_matrix(
    const std::filesystem::path& cwd, std::string_view package, ToolchainProbeProfile profile,
    Fp128 identity, std::span<const std::string> declared_inputs,
    std::string_view worker, const RunProbe& run, const MeasureProbe& measure,
    const std::function<bool()>& cancelled, std::span<const std::string> requested = {});

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

inline constexpr int catalogue_version = 1;
enum class SyntheticProbe : std::uint32_t { search_paths, shared_load, hosted_run };
constexpr std::string_view synthetic_probes[] = {"driver.search-paths", "link.host-shared.load", "link.hosted.run"};

std::string_view synthetic_name(SyntheticProbe probe) {
  return synthetic_probes[std::to_underlying(probe)];
}

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

void add_diagnostic(ProbeEvidence& evidence, std::string_view stderr_text) {
  if (evidence.support == ProbeSupport::supported) return;
  if (const auto first = stderr_text.find_first_not_of(" \r\n\t"); first != stderr_text.npos) {
    stderr_text.remove_prefix(first);
    evidence.reason += std::format(" · {}", stderr_text.substr(0, std::min(stderr_text.find('\n'), std::size_t{240})));
  }
}

void publish_observation(ToolchainMatrix& matrix, std::string_view package, std::string_view id,
                         const ProbeEvidence& evidence, ProbeKey key) {
  matrix.facts.push_back({"toolchain-capability", std::string(package), std::string(id),
                          std::string(probe_support_name(evidence.support)), describe_probe_evidence(evidence)});
  matrix.observations.push_back({std::string(id), evidence, key});
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
  const auto id = synthetic_name(shared ? SyntheticProbe::shared_load : SyntheticProbe::hosted_run);
  const auto label = std::format("{} via {}", id, profile.executable);
  const auto result = run(shared ? "ToolchainProbeLoad" : "ToolchainProbeRun", label, action, std::span{&artifact, 1});
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

std::string_view probe_support_name(ProbeSupport support) {
  constexpr std::string_view names[] = {"supported", "unsupported", "indeterminate"};
  static_assert(std::size(names) == 1 + std::to_underlying(ProbeSupport::indeterminate));
  return names[std::to_underlying(support)];
}

std::string describe_probe_evidence(const ProbeEvidence& evidence) {
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
  return origin;
}

bool known_toolchain_probe(std::string_view id) {
  return std::ranges::contains(baseline_toolchain_probes(), id, &ToolchainProbe::id) ||
         id == synthetic_name(SyntheticProbe::shared_load) || id == synthetic_name(SyntheticProbe::hosted_run);
}

std::string ToolchainProbeResult::describe(const Key& key) {
  const auto rows = baseline_toolchain_probes();
  const auto id = key.row < rows.size() ? rows[key.row].id
                                        : synthetic_name(static_cast<SyntheticProbe>(key.row - rows.size()));
  return std::format("{} [{:08x}]", id, static_cast<std::uint32_t>(key.profile.lo));
}

std::string ToolchainProbeResult::show(const Value& value) {
  if (!value) return "Measurement needed";
  const auto& evidence = value->evidence;
  if (!evidence.target.empty())
    return std::format("{}\n{}\nTarget {}\n", probe_support_name(evidence.support), evidence.reason, evidence.target);
  return std::format("{}\n{}\n", probe_support_name(evidence.support), evidence.reason);
}

std::expected<ToolchainMatrix, std::string> measure_toolchain_matrix(
    const std::filesystem::path& cwd, std::string_view package, ToolchainProbeProfile profile,
    Fp128 identity, std::span<const std::string> declared_inputs,
    std::string_view worker, const RunProbe& run, const MeasureProbe& measure,
    const std::function<bool()>& cancelled, std::span<const std::string> requested) {
  for (const auto& id : requested)
    if (!known_toolchain_probe(id)) return std::unexpected(std::format("unknown toolchain probe '{}'", id));
  if (cancelled()) return std::unexpected("build cancelled");
  auto normalized = normalize_toolchain_probe_profile(std::move(profile));
  if (!normalized) return std::unexpected(normalized.error());
  profile = std::move(*normalized);

  const auto wants = [&](std::string_view id) {
    return requested.empty() || std::ranges::contains(requested, id);
  };
  ToolchainMatrix matrix{.inputs = configured_toolchain_probe_inputs(cwd, profile.executable, profile.flags,
                                                                   profile.environment, declared_inputs),
                         .stamp = {}, .facts = {}, .observations = {}};
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
  const auto unstaged_key = [&](Fp128 stamp) {
    return fingerprint(std::tuple{catalogue_version, identity, stamp, std::string_view("unstaged"), profile});
  };
  matrix.facts.push_back({"toolchain-compiler", std::string(package), "driver", profile.executable,
                          profile.native ? "Selected native compiler" : "Selected host compiler"});
  // Even an explicit target can match the driver's default. Staging uses the
  // observed target to avoid passing a redundant target flag in that case.
  if (wants(target_probe.id) || profile.observed_target.empty()) {
    auto staged = stage_toolchain_probe(target_probe, profile);
    const ProbeKey key{staged ? fingerprint(std::tie(catalogue_version, identity, configured,
                                                       staged->action, staged->inputs))
                              : unstaged_key(configured), 0};
    const auto target = staged ? measure(key, [&] { return execute_probe(target_probe, *staged, run); })
                               : unavailable(staged.error());
    if (target.evidence.support == ProbeSupport::supported) profile.observed_target = target.evidence.target;
    publish_observation(matrix, package, target_probe.id, target.evidence, key);
  }

  std::optional<MeasuredToolchainProbe> discovery;
  const bool needs_discovery = requested.empty() || std::ranges::any_of(requested, [&](std::string_view id) {
    return id != target_probe.id;
  });
  if (!profile.native && needs_discovery && !cancelled()) {
    const auto search_key = fingerprint(std::tie(catalogue_version, identity, configured, profile.observed_target));
    discovery = measure({search_key, static_cast<std::uint32_t>(catalogue.size()) +
                                         std::to_underlying(SyntheticProbe::search_paths)},
                        [&] { return discover_inputs(profile, run); });
    merge_inputs(matrix.inputs, discovery->discovered_inputs);
  }
  matrix.stamp = fingerprint_toolchain_probe_inputs(matrix.inputs);

  for (std::uint32_t row = 1; row < catalogue.size(); ++row) {
    const auto& probe = catalogue[row];
    const bool shared = probe.id == "link.host-shared";
    const auto witness_kind = shared ? SyntheticProbe::shared_load : SyntheticProbe::hosted_run;
    const auto witness_name = synthetic_name(witness_kind);
    const bool wants_witness = (shared || probe.id == "link.hosted") && wants(witness_name);
    if (!wants(probe.id) && !wants_witness) continue;
    if (cancelled()) return std::unexpected("build cancelled");
    auto staged = stage_toolchain_probe(probe, profile);
    const ProbeKey key{staged ? fingerprint(std::tie(catalogue_version, identity, matrix.stamp, staged->action,
                                                      staged->inputs, staged->expected_target))
                              : unstaged_key(matrix.stamp), row};
    const auto result = staged ? measure(key, [&] { return execute_probe(probe, *staged, run); })
                               : unavailable(staged.error());
    publish_observation(matrix, package, probe.id, result.evidence, key);
    if (wants_witness && !cancelled()) {
      const ProbeKey witness_key{fingerprint(std::tie(key.profile, worker, result.evidence, result.artifact)),
                                  static_cast<std::uint32_t>(catalogue.size()) + std::to_underlying(witness_kind)};
      const auto execute = [&] { return run_host_witness(profile, worker, result, run, shared); };
      const auto witness = staged ? measure(witness_key, execute) : execute();
      publish_observation(matrix, package, witness_name, witness.evidence, witness_key);
    }
  }
  if (cancelled()) return std::unexpected("build cancelled");
  if (matrix.stamp != fingerprint_toolchain_probe_inputs(matrix.inputs) ||
      configured != fingerprint_toolchain_probe_inputs(configured_inputs) ||
      (discovery && discovery->discovery_stamp != fingerprint_toolchain_probe_inputs(discovery->discovered_inputs)))
    return std::unexpected("toolchain inputs changed while measuring the profile");
  return matrix;
}

}  // namespace ycxx::build
