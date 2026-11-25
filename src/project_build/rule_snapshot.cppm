// Persist admitted rule expansions for a read-only inspector (#1281).
module;
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

export module ycxx.build.rule_snapshot;

import std;
import ycxx.build.inspection_model;
import ycxx.build.rule_protocol;
import ycxx.build.toolchain_inputs;
import ycxx.hash;
import ycxx.lsp;

export namespace ycxx::build {

struct RuleProbeSnapshot {
  RulePackageRef package;
  Fp128 profile;
  ToolchainProbeInputs inputs;
  Fp128 stamp;
  std::vector<PlanFact> facts;
};

struct RuleObservation {
  RuleDemand demand;
  Fp128 package;
  Fp128 invocation;
  std::vector<RuleProbeSnapshot> projections = {};
  std::optional<Fp128> witness = std::nullopt;
};

struct RuleSnapshot {
  std::vector<RuleExpansion> expansions;
  std::vector<RuleObservation> observations;  // in expansion order
  std::vector<RuleProbeSnapshot> probes;
  std::vector<RuleProbeSnapshot> pending_projections = {};  // in-memory watch state for a failed rule
  std::optional<RuleExpansion> pending_witness = std::nullopt;
};

// Snapshot bytes are only a candidate. The caller validates the observations against current
// package inputs and declared reads before using the stored expansions.
std::optional<RuleSnapshot> read_rule_snapshot(const std::filesystem::path& cwd, std::string_view target);
void write_rule_snapshot(const std::filesystem::path& cwd, std::string_view target, const RuleSnapshot& snapshot);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

constexpr int snapshot_version = 1;
constexpr std::size_t max_snapshot_size = 8 * 1024 * 1024;
constexpr int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

struct Fd {
  explicit Fd(int value = -1) : value(value) {}
  Fd(const Fd&) = delete;
  ~Fd() { if (value >= 0) ::close(value); }
  explicit operator bool() const { return value >= 0; }

