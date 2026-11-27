// Read the load commands and export trie of a leaf dylib (Apple's Mach-O format).
module ycxx.link;

import std;
import ycxx.obj.macho;

namespace ycxx::link {
namespace {

namespace mo = obj::macho;
using bytes_view = std::span<const std::uint8_t>;

struct extent {
  std::uint64_t start, size;
  bool contains(extent inner) const {
    return inner.start >= start && inner.start - start <= size && inner.size <= size - (inner.start - start);
  }
  bool operator==(const extent&) const = default;
};

bool disjoint(std::vector<extent>& extents) {
  std::ranges::sort(extents, {}, &extent::start);
  std::uint64_t end = 0;
  for (const auto part : extents) {
    if (part.size == 0) continue;
    if (part.start < end) return false;
    end = part.start + part.size;  // checked on input
  }
  return true;
}

std::string_view chars(bytes_view bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string_view fixed_name(bytes_view bytes) {
  const auto name = chars(bytes);
  return name.substr(0, name.find('\0'));
}

std::optional<std::string_view> command_name(bytes_view command) {
  const auto offset = mo::load32(command, 8);
  if (offset < 24 || offset >= command.size()) return {};
  const auto name = chars(command.subspan(offset));
  const auto end = name.find('\0');
  if (end == 0 || end == name.npos) return {};
  return name.substr(0, end);
}

bool supported_id(std::string_view name) {
  if (name.starts_with('/')) return name.size() > 1;
  if (name.starts_with('@')) return name.starts_with("@loader_path/") && name.size() > sizeof("@loader_path/") - 1;
  return !name.contains('/') && name != "." && name != "..";
}

struct mappings {
  std::optional<std::uint64_t> base;
  std::optional<extent> linkedit;
  std::vector<extent> memory, disk, sections, code;
};

bool read_segment(bytes_view command, mappings& maps, extent file, std::uint64_t header_end) {
  if (command.size() < mo::segment_command_64_size) return false;
  const auto nsects = mo::load32(command, 64);
  if (command.size() != mo::segment_command_64_size + std::uint64_t{nsects} * mo::section_64_size) return false;
  const auto name = fixed_name(command.subspan(8, 16));
  const extent vm{mo::load64(command, 24), mo::load64(command, 32)};
  const extent disk{mo::load64(command, 40), mo::load64(command, 48)};
  const auto prot = mo::load32(command, 60);
  if (vm.size > std::numeric_limits<std::uint64_t>::max() - vm.start || !file.contains(disk) ||
      disk.size > vm.size || (prot & ~mo::load32(command, 56))) return false;
  maps.memory.push_back(vm);
  maps.disk.push_back(disk);
  if (name == "__TEXT") {
    if (maps.base || !disk.contains({0, header_end}) || !(prot & mo::vm_prot_execute)) return false;
    maps.base = vm.start;
  } else if (name == "__LINKEDIT") {
    if (maps.linkedit || nsects != 0 || disk.start < header_end) return false;
    maps.linkedit = disk;
  }
  for (std::size_t i = 0; i < nsects; ++i) {
    const auto section = command.subspan(mo::segment_command_64_size + i * mo::section_64_size, mo::section_64_size);
    if (fixed_name(section.subspan(16, 16)) != name) return false;
    const extent address{mo::load64(section, 32), mo::load64(section, 40)};
    const auto flags = mo::load32(section, 64);
    if (!vm.contains(address)) return false;
    maps.sections.push_back(address);
    if (obj::section_flags::is_zerofill(flags)) continue;
    const extent on_disk{mo::load32(section, 48), address.size};
    if (!disk.contains(on_disk) || on_disk.start - disk.start != address.start - vm.start ||
        (address.size && on_disk.start < header_end)) return false;
    if (address.size >= 4 && (prot & mo::vm_prot_execute) && (flags & obj::section_flags::pure_instructions) &&
        (flags & (obj::section_flags::type_mask | obj::section_flags::debug)) == obj::section_flags::regular)
      maps.code.push_back(address);
  }
  return true;
}

std::optional<std::uint64_t> uleb(bytes_view& input) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64 && !input.empty(); shift += 7) {
    const auto byte = input.front();
    input = input.subspan(1);
    if (shift == 63 && byte > 1) return {};
    value |= std::uint64_t(byte & 127) << shift;
    if (byte < 128) return value;
  }
  return {};
}

// Walk a compressed radix tree without recursion or copying the prefix into every pending
// child. Each node owns its bytes. Bound the total materialized names by the input image size.
std::optional<std::string_view> read_exports(bytes_view trie, std::size_t name_budget, const mappings& maps,
                                            std::set<std::string, std::less<>>& names) {
  constexpr std::string_view malformed = "invalid Mach-O dylib export trie";
  struct branch {
    std::size_t offset, prefix_size;
    std::string_view label;
  };
  std::vector<branch> pending{{0, 0, {}}};
  std::vector<bool> occupied(trie.size());
  std::string name;
  while (!pending.empty()) {
    const auto edge = pending.back();
    pending.pop_back();
    if (edge.offset >= trie.size() || occupied[edge.offset]) return malformed;
    name.resize(edge.prefix_size);
    name += edge.label;
    auto cursor = trie.subspan(edge.offset);
    const auto length = uleb(cursor);
    if (!length || *length > cursor.size()) return malformed;
    auto terminal = cursor.first(*length);
    cursor = cursor.subspan(*length);
    if (!terminal.empty()) {
      const auto flags = uleb(terminal);
      if (!flags || name.empty()) return malformed;
      if (*flags != 0) return "unsupported Mach-O dylib export flags";
      const auto offset = uleb(terminal);
      if (!offset || !terminal.empty() || *offset > std::numeric_limits<std::uint64_t>::max() - *maps.base)
        return malformed;
      const auto address = *maps.base + *offset;
      const auto next = std::ranges::upper_bound(maps.code, address, {}, &extent::start);
      if (address % 4 || next == maps.code.begin() || !std::prev(next)->contains({address, 4}))
        return "unsupported Mach-O dylib export outside executable instructions";
      if (name.size() >= name_budget || !names.insert(name).second) return malformed;
      name_budget -= name.size() + 1;
    }
    if (cursor.empty()) return malformed;
    const auto children = cursor.front();
    cursor = cursor.subspan(1);
    std::set<std::string_view> labels;
    for (unsigned i = 0; i < children; ++i) {
      const auto rest = chars(cursor);
      const auto end = rest.find('\0');
      if (end == 0 || end == rest.npos) return malformed;
      const auto label = rest.substr(0, end);
      const auto [it, inserted] = labels.insert(label);
      if (!inserted || (it != labels.begin() && label.starts_with(*std::prev(it))) ||
          (std::next(it) != labels.end() && std::next(it)->starts_with(label))) return malformed;
      cursor = cursor.subspan(end + 1);
      const auto child = uleb(cursor);
      if (!child || *child >= trie.size()) return malformed;
      pending.push_back({static_cast<std::size_t>(*child), name.size(), label});
    }
    for (auto i = edge.offset; i < trie.size() - cursor.size(); ++i) {
      if (occupied[i]) return malformed;
      occupied[i] = true;
    }
  }
  return {};
}

}  // namespace

