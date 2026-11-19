// Linux x86-64 static executables, from the ELF gABI and the System V AMD64 psABI.
export module ycxx.link.elf;

export import ycxx.obj;
import std;

export namespace ycxx::link::elf {

// Link in-memory x86-64 Linux objects. The caller supplies the entry definition in code and
// handles process startup and exit. Only the standard sections and ABS64, PC32 and PLT32
// relocations are supported. The ET_EXEC image has load segments and a non-executable stack.
// It has no section table or dynamic loader. `entry` names a nonlocal symbol.
std::expected<std::vector<std::uint8_t>, std::string> link(std::span<const obj::object> objects,
                                                        std::string_view entry = "_start");

}  // namespace ycxx::link::elf

namespace ycxx::link::elf {
namespace {

using bytes = std::vector<std::uint8_t>;
using obj::binding;
using obj::reloc_kind;

constexpr std::uint16_t et_exec = 2, em_x86_64 = 62;
constexpr std::uint16_t header_size = 64, program_header_size = 56;
constexpr std::uint32_t pt_load = 1, pt_gnu_stack = 0x6474e551;
constexpr std::uint32_t pf_x = 1, pf_w = 2, pf_r = 4;
constexpr std::uint64_t image_base = 0x400000, page_size = 0x1000;
// Keep the image below the 4-level x86-64 user address limit and keep file offsets representable.
constexpr std::uint64_t image_size_limit = (std::uint64_t{1} << 47) - image_base;
constexpr auto max_align_log2 = std::countr_zero(image_base);

enum class group { text, read_only, data, init_array, bss, count };

struct section_kind {
  std::string_view segment, name;
  std::uint32_t flags;
  group output;
};

using namespace obj::section_flags;
constexpr auto instruction_flags = pure_instructions | some_instructions;
constexpr std::array section_kinds{
    section_kind{"__TEXT", "__text", regular, group::text},
    section_kind{"__TEXT", "__cstring", cstring_literals, group::read_only},
    section_kind{"__TEXT", "__const", regular, group::read_only},
    section_kind{"__DATA", "__const", regular, group::read_only},  // pointers are fixed at link time
    section_kind{"__DATA", "__data", regular, group::data},
    section_kind{"__DATA", "__init_array", regular, group::init_array},
    section_kind{"__DATA", "__bss", zerofill, group::bss},
};

struct section_ref { std::size_t object, section; };
struct definition { std::size_t object; const obj::symbol* symbol; };

struct load_segment {
  std::uint32_t flags;
  std::uint64_t offset, file_size, memory_size;
};

struct patch {
  std::uint64_t offset, value;
  std::size_t width;
  section_ref source;
};

constexpr std::uint64_t align_up(std::uint64_t offset, std::uint64_t align) {
  return (offset + align - 1) & ~(align - 1);
}

bool executable(const obj::section& s) { return s.segment == "__TEXT" && s.name == "__text"; }

std::unexpected<std::string> object_error(const obj::object& o, std::string_view reason) {
  return std::unexpected(std::format("{}: {}", o.name.empty() ? "ELF input" : o.name, reason));
}

std::unexpected<std::string> section_error(const obj::object& o, const obj::section& s, std::string_view reason) {
  return object_error(o, std::format("{},{}: {}", s.segment, s.name, reason));
}

void write_word(std::span<std::uint8_t> out, std::uint64_t value) {
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

template <std::unsigned_integral T>
void append(bytes& out, T value) {
  const auto offset = out.size();
  out.resize(offset + sizeof(T));
  write_word(std::span(out).subspan(offset), value);
}

void write_program_header(bytes& out, std::uint32_t type, std::uint32_t flags, std::uint64_t offset,
                          std::uint64_t address, std::uint64_t file_size, std::uint64_t memory_size,
                          std::uint64_t alignment) {
  append(out, type);
  append(out, flags);
  append(out, offset);
  append(out, address);  // p_vaddr
  append(out, address);  // p_paddr is unused by Linux
  append(out, file_size);
  append(out, memory_size);
  append(out, alignment);
}

bytes write_headers(std::uint64_t entry, std::uint16_t program_count, std::span<const load_segment> loads) {
  bytes out{0x7f, 'E', 'L', 'F', 2, 1, 1};  // ELFCLASS64, ELFDATA2LSB, EV_CURRENT
  out.resize(16, 0);                        // ELFOSABI_NONE and padding
  append(out, et_exec);
  append(out, em_x86_64);
  append(out, std::uint32_t{1});           // e_version
  append(out, entry);
  append(out, std::uint64_t{header_size}); // e_phoff
  append(out, std::uint64_t{0});           // e_shoff
  append(out, std::uint32_t{0});           // e_flags
  append(out, header_size);
  append(out, program_header_size);
  append(out, program_count);
  append(out, std::uint16_t{0});           // e_shentsize
  append(out, std::uint16_t{0});           // e_shnum
  append(out, std::uint16_t{0});           // e_shstrndx
  for (const auto& segment : loads) {
    write_program_header(out, pt_load, segment.flags, segment.offset, image_base + segment.offset,
                         segment.file_size, segment.memory_size, page_size);
  }
  write_program_header(out, pt_gnu_stack, pf_r | pf_w, 0, 0, 0, 0, 16);
  return out;
}

}  // namespace

std::expected<bytes, std::string> link(std::span<const obj::object> objects, std::string_view entry) {
  std::array<std::vector<section_ref>, std::to_underlying(group::count)> sections;
  std::map<std::string_view, definition, std::less<>> globals;
  for (std::size_t oi = 0; oi < objects.size(); ++oi) {
    const auto& o = objects[oi];
    if (o.arch != obj::target::x86_64_linux) return object_error(o, "expected an x86-64 Linux object");
    for (std::size_t si = 0; si < o.sections.size(); ++si) {
      const auto& s = o.sections[si];
      const auto kind = std::ranges::find_if(section_kinds, [&](const section_kind& k) {
        return s.segment == k.segment && s.name == k.name;
      });
      if (kind == section_kinds.end()) return section_error(o, s, "unsupported ELF section");
      const auto flags = kind->output == group::text ? s.flags & ~instruction_flags : s.flags;
      if (flags != kind->flags) return section_error(o, s, "unsupported ELF section flags");
      if (s.is_zerofill() ? !s.bytes.empty() || !s.relocs.empty() : s.zerofill_size != 0) {
        return section_error(o, s, "invalid zero-fill section contents");
      }
      if (s.align_log2 > max_align_log2) return section_error(o, s, "section alignment exceeds 4 MiB");
      if (kind->output == group::init_array) {
        if (s.align_log2 != 3 || s.size() % 8 != 0)
          return section_error(o, s, "invalid .init_array alignment or size");
        if (s.relocs.size() != s.size() / 8)
          return section_error(o, s, ".init_array requires one relocation per slot");
      }
      sections[std::to_underlying(kind->output)].push_back({oi, si});
    }
    for (const obj::symbol& s : o.symbols) {
      if (s.bind != binding::local && s.bind != binding::global && s.bind != binding::hidden) {
        return object_error(o, std::format("unsupported binding for symbol '{}'", s.name));
      }
      if ((s.name.empty() && s.bind != binding::local) || s.name.contains('\0')) {
        return object_error(o, "invalid ELF symbol name");
      }
      if (s.bind == binding::local && (!s.defined() || s.weak)) {
        return object_error(o, std::format("invalid local symbol '{}'", s.name));
      }
      if (!s.defined()) {
        if (s.offset != 0) return object_error(o, std::format("undefined symbol '{}' has a section offset", s.name));
        continue;
      }
      if (*s.section >= o.sections.size() || s.offset > o.sections[*s.section].size()) {
        return object_error(o, std::format("symbol '{}' is outside its section", s.name));
      }
      if (s.bind == binding::local) continue;
      auto [existing, inserted] = globals.try_emplace(s.name, definition{oi, &s});
      if (inserted || s.weak) continue;
      if (!existing->second.symbol->weak) return object_error(o, std::format("duplicate symbol '{}'", s.name));
      existing->second = {oi, &s};
    }
  }

  for (const auto& o : objects) {
    for (const auto& s : o.symbols) {
      if (!s.defined() && !s.weak && !globals.contains(s.name)) {
        return object_error(o, std::format("undefined symbol '{}'", s.name));
      }
    }
  }

  const auto start = globals.find(entry);
  if (start == globals.end()) return std::unexpected(std::format("ELF entry symbol '{}' is undefined", entry));
  const auto& entry_section = objects[start->second.object].sections[*start->second.symbol->section];
  if (!executable(entry_section) || start->second.symbol->offset == entry_section.size()) {
    return std::unexpected(std::format("ELF entry symbol '{}' is not inside executable code", entry));
  }

  const auto& bss = sections[std::to_underlying(group::bss)];
  const auto& init_array = sections[std::to_underlying(group::init_array)];
  const bool has_read_only = !sections[std::to_underlying(group::read_only)].empty() || !init_array.empty();
  const bool has_data = !sections[std::to_underlying(group::data)].empty() || !bss.empty();
  const auto program_count = static_cast<std::uint16_t>(2 + has_read_only + has_data);  // RX, GNU_STACK, R?, RW?
  std::uint64_t file_end = header_size + program_count * program_header_size;

  std::vector<std::vector<std::uint64_t>> offsets;
  offsets.reserve(objects.size());
  for (const auto& o : objects) offsets.emplace_back(o.sections.size());
  auto place = [&](std::span<const section_ref> inputs, std::uint64_t offset) -> std::expected<std::uint64_t, std::string> {
    for (const auto [oi, si] : inputs) {
      const auto& s = objects[oi].sections[si];
      offset = align_up(offset, std::uint64_t{1} << s.align_log2);
      if (offset > image_size_limit || s.size() > image_size_limit - offset) {
        return section_error(objects[oi], s, "section exceeds the ELF image address limit");
      }
      offsets[oi][si] = offset;
      offset += s.size();
    }
    return offset;
  };

  // Permissions are set per page. Put zero-filled contributions last in the final segment,
  // where p_memsz can include both their alignment padding and their bytes without file space.
  constexpr std::array load_groups{std::pair{group::text, pf_r | pf_x},
                                   std::pair{group::read_only, pf_r},
                                   std::pair{group::data, pf_r | pf_w}};
  std::vector<load_segment> loads;
  for (const auto& [kind, flags] : load_groups) {
    if ((kind == group::read_only && !has_read_only) || (kind == group::data && !has_data)) continue;
    const auto begin = kind == group::text ? 0 : align_up(file_end, page_size);
    auto initialized = place(sections[std::to_underlying(kind)], kind == group::text ? file_end : begin);
    if (!initialized) return std::unexpected(initialized.error());
    // The table is fixed at link time. Each contribution has 8-byte alignment and an integral
    // number of slots, so only the first can need padding. Startup's empty contribution is last.
    if (kind == group::read_only) initialized = place(init_array, *initialized);
    if (!initialized) return std::unexpected(initialized.error());
    const auto memory = kind == group::data ? place(bss, *initialized) : initialized;
    if (!memory) return std::unexpected(memory.error());
    file_end = *initialized;
    loads.push_back({flags, begin, file_end - begin, *memory - begin});
  }

  auto address = [&](definition d) {
    return image_base + offsets[d.object][*d.symbol->section] + d.symbol->offset;
  };
  auto resolve = [&](std::size_t oi, const obj::symbol& s) -> std::optional<definition> {
    if (s.bind == binding::local) return definition{oi, &s};
    const auto global = globals.find(s.name);
    if (global == globals.end()) return std::nullopt;
    return global->second;
  };

  std::vector<patch> patches;
  for (std::size_t oi = 0; oi < objects.size(); ++oi) {
    const auto& o = objects[oi];
    for (std::size_t si = 0; si < o.sections.size(); ++si) {
      const auto& s = o.sections[si];
      const bool is_init_array = s.is_init_array();
      for (const auto& r : s.relocs) {
        const bool absolute = r.kind == reloc_kind::abs64;
        if (r.minus) return section_error(o, s, "ELF symbol-difference relocations are unsupported");
        if (!absolute && r.kind != reloc_kind::x86_64_pc32 && r.kind != reloc_kind::x86_64_plt32) {
          return section_error(o, s, "unsupported x86-64 ELF relocation kind");
        }
        if (is_init_array && (!absolute || r.offset % 8 != 0))
          return section_error(o, s, ".init_array requires an aligned R_X86_64_64 relocation per slot");
        const std::size_t width = absolute ? 8 : 4;
        if (r.offset > s.size() || width > s.size() - r.offset) {
          return section_error(o, s, "ELF relocation is outside its section");
        }
        if (r.symbol >= o.symbols.size()) return section_error(o, s, "invalid ELF relocation symbol index");
        const auto offset = offsets[oi][si] + r.offset;
        const auto def = resolve(oi, o.symbols[r.symbol]);
        if (is_init_array) {
          if (!def) return section_error(o, s, ".init_array relocation has no defined target");
          const auto& code = objects[def->object].sections[*def->symbol->section];
          // Image layout has bounded both offsets. Compare the signed addend before adding it.
          const auto symbol_offset = static_cast<std::int64_t>(def->symbol->offset);
          const auto size = static_cast<std::int64_t>(code.size());
          if (!executable(code) || r.addend < -symbol_offset || r.addend >= size - symbol_offset)
            return section_error(o, s, ".init_array relocation target is outside its executable input section");
        }
        // An undefined weak symbol without a definition has value zero (ELF gABI, Symbol Binding).
        const auto target = def ? address(*def) : 0;
        auto value = target + static_cast<std::uint64_t>(r.addend);  // R_X86_64_64: S + A modulo 2^64
        if (!absolute) {
          // R_X86_64_PC32: S + A - P. With static binding, PLT32 targets S directly as well.
          const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(image_base + offset);
          if (r.addend < std::numeric_limits<std::int32_t>::min() - delta ||
              r.addend > std::numeric_limits<std::int32_t>::max() - delta) {
            const auto kind = r.kind == reloc_kind::x86_64_pc32 ? "R_X86_64_PC32" : "R_X86_64_PLT32";
            return section_error(o, s, std::format("{} displacement overflows for '{}'", kind, o.symbols[r.symbol].name));
          }
          value = static_cast<std::uint32_t>(delta + r.addend);
        }
        patches.push_back({offset, value, width, {oi, si}});
      }
    }
  }
  std::ranges::stable_sort(patches, {}, &patch::offset);
  std::uint64_t previous_end = 0;
  for (const auto& p : patches) {
    if (p.offset < previous_end) {
      const auto& o = objects[p.source.object];
      return section_error(o, o.sections[p.source.section], "overlapping ELF relocations are unsupported");
    }
    previous_end = p.offset + p.width;
  }

  auto image = write_headers(address(start->second), program_count, loads);
  if (file_end > image.max_size()) return std::unexpected("ELF image exceeds the maximum file buffer size");
  image.resize(file_end);
  for (std::size_t oi = 0; oi < objects.size(); ++oi) {
    const auto& o = objects[oi];
    for (std::size_t si = 0; si < o.sections.size(); ++si) {
      const auto& s = o.sections[si];
      if (!s.bytes.empty()) std::ranges::copy(s.bytes, image.data() + offsets[oi][si]);
    }
  }
  for (const auto& p : patches) write_word(std::span(image).subspan(p.offset, p.width), p.value);
  return image;
}

}  // namespace ycxx::link::elf