  int value;
};

Fd open_directory(int parent, const char* name, bool create) {
  if (create && ::mkdirat(parent, name, 0700) != 0 && errno != EEXIST) return Fd();
  return Fd(::openat(parent, name, directory_flags));
}

Fd snapshot_directory(const std::filesystem::path& cwd, bool create) {
  const Fd root(::open(cwd.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!root) return Fd();
  const auto state = open_directory(root.value, ".ycxx", create);
  if (!state) return Fd();
  return open_directory(state.value, "rules", create);
}

std::string hex(Fp128 fp) { return std::format("{:016x}{:016x}", fp.lo, fp.hi); }

std::optional<Fp128> decode_fp(std::string_view text) {
  if (text.size() != 32) return std::nullopt;
  Fp128 fp;
  const char* first = text.data();
  const auto lo = std::from_chars(first, first + 16, fp.lo, 16);
  const auto hi = std::from_chars(first + 16, first + 32, fp.hi, 16);
  if (lo.ec != std::errc{} || lo.ptr != first + 16 || hi.ec != std::errc{} || hi.ptr != first + 32)
    return std::nullopt;
  return fp;
}

std::string entry_name(std::string_view target) { return "target-" + hex(fingerprint128(target)) + ".json"; }

std::optional<std::string> read_text(int parent, const char* name) {
  const Fd file(::openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  struct stat info{};
  if (!file || ::fstat(file.value, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      static_cast<std::uintmax_t>(info.st_size) > max_snapshot_size)
    return std::nullopt;
  std::string contents;
  contents.reserve(static_cast<std::size_t>(info.st_size));
  std::array<char, 8192> buffer;
  for (;;) {
    const auto n = ::read(file.value, buffer.data(), buffer.size());
    if (n == 0) return contents;
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    const auto size = static_cast<std::size_t>(n);
    if (size > max_snapshot_size - contents.size()) return std::nullopt;
    contents.append(buffer.data(), size);
  }
}

bool write_text(int file, std::string_view contents) {
  while (!contents.empty()) {
    const auto n = ::write(file, contents.data(), contents.size());
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    contents.remove_prefix(static_cast<std::size_t>(n));
  }
  return ::fsync(file) == 0;
}

void store(int parent, const char* name, std::string_view text) {
  static std::atomic<std::uint64_t> sequence{0};
  for (int attempt = 0; attempt < 16; ++attempt) {
    const auto temporary = std::format(".rule-{}-{}", ::getpid(), sequence.fetch_add(1, std::memory_order_relaxed));
    const Fd file(::openat(parent, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (!file) {
      if (errno == EEXIST) continue;
      return;
    }
    if (write_text(file.value, text) && ::renameat(parent, temporary.c_str(), parent, name) == 0)
      (void)::fsync(parent);
    else
      (void)::unlinkat(parent, temporary.c_str(), 0);
    return;
  }
}

// The JSON codec handles aggregates and exact small integers. Use an explicit package class and
// hex for the full-width fingerprints.
struct StoredProbe {
  std::string package_target;
  bool standard_package;
  std::string profile;
  ToolchainProbeInputs inputs;
  std::string stamp;
  std::vector<PlanFact> facts;
  std::optional<std::uint8_t> package_kind = std::nullopt;
};

struct StoredRule {
  std::string target;
  std::string rule;
  std::string package_target;
  bool standard_package;
  std::string demand_definition;
  std::vector<RuleAttribute> attributes;
  std::vector<std::string> reads;
  std::string definition;
  std::string package;
  std::string invocation;
  std::optional<std::uint8_t> package_kind = std::nullopt;
  std::optional<std::vector<RuleCapabilityBinding>> capabilities = std::nullopt;
  std::optional<std::vector<StoredProbe>> projections = std::nullopt;
  std::optional<std::vector<PlanFact>> capability_facts = std::nullopt;
  std::optional<std::string> value_export = std::nullopt;
  std::optional<std::string> witness = std::nullopt;
};

struct SnapshotFile {
  int version;
  std::string target;
  std::vector<StoredRule> rules;
  std::optional<std::vector<StoredProbe>> probes;
};

using PackageKind = RulePackageRef::Kind;

std::optional<PackageKind> restore_kind(bool standard, std::optional<std::uint8_t> extended) {
  if (!extended) return standard ? PackageKind::standard : PackageKind::workspace;
  if (*extended == std::to_underlying(PackageKind::capabilities)) return PackageKind::capabilities;
  return std::nullopt;
}

std::optional<std::uint8_t> store_kind(PackageKind kind) {
  if (kind == PackageKind::capabilities) return std::to_underlying(kind);
  return std::nullopt;
}

std::optional<RuleProbeSnapshot> restore_probe(StoredProbe item) {
  const auto kind = restore_kind(item.standard_package, item.package_kind);
  const auto profile = decode_fp(item.profile);
  const auto stamp = decode_fp(item.stamp);
  if (!kind || !profile || !stamp) return std::nullopt;
  return RuleProbeSnapshot{{*kind, std::move(item.package_target)}, *profile,
                           std::move(item.inputs), *stamp, std::move(item.facts)};
}

StoredProbe store_probe(const RuleProbeSnapshot& probe) {
  return {probe.package.target, probe.package.kind == PackageKind::standard, hex(probe.profile),
          probe.inputs, hex(probe.stamp), probe.facts, store_kind(probe.package.kind)};
}

}  // namespace

std::optional<RuleSnapshot> read_rule_snapshot(const std::filesystem::path& cwd, std::string_view target) {
  const auto dir = snapshot_directory(cwd, false);
  if (!dir) return std::nullopt;
  const auto name = entry_name(target);
  const auto text = read_text(dir.value, name.c_str());
  if (!text) return std::nullopt;
  const auto json = lsp::parse(*text);
  if (!json) return std::nullopt;
  auto saved = lsp::from_json<SnapshotFile>(*json);
  if (!saved || saved->version != snapshot_version || saved->target != target) return std::nullopt;

  RuleSnapshot result;
  for (auto& item : saved->rules) {
    const auto kind = restore_kind(item.standard_package, item.package_kind);
    const auto package = decode_fp(item.package);
    const auto invocation = decode_fp(item.invocation);
    if (!kind || !package || !invocation || item.definition.empty()) return std::nullopt;
    RuleDemand demand{
        .target = std::move(item.target),
        .rule = std::move(item.rule),
        .package = {*kind, std::move(item.package_target)},
        .definition = std::move(item.demand_definition),
        .attributes = std::move(item.attributes),
        .reads = std::move(item.reads),
    };
    if (item.capabilities) demand.capabilities = std::move(*item.capabilities);
    demand.value_export = std::move(item.value_export);
    const auto witness = item.witness ? decode_fp(*item.witness) : std::nullopt;
    if (item.witness.has_value() != witness.has_value() ||
        demand.value_export.has_value() != witness.has_value()) return std::nullopt;
    std::vector<RuleProbeSnapshot> projections;
    if (item.projections)
      for (auto& projection : *item.projections) {
        auto restored = restore_probe(std::move(projection));
        if (!restored) return std::nullopt;
        projections.push_back(std::move(*restored));
      }
    const bool compiler_bindings = std::ranges::any_of(demand.capabilities, [](const auto& binding) {
      return !binding.value;
    });
    const auto facts = demand.capabilities.size() + (demand.value_export ? 1 : 0);
    if ((compiler_bindings && projections.empty()) ||
        (facts != 0 && (!item.capability_facts || item.capability_facts->size() != facts)))
      return std::nullopt;
    result.observations.push_back({demand, *package, *invocation, std::move(projections), witness});
    result.expansions.push_back({std::move(demand), std::move(item.definition),
                                 item.capability_facts ? std::move(*item.capability_facts) : std::vector<PlanFact>{}});
  }
  if (saved->probes)
    for (auto& item : *saved->probes)
      if (auto probe = restore_probe(std::move(item))) result.probes.push_back(std::move(*probe));
  return result;
}

void write_rule_snapshot(const std::filesystem::path& cwd, std::string_view target, const RuleSnapshot& snapshot) {
  if (snapshot.expansions.size() != snapshot.observations.size()) return;
  SnapshotFile saved{snapshot_version, std::string(target), {}, {}};
  for (std::size_t i = 0; i < snapshot.expansions.size(); ++i) {
    const auto& expansion = snapshot.expansions[i];
    const auto& observed = snapshot.observations[i];
    const auto& demand = expansion.demand;
    if (demand != observed.demand || expansion.definition.empty()) return;
    StoredRule rule{
        demand.target, demand.rule, demand.package.target, demand.package.kind == RulePackageRef::Kind::standard,
        demand.definition, demand.attributes, demand.reads, expansion.definition,
        hex(observed.package), hex(observed.invocation), store_kind(demand.package.kind)
    };
    if (!demand.capabilities.empty()) {
      rule.capabilities = demand.capabilities;
      auto& projections = rule.projections.emplace();
      for (const auto& probe : observed.projections) projections.push_back(store_probe(probe));
    }
    if (!expansion.capability_facts.empty()) rule.capability_facts = expansion.capability_facts;
    rule.value_export = demand.value_export;
    if (observed.witness) rule.witness = hex(*observed.witness);
    saved.rules.push_back(std::move(rule));
  }
  if (!snapshot.probes.empty()) {
    auto& probes = saved.probes.emplace();
    for (const auto& probe : snapshot.probes) probes.push_back(store_probe(probe));
  }
  const auto text = lsp::dump(lsp::to_json(saved));
  if (text.size() > max_snapshot_size) return;

  const auto dir = snapshot_directory(cwd, true);
  if (!dir) return;
  const auto name = entry_name(target);
  if (read_text(dir.value, name.c_str()) != text) store(dir.value, name.c_str(), text);
}

}  // namespace ycxx::build
