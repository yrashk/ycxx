// Pure argument staging and evidence normalization for toolchain probes. The caller runs
// the action and reports how it completed.
export module ycxx.build.toolchain_adapter;

import std;
import ycxx.build.action;
import ycxx.build.toolchain_probe;

export namespace ycxx::build {

struct ToolchainProbeProfile {
  std::string executable;
  std::string target;  // requested target for probes without an explicit target
  std::string observed_target;  // the driver's default as reported by -dumpmachine
  bool native = false;
  std::vector<std::string> flags;
  std::vector<std::pair<std::string, std::string>> environment;
};

struct StagedToolchainProbe {
  Action action;
  std::vector<ActionInput> inputs;
  std::string expected_target;
};

enum class ProbeSupport : std::uint8_t { supported, unsupported, indeterminate };
enum class ProbeCompletion : std::uint8_t {
  completed, rejected, setup_failure, output_failure, signalled, timed_out, cancelled,
};
enum class ProbeArtifactFormat : std::uint8_t { elf, mach_o, coff };
enum class ProbeArtifactKind : std::uint8_t { object, executable, shared_library };

struct ProbeArtifactEvidence {
  ProbeArtifactFormat format;
  ProbeArtifactKind kind;
  std::string architecture;
  bool host_runnable = false;
};

struct ProbeEvidence {
  ProbeSupport support = ProbeSupport::indeterminate;
  std::string reason;
  std::string target;
  std::optional<ProbeArtifactEvidence> artifact;
  std::optional<int> exit_status;
};

std::expected<ToolchainProbeProfile, std::string> normalize_toolchain_probe_profile(ToolchainProbeProfile profile);
bool toolchain_probe_host_matches(std::string_view target, std::string_view native_host);
std::expected<StagedToolchainProbe, std::string> stage_toolchain_probe(
    const ToolchainProbe& probe, const ToolchainProbeProfile& profile);
ProbeEvidence classify_toolchain_probe(const ToolchainProbe& probe, std::string_view expected_target,
                                       const ActionResult& result, ProbeCompletion completion);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

using enum ProbeArtifactFormat;
using enum ProbeArtifactKind;

constexpr std::string_view output_name = "artifact";

struct Target {
  std::string_view architecture;
  std::string_view platform;
  std::optional<ProbeArtifactFormat> format;
};

std::string_view architecture(std::string_view name) {
  if (name == "x86_64" || name == "amd64") return "x86_64";
  if (name == "x86" || (name.size() == 4 && name.front() == 'i' && name.ends_with("86"))) return "i386";
  if (name == "aarch64" || name == "arm64") return "aarch64";
  if (name == "aarch64_be") return name;
  if (name.starts_with("armeb") || name.starts_with("thumbeb")) return "armeb";
  if (name == "arm" || name.starts_with("armv") || name.starts_with("thumb")) return "arm";
  if (name == "powerpc" || name == "ppc") return "powerpc";
  if (name == "powerpc64" || name == "ppc64") return "powerpc64";
  if (name == "powerpc64le" || name == "ppc64le") return "powerpc64le";
  if (name == "riscv32" || name == "riscv64" || name == "s390x" || name == "loongarch64") return name;
  return {};
}

Target parse_target(std::string_view name) {
  Target target{architecture(name.substr(0, name.find('-'))), {}, std::nullopt};
  for (auto dash = name.find('-'); dash != name.npos && dash + 1 < name.size(); dash = name.find('-')) {
    name.remove_prefix(dash + 1);
    const auto part = name.substr(0, name.find('-'));
    if (part == "linux" || part == "none" || part.starts_with("freebsd") ||
        part.starts_with("netbsd") || part.starts_with("openbsd"))
      target.format = elf;
    else if (part.starts_with("darwin") || part.starts_with("macos") || part.starts_with("ios"))
      target.format = mach_o;
    else if (part == "windows" || part.starts_with("mingw"))
      target.format = coff;
    if (target.format) {
      target.platform = name;
      if (target.architecture == "x86_64" && name.ends_with("gnux32")) target.architecture = "x86_64-x32";
      break;
    }
  }
  return target;
}

bool same_architecture(const Target& requested, const Target& observed) {
  return !requested.architecture.empty() && requested.architecture == observed.architecture &&
         requested.format && requested.format == observed.format;
}

bool same_default_target(std::string_view requested, std::string_view observed) {
  if (requested == observed) return true;
  const auto want = parse_target(requested);
  const auto have = parse_target(observed);
  // Vendor spellings can differ even when the driver already defaults to this ABI.
  return same_architecture(want, have) && want.platform == have.platform;
}

bool darwin_host(std::string_view platform) {
  constexpr std::string_view names[] = {"darwin", "macosx", "macos"};
  const auto name = std::ranges::find_if(names, [&](auto prefix) { return platform.starts_with(prefix); });
  if (name == std::end(names)) return false;
  platform.remove_prefix(name->size());
  if (platform.empty()) return true;
  for (;;) {
    const auto dot = platform.find('.');
    const auto component = platform.substr(0, dot);
    if (component.empty() || !std::ranges::all_of(component, [](char c) { return '0' <= c && c <= '9'; }))
      return false;
    if (dot == platform.npos) return true;
    platform.remove_prefix(dot + 1);
  }
}

struct Bytes {
  std::span<const std::uint8_t> data;
  bool little = true;

