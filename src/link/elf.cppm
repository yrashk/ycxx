// Linux x86-64 executables, from the ELF gABI and the System V AMD64 psABI.
export module ycxx.link.elf;

export import ycxx.obj;
export import ycxx.link.elf.shared;
import std;

export namespace ycxx::link::elf {

struct dynamic_options {
  std::string interpreter;
  std::vector<shared_object> libraries;
};

// Link in-memory x86-64 Linux objects. The caller supplies the entry definition in code and
// handles process startup and exit. Only the standard sections and ABS64, PC32 and PLT32
// relocations are supported. The ET_EXEC image has load segments and a non-executable stack.
// It has no section table. `entry` names a nonlocal symbol. Dynamic links support direct calls
// to functions provided by the supplied libraries.
std::expected<std::vector<std::uint8_t>, std::string> link(std::span<const obj::object> objects,
                                                        std::string_view entry = "_start",
                                                        const dynamic_options* dynamic = nullptr);

}  // namespace ycxx::link::elf

namespace ycxx::link::elf {
namespace {

using bytes = std::vector<std::uint8_t>;
using obj::binding;
using obj::reloc_kind;

constexpr std::uint16_t et_exec = 2, em_x86_64 = 62;
constexpr std::uint16_t header_size = 64, program_header_size = 56;
constexpr std::uint32_t pt_load = 1, pt_dynamic = 2, pt_interp = 3, pt_phdr = 6, pt_gnu_stack = 0x6474e551;
constexpr std::uint64_t dt_null = 0, dt_needed = 1, dt_hash = 4, dt_strtab = 5, dt_symtab = 6;
constexpr std::uint64_t dt_rela = 7, dt_relasz = 8, dt_relaent = 9, dt_strsz = 10, dt_syment = 11;
constexpr std::uint64_t symbol_size = 24, rela_size = 24, dynamic_entry_size = 16, slot_size = 8, thunk_size = 6;
constexpr std::uint32_t r_x86_64_glob_dat = 6;
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
  bool direct_import;
};

using import_indices = std::map<std::string_view, std::uint32_t, std::less<>>;

struct dynamic_layout {
  std::uint64_t interpreter, strings, symbols, hash, rela, read_only_end;
  std::uint64_t thunks = 0, table = 0, slots = 0;
  bytes names{0};
  std::vector<std::uint32_t> needed;
  std::vector<std::uint32_t> symbol_names;