std::expected<dylib, std::string> read_dylib(std::span<const std::uint8_t> bytes, std::string_view path) {
  const auto error = [&](std::string_view reason) { return std::unexpected(std::format("{}: {}", path, reason)); };
  if (bytes.size() < mo::mach_header_64_size || mo::load32(bytes, 0) != mo::mh_magic_64 ||
      mo::load32(bytes, 4) != mo::cpu_type_arm64 || mo::load32(bytes, 8) != mo::cpu_subtype_arm64_all ||
      mo::load32(bytes, 12) != mo::mh_dylib) return error("expected a thin arm64 Mach-O dylib");
  const auto count = mo::load32(bytes, 16), size = mo::load32(bytes, 20);
  if (size > bytes.size() - mo::mach_header_64_size || count > size / 8) return error("invalid Mach-O dylib load commands");
  auto commands = bytes.subspan(mo::mach_header_64_size, size);
  dylib result;
  result.exports.emplace();
  mappings maps;
  std::optional<extent> exports;
  std::vector<extent> linkedit_data;
  bool platform = false, system = false, uuid = false, modern = false, legacy = false;
  const extent file{0, bytes.size()};
  for (unsigned i = 0; i < count; ++i) {
    if (commands.size() < 8) return error("invalid Mach-O dylib load commands");
    const auto kind = mo::load32(commands, 0), length = mo::load32(commands, 4);
    if (length < 8 || length % 8 != 0 || length > commands.size()) return error("invalid Mach-O dylib load commands");
    const auto command = commands.first(length);
    commands = commands.subspan(length);
    switch (kind) {
    case mo::lc_segment_64:
      if (!read_segment(command, maps, file, mo::mach_header_64_size + size)) return error("invalid Mach-O dylib segment");
      break;
    case mo::lc_id_dylib:
    case mo::lc_load_dylib: {
      const auto name = length >= 24 ? command_name(command) : std::nullopt;
      if (kind == mo::lc_id_dylib) {
        if (!name || !result.install_name.empty()) return error("invalid or duplicate Mach-O dylib install name");
        if (!supported_id(*name)) return error("unsupported Mach-O dylib install name");
        result.install_name = *name;
        result.current_version = mo::load32(command, 16);
        result.compatibility_version = mo::load32(command, 20);
      } else {
        if (!name || *name != "/usr/lib/libSystem.B.dylib" || std::exchange(system, true))
          return error("unsupported Mach-O dylib dependency");
      }
      break;
    }
    case 0x18 | mo::lc_req_dyld:  // LC_LOAD_WEAK_DYLIB
    case 0x1f | mo::lc_req_dyld:  // LC_REEXPORT_DYLIB
    case 0x23 | mo::lc_req_dyld:  // LC_LOAD_UPWARD_DYLIB
    case 0x20:                    // LC_LAZY_LOAD_DYLIB
    case 0x12: case 0x13: case 0x14: case 0x15:  // sub-framework, umbrella, client, library
      return error("unsupported Mach-O dylib dependency");
    case mo::lc_build_version:
      if (length < 24 || length != 24 + std::uint64_t{mo::load32(command, 20)} * 8 ||
          mo::load32(command, 8) != mo::platform_macos || std::exchange(platform, true))
        return error("expected macOS platform in Mach-O dylib");
      break;
    case 0x24:  // LC_VERSION_MIN_MACOSX
      if (length != 16 || std::exchange(platform, true)) return error("invalid Mach-O dylib platform");
      break;
    case 0x25: case 0x2f: case 0x30:  // iOS, tvOS, watchOS
      return error("expected macOS platform in Mach-O dylib");
    case mo::lc_uuid:
      if (length != 24 || std::exchange(uuid, true)) return error("invalid Mach-O dylib UUID command");
      break;
    case mo::lc_dyld_chained_fixups: case mo::lc_code_signature:
    case 0x26: case 0x29:  // LC_FUNCTION_STARTS, LC_DATA_IN_CODE
      if (length != 16) return error("invalid Mach-O dylib linkedit command");
      if (const auto n = mo::load32(command, 12)) linkedit_data.push_back({mo::load32(command, 8), n});
      break;
    case mo::lc_symtab:
      if (length != 24) return error("invalid Mach-O dylib symbol table command");
      if (const auto n = mo::load32(command, 12))
        linkedit_data.push_back({mo::load32(command, 8), std::uint64_t{n} * mo::nlist_64_size});
      if (const auto n = mo::load32(command, 20)) linkedit_data.push_back({mo::load32(command, 16), n});
      break;
    case mo::lc_dysymtab:
      if (length != 80) return error("invalid Mach-O dylib dynamic symbol table command");
      break;
    case 0x1c | mo::lc_req_dyld: {  // LC_RPATH in an input does not alter the consumer's search paths
      if (length < 16) return error("invalid Mach-O dylib rpath command");
      const auto offset = mo::load32(command, 8);
      if (offset < 12 || offset >= length || !command[offset] || !chars(command.subspan(offset)).contains('\0'))
        return error("invalid Mach-O dylib rpath command");
      break;
    }
    case mo::lc_dyld_exports_trie:
    case 0x22: case 0x22 | mo::lc_req_dyld: {  // LC_DYLD_INFO[_ONLY]
      const bool standalone = kind == mo::lc_dyld_exports_trie;
      if (length != (standalone ? 16u : 48u) || std::exchange(standalone ? modern : legacy, true))
        return error("invalid Mach-O dylib export trie command");
      if (!standalone)
        for (int i = 8; i < 40; i += 8)
          if (const auto n = mo::load32(command, i + 4)) linkedit_data.push_back({mo::load32(command, i), n});
      const auto off = standalone ? 8 : 40;
      const extent range{mo::load32(command, off), mo::load32(command, off + 4)};
      if (!range.size && !standalone) break;
      if (!range.size || !file.contains(range) || (exports && *exports != range))
        return error("invalid Mach-O dylib export trie range");
      exports = range;
      break;
    }
    default:
      if (kind & mo::lc_req_dyld) return error("unsupported required Mach-O dylib load command");
      break;  // unrelated load commands do not supply exports
    }
  }
  if (!commands.empty()) return error("invalid Mach-O dylib load commands");
  if (result.install_name.empty()) return error("missing Mach-O dylib install name");
  if (!system) return error("missing Mach-O dylib dependency on libSystem");
  if (!platform) return error("expected macOS platform in Mach-O dylib");
  if (!uuid) return error("missing Mach-O dylib UUID command");
  if (!maps.base || !maps.linkedit || !disjoint(maps.disk) || !disjoint(maps.memory) || !disjoint(maps.sections))
    return error("invalid Mach-O dylib segment layout");
  for (const auto range : linkedit_data)
    if (!maps.linkedit->contains(range)) return error("invalid Mach-O dylib linkedit range");
  if (!exports || !maps.linkedit->contains(*exports)) return error("invalid Mach-O dylib export trie range");
  std::ranges::sort(maps.code, {}, &extent::start);
  if (const auto failure = read_exports(bytes.subspan(exports->start, exports->size), bytes.size(), maps, *result.exports))
    return error(*failure);
  return result;
}

}  // namespace ycxx::link