  bool contains(std::uint64_t offset, std::uint64_t size) const {
    return offset <= data.size() && size <= data.size() - offset;
  }
  bool table(std::uint64_t offset, std::uint64_t count, std::uint64_t stride) const {
    // Probe artifacts are small. This also bounds inspection of malformed tables.
    return count != 0 && count <= 1024 && contains(offset, count * stride);
  }
  bool starts(std::initializer_list<std::uint8_t> prefix) const {
    return std::ranges::starts_with(data, prefix);
  }
  std::uint64_t read(std::uint64_t offset, unsigned width) const {
    if (!contains(offset, width)) return 0;
    std::uint64_t value = 0;
    for (unsigned byte = 0; byte < width; ++byte) {
      const auto shift = 8 * (little ? byte : width - 1 - byte);
      value |= std::uint64_t{data[offset + byte]} << shift;
    }
    return value;
  }
};

using ArtifactEvidence = std::expected<ProbeArtifactEvidence, std::string>;

std::string_view elf_architecture(std::uint64_t machine, bool wide, bool little) {
  switch (machine) {
    case 3: return wide ? "" : "i386";
    case 20: return wide ? "" : "powerpc";
    case 21: return !wide ? "" : little ? "powerpc64le" : "powerpc64";
    case 22: return wide ? "s390x" : "";
    case 40: return wide ? "" : little ? "arm" : "armeb";
    case 62: return !little ? "" : wide ? "x86_64" : "x86_64-x32";
    case 183: return !wide ? "" : little ? "aarch64" : "aarch64_be";
    case 243: return wide ? "riscv64" : "riscv32";
    case 258: return wide && little ? "loongarch64" : "";
    default: return {};
  }
}

ArtifactEvidence read_elf(Bytes bytes) {
  if (!bytes.contains(0, 16) || (bytes.data[4] != 1 && bytes.data[4] != 2) ||
      (bytes.data[5] != 1 && bytes.data[5] != 2) || bytes.data[6] != 1)
    return std::unexpected("invalid ELF identification");
  const bool wide = bytes.data[4] == 2;
  bytes.little = bytes.data[5] == 1;
  const unsigned word = wide ? 8 : 4;
  const unsigned header = wide ? 64 : 52;
  if (!bytes.contains(0, header) || bytes.read(header - 12, 2) != header || bytes.read(20, 4) != 1)
    return std::unexpected("invalid ELF header");
  const auto arch = elf_architecture(bytes.read(18, 2), wide, bytes.little);
  const bool linux_abi = bytes.data[7] == 0 || bytes.data[7] == 3;  // System V or Linux
  const auto type = bytes.read(16, 2);
  if (type == 1) {
    const auto offset = bytes.read(24 + 2 * word, word);
    const auto count = bytes.read(header - 4, 2);
    const unsigned stride = wide ? 64 : 40;
    if (offset < header || bytes.read(header - 6, 2) != stride ||
        count < 2 || !bytes.table(offset, count, stride))
      return std::unexpected("invalid ELF object section table");
    for (std::uint64_t i = 1; i < count; ++i) {
      const auto record = offset + i * stride;
      if (bytes.read(record + 4, 4) == 8) continue;  // SHT_NOBITS
      if (!bytes.contains(bytes.read(record + (wide ? 24 : 16), word),
                          bytes.read(record + (wide ? 32 : 20), word)))
        return std::unexpected("invalid ELF object section extent");
    }
    return ProbeArtifactEvidence{elf, object, std::string(arch)};
  }
  if (type != 2 && type != 3) return std::unexpected("unrecognized ELF artifact type");

  const auto offset = bytes.read(24 + word, word);
  const auto count = bytes.read(header - 8, 2);
  const unsigned stride = wide ? 56 : 32;
  if (offset < header || bytes.read(header - 10, 2) != stride || !bytes.table(offset, count, stride))
    return std::unexpected("invalid ELF program header table");

  bool loadable = false;
  bool dynamic = false;
  bool pie = false;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto record = offset + i * stride;
    const auto segment = bytes.read(record, 4);
    const auto start = bytes.read(record + (wide ? 8 : 4), word);
    const auto length = bytes.read(record + (wide ? 32 : 16), word);
    if (length != 0 && !bytes.contains(start, length)) return std::unexpected("invalid ELF segment extent");
    if (segment == 1) loadable = true;
    if (segment == 3) {
      if (length <= 1 || bytes.read(start + length - 1, 1) != 0)
        return std::unexpected("invalid ELF interpreter segment");
      pie = true;
    }
    if (segment != 2) continue;

    if (dynamic) return std::unexpected("duplicate ELF dynamic table");
    dynamic = true;
    const auto entries = length / (2 * word);
    if (length % (2 * word) != 0 || !bytes.table(start, entries, 2 * word))
      return std::unexpected("invalid ELF dynamic table");
    for (std::uint64_t j = 0; j < entries; ++j) {
      const auto entry = start + j * 2 * word;
      const auto tag = bytes.read(entry, word);
      if (tag == 0) break;
      // DT_FLAGS_1 / DF_1_PIE distinguish a static PIE from a shared library too.
      if (tag == 0x6ffffffb && (bytes.read(entry + word, word) & 0x08000000) != 0) pie = true;
    }
  }
  if (!loadable) return std::unexpected("ELF image has no loadable segment");
  if (type == 2 || pie) return ProbeArtifactEvidence{elf, executable, std::string(arch), linux_abi};
  if (!dynamic || bytes.read(24, word) != 0) return std::unexpected("ambiguous ELF dynamic image type");
  return ProbeArtifactEvidence{elf, shared_library, std::string(arch), linux_abi};
}