  std::uint64_t table_size() const { return (needed.size() + 9) * dynamic_entry_size; }
  std::uint64_t rela_size_bytes() const { return symbol_names.size() * rela_size; }
};

constexpr std::uint64_t align_up(std::uint64_t offset, std::uint64_t align) {
  return (offset + align - 1) & ~(align - 1);
}

std::optional<std::uint32_t> displacement(std::uint64_t target, std::uint64_t pc, std::int64_t addend) {
  // Image addresses fit in 47 bits. Bound the addend before adding it.
  const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
  if (addend < std::numeric_limits<std::int32_t>::min() - delta ||
      addend > std::numeric_limits<std::int32_t>::max() - delta) return std::nullopt;
  return static_cast<std::uint32_t>(delta + addend);
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

bytes write_headers(std::uint64_t entry, std::uint16_t program_count, std::span<const load_segment> loads,
                    const dynamic_layout* dynamic, std::uint64_t interpreter_size) {
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
  if (dynamic) {
    const auto size = std::uint64_t{program_count} * program_header_size;
    write_program_header(out, pt_phdr, pf_r, header_size, image_base + header_size, size, size, 8);
    write_program_header(out, pt_interp, pf_r, dynamic->interpreter, image_base + dynamic->interpreter,
                         interpreter_size, interpreter_size, 1);
  }
  for (const auto& segment : loads) {
    write_program_header(out, pt_load, segment.flags, segment.offset, image_base + segment.offset,
                         segment.file_size, segment.memory_size, page_size);
  }
  if (dynamic) {
    const auto size = dynamic->table_size();
    write_program_header(out, pt_dynamic, pf_r | pf_w, dynamic->table, image_base + dynamic->table, size, size, 8);
  }
  write_program_header(out, pt_gnu_stack, pf_r | pf_w, 0, 0, 0, 0, 16);
  return out;
}

const shared_symbol* provided_symbol(const dynamic_options& options, std::string_view name) {
  for (const auto& library : options.libraries) {
    const auto found = std::ranges::lower_bound(library.exports, name, std::less<>{}, &shared_symbol::name);
    if (found != library.exports.end() && found->name == name) return &*found;
  }
  return nullptr;
}

std::expected<dynamic_layout, std::string> layout_dynamic(const dynamic_options& options, import_indices& imports,
                                                         std::uint64_t headers_end) {
  constexpr auto max_word = std::numeric_limits<std::uint32_t>::max();
  if (imports.size() >= max_word || options.libraries.size() >= max_word ||
      options.interpreter.size() >= image_size_limit - headers_end)
    return std::unexpected("ELF dynamic metadata exceeds the image size limit");
  dynamic_layout layout{};
  const auto add_name = [&](std::string_view name) -> std::optional<std::uint32_t> {
    if (name.size() >= max_word - layout.names.size()) return std::nullopt;
    const auto offset = static_cast<std::uint32_t>(layout.names.size());
    layout.names.insert(layout.names.end(), name.begin(), name.end());
    layout.names.push_back(0);
    return offset;
  };
  for (const auto& library : options.libraries) {
    const auto offset = add_name(library.soname);
    if (!offset) return std::unexpected("ELF dynamic string table exceeds the size limit");
    layout.needed.push_back(*offset);
  }
  for (auto& [name, index] : imports) {
    const auto offset = add_name(name);
    if (!offset) return std::unexpected("ELF dynamic string table exceeds the size limit");
    index = static_cast<std::uint32_t>(layout.symbol_names.size());
    layout.symbol_names.push_back(*offset);
  }
  layout.interpreter = headers_end;
  layout.strings = layout.interpreter + options.interpreter.size() + 1;
  layout.symbols = align_up(layout.strings + layout.names.size(), 8);
  const auto symbol_count = layout.symbol_names.size() + 1;  // including STN_UNDEF
  layout.hash = layout.symbols + symbol_count * symbol_size;
  layout.rela = align_up(layout.hash + 4 * (3 + symbol_count), 8);  // header, one bucket, symbol chains
  layout.read_only_end = layout.rela + std::max<std::uint64_t>(layout.rela_size_bytes(), 8);
  if (layout.read_only_end > image_size_limit)
    return std::unexpected("ELF dynamic metadata exceeds the image size limit");
  return layout;
}

std::expected<void, std::string> write_dynamic(bytes& image, const dynamic_layout& layout,
                                               const dynamic_options& options) {
  std::ranges::copy(options.interpreter, image.data() + layout.interpreter);
  std::ranges::copy(layout.names, image.data() + layout.strings);
  const auto word = [&](std::uint64_t offset, std::size_t width, std::uint64_t value) {
    write_word(std::span(image).subspan(offset, width), value);
  };
  const auto symbol_count = layout.symbol_names.size() + 1;
  word(layout.hash, 4, 1);  // a single bucket is sufficient for this undefined-only symbol table
  word(layout.hash + 4, 4, symbol_count);
  if (symbol_count > 1) word(layout.hash + 8, 4, 1);  // first symbol in the bucket
  for (std::size_t i = 0; i < layout.symbol_names.size(); ++i) {
    const auto index = i + 1;
    const auto symbol = layout.symbols + index * symbol_size;
    word(symbol, 4, layout.symbol_names[i]);
    image[symbol + 4] = 0x12;  // STB_GLOBAL / STT_FUNC, with STV_DEFAULT and SHN_UNDEF
    if (index + 1 < symbol_count) word(layout.hash + 12 + index * 4, 4, index + 1);

    const auto slot = layout.slots + i * slot_size;
    const auto thunk = layout.thunks + i * thunk_size;
    const auto rip_offset = displacement(slot, thunk + thunk_size, 0);
    if (!rip_offset) return std::unexpected("ELF import thunk displacement overflows");
    // jmp qword ptr [rip + slot]; tail jump preserves the call's arguments and return address.
    image[thunk] = 0xff;
    image[thunk + 1] = 0x25;
    word(thunk + 2, 4, *rip_offset);
    const auto rela = layout.rela + i * rela_size;
    word(rela, 8, image_base + slot);
    word(rela + 8, 8, (std::uint64_t{index} << 32) | r_x86_64_glob_dat);
  }

  auto offset = layout.table;
  const auto tag = [&](std::uint64_t type, std::uint64_t value) {
    word(offset, 8, type);
    word(offset + 8, 8, value);
    offset += dynamic_entry_size;
  };
  for (auto name : layout.needed) tag(dt_needed, name);
  tag(dt_strtab, image_base + layout.strings);
  tag(dt_strsz, layout.names.size());
  tag(dt_symtab, image_base + layout.symbols);
  tag(dt_syment, symbol_size);
  tag(dt_hash, image_base + layout.hash);
  tag(dt_rela, image_base + layout.rela);
  tag(dt_relasz, layout.rela_size_bytes());
  tag(dt_relaent, rela_size);
  tag(dt_null, 0);
  return {};
}

}  // namespace

std::expected<bytes, std::string> link(std::span<const obj::object> objects, std::string_view entry,
                                     const dynamic_options* dynamic) {
  if (dynamic) {
    if (!dynamic->interpreter.starts_with('/') || dynamic->interpreter.contains('\0'))
      return std::unexpected("ELF dynamic linking requires an absolute interpreter path");
    if (dynamic->libraries.empty()) return std::unexpected("ELF dynamic linking requires an explicit shared object");
    std::set<std::string_view> sonames;
    for (const auto& library : dynamic->libraries) {
      if (library.soname.empty() || library.soname.contains('/') || library.soname.contains('\0'))
        return std::unexpected(std::format("{}: invalid ELF shared object SONAME", library.name));
      if (!sonames.insert(library.soname).second)
        return std::unexpected(std::format("{}: duplicate ELF shared object SONAME '{}'", library.name, library.soname));
    }
  }

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

  import_indices imports;
  for (const auto& o : objects) {
    for (const auto& s : o.symbols) {
      if (s.defined() || s.weak || globals.contains(s.name)) continue;
      if (!dynamic || s.bind != binding::global)
        return object_error(o, std::format("undefined symbol '{}'", s.name));
      if (imports.contains(s.name)) continue;
      const auto* provider = provided_symbol(*dynamic, s.name);
      if (!provider) return object_error(o, std::format("undefined symbol '{}'", s.name));
      if (!provider->supported_function)
        return object_error(o, std::format("unsupported ELF import '{}': expected a default-visible global function", s.name));
      imports.try_emplace(s.name);
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
  // Both layouts have RX and GNU_STACK. Dynamic links always have R, RW, PHDR, INTERP and DYNAMIC.
  const auto program_count = static_cast<std::uint16_t>(dynamic ? 7 : 2 + has_read_only + has_data);
  const std::uint64_t headers_end = header_size + program_count * program_header_size;
  std::uint64_t file_end = headers_end;
  std::optional<dynamic_layout> layout;
  if (dynamic) {
    auto metadata = layout_dynamic(*dynamic, imports, headers_end);
    if (!metadata) return std::unexpected(metadata.error());
    layout.emplace(std::move(*metadata));
    file_end = layout->read_only_end;
  }

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
  constexpr auto text = std::pair{group::text, pf_r | pf_x};
  constexpr auto read_only = std::pair{group::read_only, pf_r};
  constexpr auto data = std::pair{group::data, pf_r | pf_w};
  const auto load_groups = dynamic ? std::array{read_only, text, data} : std::array{text, read_only, data};
  std::vector<load_segment> loads;
  for (const auto& [kind, flags] : load_groups) {
    if (!dynamic && ((kind == group::read_only && !has_read_only) || (kind == group::data && !has_data))) continue;
    const auto begin = loads.empty() ? 0 : align_up(file_end, page_size);
    auto input_begin = loads.empty() ? file_end : begin;
    if (layout && kind == group::data) {
      layout->table = begin;
      layout->slots = begin + layout->table_size();
      input_begin = layout->slots + imports.size() * slot_size;
    }
    auto initialized = place(sections[std::to_underlying(kind)], input_begin);
    if (!initialized) return std::unexpected(initialized.error());
    if (layout && kind == group::text) {
      layout->thunks = *initialized;
      *initialized += imports.size() * thunk_size;
    }
    if (*initialized > image_size_limit) return std::unexpected("ELF load segment exceeds the image address limit");
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
  std::vector<std::uint8_t> import_has_call(imports.size());
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
        const auto& symbol = o.symbols[r.symbol];
        const auto def = resolve(oi, symbol);
        const auto imported = def ? imports.end() : imports.find(symbol.name);
        if (!def && imported == imports.end() && dynamic && symbol.weak && symbol.bind == binding::global &&
            provided_symbol(*dynamic, symbol.name))
          return section_error(o, s, std::format("weak ELF import '{}' is unsupported", symbol.name));
        if (imported != imports.end() &&
            (r.kind != reloc_kind::x86_64_plt32 || r.addend != -4 || symbol.bind != binding::global ||
             !executable(s) || r.offset == 0 || s.bytes[r.offset - 1] != 0xe8))
          return section_error(o, s, std::format("ELF import '{}' requires a direct call with R_X86_64_PLT32", symbol.name));
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
        auto target = def ? address(*def) : 0;
        if (imported != imports.end()) {
          const auto index = imported->second;
          import_has_call[index] = 1;
          target = image_base + layout->thunks + index * thunk_size;
        }
        auto value = target + static_cast<std::uint64_t>(r.addend);  // R_X86_64_64: S + A modulo 2^64
        if (!absolute) {
          // S + A - P, with an imported call targeting its thunk (L) instead of S.
          const auto delta = displacement(target, image_base + offset, r.addend);
          if (!delta) {
            const auto kind = r.kind == reloc_kind::x86_64_pc32 ? "R_X86_64_PC32" : "R_X86_64_PLT32";
            return section_error(o, s, std::format("{} displacement overflows for '{}'", kind, symbol.name));
          }
          value = *delta;
        }
        patches.push_back({offset, value, width, {oi, si}, imported != imports.end()});
      }
    }
  }
  for (const auto& [name, index] : imports) {
    if (!import_has_call[index])
      return std::unexpected(std::format("ELF import '{}' has no direct call reference", name));
  }
  std::ranges::stable_sort(patches, {}, &patch::offset);
  std::uint64_t previous_end = 0;
  for (const auto& p : patches) {
    // An otherwise adjacent fixup cannot overwrite the opcode of an imported call.
    if (p.offset - p.direct_import < previous_end) {
      const auto& o = objects[p.source.object];
      return section_error(o, o.sections[p.source.section], "overlapping ELF relocations are unsupported");
    }
    previous_end = p.offset + p.width;
  }

  auto image = write_headers(address(start->second), program_count, loads, layout ? &*layout : nullptr,
                             dynamic ? dynamic->interpreter.size() + 1 : 0);
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
  if (layout) {
    auto written = write_dynamic(image, *layout, *dynamic);
    if (!written) return std::unexpected(written.error());
  }
  return image;
}

}  // namespace ycxx::link::elf
