// Persist admitted rule expansions for a read-only inspector (#1281).
module;
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

export module ycxx.build.rule_snapshot;

import std;
import ycxx.build.rule_protocol;
import ycxx.hash;
import ycxx.lsp;

export namespace ycxx::build {

struct RuleObservation {
  RuleDemand demand;
  Fp128 package;
  Fp128 invocation;
};

struct RuleSnapshot {
  std::vector<RuleExpansion> expansions;
  std::vector<RuleObservation> observations;  // in expansion order
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
};

struct SnapshotFile {
  int version;
  std::string target;
  std::vector<StoredRule> rules;
};

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
  using Kind = RulePackageRef::Kind;
  for (auto& item : saved->rules) {
    const auto package = decode_fp(item.package);
    const auto invocation = decode_fp(item.invocation);
    if (!package || !invocation || item.definition.empty()) return std::nullopt;
    RuleDemand demand{
        .target = std::move(item.target),
        .rule = std::move(item.rule),
        .package = {item.standard_package ? Kind::standard : Kind::workspace, std::move(item.package_target)},
        .definition = std::move(item.demand_definition),
        .attributes = std::move(item.attributes),
        .reads = std::move(item.reads),
    };
    result.observations.push_back({demand, *package, *invocation});
    result.expansions.push_back({std::move(demand), std::move(item.definition)});
  }
  return result;
}

void write_rule_snapshot(const std::filesystem::path& cwd, std::string_view target, const RuleSnapshot& snapshot) {
  if (snapshot.expansions.size() != snapshot.observations.size()) return;
  SnapshotFile saved{snapshot_version, std::string(target), {}};
  for (std::size_t i = 0; i < snapshot.expansions.size(); ++i) {
    const auto& expansion = snapshot.expansions[i];
    const auto& observed = snapshot.observations[i];
    const auto& demand = expansion.demand;
    if (demand != observed.demand || expansion.definition.empty()) return;
    saved.rules.push_back({
        demand.target, demand.rule, demand.package.target, demand.package.kind == RulePackageRef::Kind::standard,
        demand.definition, demand.attributes, demand.reads, expansion.definition,
        hex(observed.package), hex(observed.invocation),
    });
  }
  const auto text = lsp::dump(lsp::to_json(saved));
  if (text.size() > max_snapshot_size) return;

  const auto dir = snapshot_directory(cwd, true);
  if (!dir) return;
  const auto name = entry_name(target);
  if (read_text(dir.value, name.c_str()) != text) store(dir.value, name.c_str(), text);
}

}  // namespace ycxx::build