std::string_view macho_architecture(std::uint64_t cpu) {
  switch (cpu) {
    case 7: return "i386";
    case 12: return "arm";
    case 18: return "powerpc";
    case 0x01000007: return "x86_64";
    case 0x0100000c: return "aarch64";
    case 0x01000012: return "powerpc64";
    default: return {};
  }
}

ArtifactEvidence read_macho(Bytes bytes, bool wide) {
  const unsigned header = wide ? 32 : 28;
  if (!bytes.contains(0, header)) return std::unexpected("invalid Mach-O header");
  const auto commands = bytes.read(16, 4);
  const auto length = bytes.read(20, 4);
  if (commands == 0 || commands > 1024 || !bytes.contains(header, length))
    return std::unexpected("invalid Mach-O command table");
  const auto end = header + length;
  std::uint64_t offset = header;
  std::optional<bool> macos;
  for (std::uint64_t i = 0; i < commands; ++i) {
    if (end - offset < 8) return std::unexpected("truncated Mach-O load command");
    const auto command = bytes.read(offset, 4);
    const auto size = bytes.read(offset + 4, 4);
    if (size < 8 || size > end - offset) return std::unexpected("invalid Mach-O load command");
    std::optional<bool> platform;
    if (command == 0x32) {  // LC_BUILD_VERSION
      if (size < 24) return std::unexpected("invalid Mach-O build version");
      platform = bytes.read(offset + 8, 4) == 1;  // PLATFORM_MACOS
    } else if (command == 0x24 || command == 0x25 || command == 0x2f || command == 0x30) {
      if (size < 16) return std::unexpected("invalid Mach-O minimum OS version");
      platform = command == 0x24;  // LC_VERSION_MIN_MACOSX, alongside the other legacy OS commands.
    }
    if (platform) {
      if (macos && macos != platform) return std::unexpected("conflicting Mach-O build platforms");
      macos = platform;
    }
    offset += size;
  }
  if (offset != end) return std::unexpected("inconsistent Mach-O command table");

  ProbeArtifactKind kind;
  switch (bytes.read(12, 4)) {
    case 1: kind = object; break;
    case 2: kind = executable; break;
    case 6: kind = shared_library; break;
    default: return std::unexpected("unrecognized Mach-O artifact type");
  }
  const auto cpu = bytes.read(4, 4);
  if (((cpu & 0x01000000) != 0) != wide) return std::unexpected("inconsistent Mach-O processor width");
  return ProbeArtifactEvidence{mach_o, kind, std::string(macho_architecture(cpu)), macos.value_or(false)};
}

std::string_view coff_architecture(std::uint64_t machine) {
  switch (machine) {
    case 0x014c: return "i386";
    case 0x01c0:
    case 0x01c4: return "arm";
    case 0x5032: return "riscv32";
    case 0x5064: return "riscv64";
    case 0x8664: return "x86_64";
    case 0xaa64: return "aarch64";
    default: return {};
  }
}

ArtifactEvidence read_coff(Bytes bytes, bool image) {
  std::uint64_t offset = 0;
  if (image) {
    const auto pe = bytes.read(60, 4);
    if (pe < 64 || !bytes.contains(pe, 4) || bytes.read(pe, 4) != 0x00004550)
      return std::unexpected("invalid PE header");
    offset = pe + 4;
  }
  if (!bytes.contains(offset, 20)) return std::unexpected("invalid COFF header");
  const auto sections = bytes.read(offset + 2, 2);
  const auto optional = bytes.read(offset + 16, 2);
  const auto flags = bytes.read(offset + 18, 2);
  const auto arch = coff_architecture(bytes.read(offset, 2));
  const auto table = offset + 20 + optional;
  if (!bytes.table(table, sections, 40)) return std::unexpected("invalid COFF section table");
  for (std::uint64_t i = 0; i < sections; ++i) {
    const auto record = table + i * 40;
    const auto data = bytes.read(record + 20, 4);
    if (data != 0 && !bytes.contains(data, bytes.read(record + 16, 4)))
      return std::unexpected("invalid COFF section extent");
  }
  if (!image) {
    if (optional != 0 || (flags & 0x2002) != 0) return std::unexpected("invalid COFF object header");
    return ProbeArtifactEvidence{coff, object, std::string(arch)};
  }
  const auto magic = bytes.read(offset + 20, 2);
  if ((flags & 2) == 0 || !((magic == 0x10b && optional >= 96) || (magic == 0x20b && optional >= 112)))
    return std::unexpected("invalid PE optional header");
  const bool wide = arch == "x86_64" || arch == "aarch64" || arch == "riscv64";
  if (!arch.empty() && (magic == 0x20b) != wide) return std::unexpected("inconsistent PE processor width");
  return ProbeArtifactEvidence{coff, (flags & 0x2000) != 0 ? shared_library : executable, std::string(arch)};
}

ArtifactEvidence read_artifact(std::span<const std::uint8_t> data) {
  Bytes bytes{data};
  if (bytes.starts({0x7f, 'E', 'L', 'F'})) return read_elf(bytes);
  const auto magic = bytes.read(0, 4);
  if (magic == 0xfeedface || magic == 0xfeedfacf) return read_macho(bytes, magic == 0xfeedfacf);
  if (magic == 0xcefaedfe || magic == 0xcffaedfe) {
    bytes.little = false;
    return read_macho(bytes, magic == 0xcffaedfe);
  }
  if (magic == 0xcafebabe || magic == 0xcafebabf || magic == 0xbebafeca || magic == 0xbfbafeca)
    return std::unexpected("universal Mach-O output has no single target");
  if (bytes.starts({'!', '<', 'a', 'r', 'c', 'h', '>', '\n'}) ||
      bytes.starts({'!', '<', 't', 'h', 'i', 'n', '>', '\n'}))
    return std::unexpected("compiler output is an archive");
  if (bytes.starts({'M', 'Z'})) return read_coff(bytes, true);
  if (!coff_architecture(bytes.read(0, 2)).empty()) return read_coff(bytes, false);
  return std::unexpected("unrecognized compiler output format");
}

bool target_token(std::string_view token) {
  return token.size() <= 256 && token.contains('-') && !token.starts_with('-') && !token.ends_with('-') &&
         std::ranges::all_of(token, [](const char c) {
           return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
         });
}

bool dialect_flag(std::string_view flag) {
  return flag.starts_with("-std=") || flag.starts_with("--std=") || flag == "-ansi";
}

bool controls_probe_phase(std::string_view flag) {
  return flag == "--" || flag.starts_with("-x") || flag == "-E" || flag == "-S" || flag == "-c" ||
         flag == "-fsyntax-only" || flag == "-shared" || flag == "-dynamiclib" ||
         flag == "-o" || flag == "--output" || flag.starts_with("--output=") ||
         flag == "-M" || flag == "-MM" || flag == "-###" || flag == "-fdriver-only";
}

std::string_view trim(std::string_view value) {
  constexpr std::string_view space = " \t\r\n";
  const auto first = value.find_first_not_of(space);
  return first == value.npos ? std::string_view{} : value.substr(first, value.find_last_not_of(space) - first + 1);
}

}  // namespace

std::expected<ToolchainProbeProfile, std::string> normalize_toolchain_probe_profile(ToolchainProbeProfile profile) {
  if (!profile.target.empty() && !target_token(profile.target))
    return std::unexpected("invalid probe target triple");
  auto flags = std::exchange(profile.flags, {});
  for (std::size_t i = 0; i < flags.size(); ++i) {
    const std::string_view flag = flags[i];
    std::string_view target;
    if (flag.starts_with("--target=")) {
      target = flag.substr(std::string_view("--target=").size());
    } else if (flag == "--target" || flag == "-target") {
      if (++i == flags.size()) return std::unexpected("missing target triple in probe flags");
      target = flags[i];
    } else {
      if (flag.starts_with("-target="))
        return std::unexpected("use --target=<triple> or -target <triple> in probe flags");
      if (controls_probe_phase(flag))
        return std::unexpected(std::format("probe flag {:?} selects the source language, phase, or output", flag));
      profile.flags.push_back(std::move(flags[i]));
      continue;
    }
    if (!target_token(target)) return std::unexpected("invalid probe target triple");
    if (!profile.target.empty() && profile.target != target)
      return std::unexpected("conflicting targets in probe profile");
    profile.target = target;
  }
  return profile;
}

bool toolchain_probe_host_matches(std::string_view target, std::string_view native_host) {
  if (!target_token(target) || !target_token(native_host)) return false;
  const auto compiled = parse_target(target);
  const auto host = parse_target(native_host);
  if (!same_architecture(compiled, host)) return false;
  if (host.format == mach_o) return darwin_host(compiled.platform) && darwin_host(host.platform);
  return compiled.platform == host.platform;
}

std::expected<StagedToolchainProbe, std::string> stage_toolchain_probe(
    const ToolchainProbe& probe, const ToolchainProbeProfile& profile) {
  if (profile.executable.contains('\0') || !std::filesystem::path(profile.executable).is_absolute())
    return std::unexpected("probe compiler must be an absolute executable path");

  StagedToolchainProbe staged;
  staged.action.tool = profile.executable;
  staged.action.environment = profile.environment;
  const auto literal = [&](std::string_view arg) {
    staged.action.args.push_back({ActionArg::Kind::literal, std::string(arg)});
  };
  if (profile.native) {
    literal("cxx");
    literal("--no-daemon");
  }

  const bool driver = probe.phase == ToolchainProbePhase::driver_query;
  const bool exact_dialect = probe.id.starts_with("dialect.");
  const bool profile_dialect = !exact_dialect && std::ranges::any_of(profile.flags, dialect_flag);
  const std::string_view selected = probe.target.empty() ? std::string_view(profile.target) : probe.target;
  const auto target = selected.empty() ? std::string_view(profile.observed_target) : selected;
  if (!driver) {
    if (!target.empty() && !target_token(target)) return std::unexpected("invalid probe target triple");
    staged.expected_target = target;
  }

  const bool darwin_executable = probe.phase == ToolchainProbePhase::executable_link && parse_target(target).format == mach_o;
  const auto native_runtime_off = darwin_executable ? "-nostdlib++" : "-nostdlib";
  bool recipe_runtime_off = false;
  for (auto arg : probe.args) {
    if (!driver && dialect_flag(arg)) {
      if (profile_dialect) continue;
      if (profile.native && !exact_dialect) arg = "-std=c++26";
    }
    if (arg == "-nostdlib") {
      recipe_runtime_off = true;
      if (profile.native && probe.native_leaf_link) arg = native_runtime_off;
    }
    literal(arg);
  }
  if (!driver) {
    for (const auto& flag : profile.flags)
      if (!exact_dialect || !dialect_flag(flag)) literal(flag);
    if (!selected.empty() && !same_default_target(selected, profile.observed_target))
      literal(std::format("--target={}", selected));
  }
  if (profile.native && probe.native_leaf_link && !recipe_runtime_off) literal(native_runtime_off);

  if (!probe.source.empty()) {
    staged.inputs.push_back({"source", "probe.cc", {{probe.source.begin(), probe.source.end()}, false}});
    staged.action.args.push_back({ActionArg::Kind::input, "source"});
  }
  if (!probe.output.empty()) {
    staged.action.outputs.push_back({std::string(output_name), std::string(probe.output)});
    literal("-o");
    staged.action.args.push_back({ActionArg::Kind::output, std::string(output_name)});
  }
  return staged;
}

ProbeEvidence classify_toolchain_probe(const ToolchainProbe& probe, std::string_view expected_target,
                                       const ActionResult& result, ProbeCompletion completion) {
  ProbeEvidence evidence{.reason = {}, .target = std::string(expected_target), .artifact = {},
                         .exit_status = result.exit_status};
  switch (completion) {
    case ProbeCompletion::rejected:
      if (result.exit_status != 0) {
        evidence.support = ProbeSupport::unsupported;
        evidence.reason = "compiler rejected the probe";
        return evidence;
      }
      evidence.reason = "rejected probe reported a successful exit";
      return evidence;
    case ProbeCompletion::setup_failure:
      evidence.reason = "probe could not be set up";
      return evidence;
    case ProbeCompletion::output_failure:
      evidence.reason = "probe output could not be collected";
      return evidence;
    case ProbeCompletion::signalled:
      evidence.reason = "probe process terminated by a signal";
      return evidence;
    case ProbeCompletion::timed_out:
      evidence.reason = "probe timed out";
      return evidence;
    case ProbeCompletion::cancelled:
      evidence.reason = "probe cancelled";
      return evidence;
    case ProbeCompletion::completed: break;
  }
  if (result.exit_status != 0) {
    evidence.reason = "compiler exited without a conclusive probe result";
    return evidence;
  }
  if (probe.phase == ToolchainProbePhase::driver_query) {
    const auto token = trim(result.out);
    if (!target_token(token)) {
      evidence.reason = "compiler did not report a single target triple";
      return evidence;
    }
    evidence.target = token;
  }
  if (!probe.output.empty()) {
    if (result.outputs.size() != 1 || result.outputs.front().name != output_name) {
      evidence.reason = "compiler did not produce the declared probe artifact";
      return evidence;
    }
    auto artifact = read_artifact(result.outputs.front().contents.bytes);
    if (!artifact) {
      evidence.reason = std::move(artifact.error());
      return evidence;
    }
    evidence.artifact = std::move(*artifact);
    const auto target = parse_target(expected_target);
    if (target.architecture.empty() || !target.format || evidence.artifact->architecture.empty()) {
      evidence.reason = "probe artifact target could not be verified";
      return evidence;
    }
    if (target.architecture != evidence.artifact->architecture || *target.format != evidence.artifact->format) {
      evidence.reason = "probe artifact does not match the requested target";
      return evidence;
    }
    const auto kind = probe.phase == ToolchainProbePhase::object ? object :
                      probe.phase == ToolchainProbePhase::shared_link ? shared_library : executable;
    if (evidence.artifact->kind != kind) {
      evidence.reason = "compiler produced a different artifact kind from the probe";
      return evidence;
    }
  }
  evidence.support = ProbeSupport::supported;
  evidence.reason = "compiler completed the probe";
  return evidence;
}

}  // namespace ycxx::build
