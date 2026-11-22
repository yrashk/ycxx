// ELF64 relocatable objects for x86-64, from the ELF gABI and the System V AMD64 psABI.
export module ycxx.obj.elf;

export import ycxx.obj;
import std;

export namespace ycxx::obj::elf {

// The standard code and data sections, symbols and explicit-addend relocations for x86-64.
// A COMDAT group has one read-only payload and a named weak object. A .data.rel.ro payload
// also groups its generated RELA section for named-symbol ABS64 relocations.
// Returns a diagnostic if the in-memory object uses a feature outside this subset.
std::expected<std::vector<std::uint8_t>, std::string> write_object(const object& o);

// Read the x86-64 ELF64 relocatable subset emitted by write_object and the GNU forms
// used by libycxx's static linkage marker. Preserve types and sizes for defined functions
// and for objects in COMDAT payloads.
// Retained sections and symbols keep file order. Preserve COMDAT signatures and member sections.
std::expected<object, std::string> read_object(std::span<const std::uint8_t> file, std::string name);

}  // namespace ycxx::obj::elf

namespace ycxx::obj::elf {
namespace {

constexpr std::uint16_t et_rel = 1, em_x86_64 = 62, shn_loreserve = 0xff00;
constexpr std::uint16_t shn_abs = 0xfff1;
constexpr std::uint16_t header_size = 64, section_header_size = 64;
constexpr std::uint64_t symbol_size = 24, rela_size = 24;
constexpr std::uint32_t sht_progbits = 1, sht_symtab = 2, sht_strtab = 3, sht_rela = 4, sht_nobits = 8;
constexpr std::uint32_t sht_init_array = 14, sht_group = 17, grp_comdat = 1;
constexpr std::uint32_t sht_x86_64_unwind = 0x70000001;
constexpr std::uint64_t shf_write = 1, shf_alloc = 2, shf_execinstr = 4, shf_info_link = 0x40;
constexpr std::uint64_t shf_merge = 0x10, shf_strings = 0x20, shf_group = 0x200;
constexpr std::uint8_t stt_notype = 0, stt_object = 1, stt_func = 2, stt_section = 3, stt_file = 4;
constexpr std::uint8_t stb_local = 0, stb_global = 1, stb_weak = 2;
constexpr std::uint8_t stv_default = 0, stv_hidden = 2;

struct section_mapping {
  standard_section kind;
  std::string_view segment;
  std::string_view section;
  std::string_view name;
  std::uint32_t type;
  std::uint64_t flags;
};

constexpr std::array standard_sections{
    section_mapping{standard_section::text, "__TEXT", "__text", ".text", sht_progbits, shf_alloc | shf_execinstr},
    section_mapping{standard_section::cstring, "__TEXT", "__cstring", ".rodata.str", sht_progbits, shf_alloc},
    section_mapping{standard_section::rodata, "__TEXT", "__const", ".rodata", sht_progbits, shf_alloc},
    section_mapping{standard_section::data_const, "__DATA", "__const", ".data.rel.ro", sht_progbits, shf_alloc | shf_write},
    section_mapping{standard_section::data, "__DATA", "__data", ".data", sht_progbits, shf_alloc | shf_write},
    section_mapping{standard_section::bss, "__DATA", "__bss", ".bss", sht_nobits, shf_alloc | shf_write},
    section_mapping{standard_section::init_array, "__DATA", "__init_array", ".init_array", sht_init_array, shf_alloc | shf_write},
    section_mapping{standard_section::eh_frame, "__TEXT", "__eh_frame", ".eh_frame", sht_x86_64_unwind, shf_alloc},
    section_mapping{standard_section::gcc_except_table, "__TEXT", "__gcc_except", ".gcc_except_table", sht_progbits, shf_alloc},
};

struct relocation_mapping {
  reloc_kind kind;
  std::uint32_t type;
  std::uint32_t width;
};

constexpr std::array relocations{
    relocation_mapping{reloc_kind::abs64, 1, 8},         // R_X86_64_64
    relocation_mapping{reloc_kind::x86_64_pc32, 2, 4},   // R_X86_64_PC32
    relocation_mapping{reloc_kind::x86_64_plt32, 4, 4},  // R_X86_64_PLT32
    relocation_mapping{reloc_kind::x86_64_rex_gotpcrelx, 42, 4},  // R_X86_64_REX_GOTPCRELX
};
constexpr std::uint32_t r_x86_64_32 = 10;  // only needed in discarded DWARF sections

constexpr std::array<std::uint8_t, 9> elf_ident{0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0};

using bytes = std::vector<std::uint8_t>;

template <std::unsigned_integral T>
void append(bytes& out, T n) {
  for (std::size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
}

// The caller first bounds the entire header or table entry containing the field.
template <std::unsigned_integral T>
T load(std::span<const std::uint8_t> record, std::size_t offset) {
  std::uint64_t n = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) n |= std::uint64_t{record[offset + i]} << (8 * i);
  return static_cast<T>(n);
}

std::uint32_t append_name(bytes& table, std::string_view s) {
  if (s.empty()) return 0;
  auto index = static_cast<std::uint32_t>(table.size());
  table.insert(table.end(), s.begin(), s.end());
  table.push_back(0);
  return index;
}

struct section_header {
  std::uint32_t name = 0;
  std::uint32_t type = 0;
  std::uint64_t flags = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint32_t link = 0;
  std::uint32_t info = 0;
  std::uint64_t align = 0;
  std::uint64_t entsize = 0;

  bool contains_code_at(std::uint64_t at) const {
    return type == sht_progbits && (flags & shf_execinstr) != 0 && at < size;
  }

  void write(bytes& out) const {
    append(out, name);
    append(out, type);
    append(out, flags);
    append(out, std::uint64_t{0});  // sh_addr: assigned by the linker
    append(out, offset);
    append(out, size);
    append(out, link);
    append(out, info);
    append(out, align);
    append(out, entsize);
  }
};

bool all_zero(std::span<const std::uint8_t> record) {
  return std::ranges::all_of(record, [](auto b) { return b == 0; });
}

bool valid_string_table(const section_header& header, std::span<const std::uint8_t> table) {
  return header.type == sht_strtab && header.flags == 0 && header.link == 0 && header.info == 0 &&
         header.entsize == 0 && (table.empty() || (table.front() == 0 && table.back() == 0));
}

bool plain_metadata(const section_header& header) {
  return header.type == sht_progbits && header.link == 0 && header.info == 0 &&
         ((header.flags == 0 && header.entsize == 0) ||
          (header.flags == (shf_merge | shf_strings) && header.entsize == 1));
}

bool valid_unwind_section(std::uint64_t alignment, std::uint64_t size) {
  return (alignment == 4 || alignment == 8) && size % 4 == 0 && std::in_range<std::uint32_t>(size);
}

// The first handler profile uses one ungrouped table and one frame section. The ordinary
// code-only input subset has no LSDA and retains its existing relocation rules.
std::optional<std::string_view> handler_frame_error(const object& o) {
  const auto tables = std::ranges::find_if(o.sections, &section::is_gcc_except_table);
  if (tables == o.sections.end()) return std::nullopt;
  const auto frames = std::ranges::find_if(o.sections, &section::is_eh_frame);
  if (frames == o.sections.end() || std::ranges::count_if(o.sections, &section::is_eh_frame) != 1 ||
      std::ranges::count_if(o.sections, &section::is_gcc_except_table) != 1 ||
      tables->flags != section_flags::regular || tables->zerofill_size != 0 || tables->size() == 0 ||
      !std::in_range<std::uint32_t>(tables->size()) || !tables->relocs.empty() ||
      frames->flags != section_flags::regular || frames->zerofill_size != 0 ||
      !valid_unwind_section(8, frames->size()) || frames->align_log2 != 3)
    return "invalid ELF handler table or frame section";

  constexpr std::uint8_t ordinary_cie[]{1, 'z', 'R', 0, 1, 0x78, 16, 1, 0x1b};
  constexpr std::uint8_t handler_cie[]{1, 'z', 'P', 'L', 'R', 0, 1, 0x78, 16, 7, 0x9b};
  std::map<std::uint32_t, bool> cies;
  std::map<std::uint32_t, const reloc*> relocs;
  for (const auto& r : frames->relocs) {
    if (r.kind != reloc_kind::x86_64_pc32 || r.addend != 0 || r.minus || r.symbol >= o.symbols.size() ||
        !relocs.emplace(r.offset, &r).second)
      return "invalid ELF handler frame relocation";
  }
  auto local_anchor = [&](std::uint32_t at) -> const symbol* {
    const auto r = relocs.find(at);
    if (r == relocs.end()) return nullptr;
    const auto& s = o.symbols[r->second->symbol];
    relocs.erase(r);
    return s.bind == binding::local && s.section && *s.section < o.sections.size() ? &s : nullptr;
  };
  auto personality_cell = [&](std::uint32_t at) {
    const auto* s = local_anchor(at);
    if (!s) return false;
    const auto& data = o.sections[*s->section];
    if (data.segment != "__DATA" || data.name != "__const" || data.align_log2 < 3 ||
        data.flags != section_flags::regular || data.zerofill_size != 0 ||
        s->offset % 8 != 0 || s->offset > data.size() || data.size() - s->offset < 8)
      return false;
    const auto ptr = std::ranges::find(data.relocs, s->offset, &reloc::offset);
    if (ptr == data.relocs.end() || ptr->kind != reloc_kind::abs64 || ptr->addend != 0 || ptr->minus ||
        ptr->symbol >= o.symbols.size() || o.symbols[ptr->symbol].bind != binding::global ||
        o.symbols[ptr->symbol].name_view() != "__gxx_personality_v0")
      return false;
    return std::ranges::none_of(data.relocs, [&](const reloc& r) {
      const std::uint64_t width = r.kind == reloc_kind::abs64 ? 8 : 4;
      return &r != &*ptr && r.offset < s->offset + 8 && r.offset + width > s->offset;
    });
  };

  const std::span<const std::uint8_t> bytes = frames->bytes;
  bool has_handler = false;
  bool has_ordinary_cie = false, has_handler_cie = false;
  for (std::uint32_t at = 0; at < bytes.size();) {
    if (bytes.size() - at < 8) return "truncated ELF handler frame record";
    const std::uint64_t size = std::uint64_t{load<std::uint32_t>(bytes, at)} + 4;
    if (size < 24 || size % 8 != 0 || size > bytes.size() - at)
      return "invalid ELF handler frame record size";
    const auto record = bytes.subspan(at, size);
    const auto cie = load<std::uint32_t>(record, 4);
    if (cie == 0) {
      if (size == 24 && std::ranges::equal(record.subspan(8, sizeof(ordinary_cie)), ordinary_cie)) {
        if (std::exchange(has_ordinary_cie, true)) return "duplicate ELF ordinary CIE";
        cies.emplace(at, false);
      } else if (size == 32 && std::ranges::equal(record.subspan(8, sizeof(handler_cie)), handler_cie) &&
                 load<std::uint32_t>(record, 19) == 0 && record[23] == 0x1b && record[24] == 0x1b) {
        if (std::exchange(has_handler_cie, true)) return "duplicate ELF handler CIE";
        if (!personality_cell(at + 19)) return "invalid ELF handler personality cell relocation";
        cies.emplace(at, true);
      } else {
        return "unsupported ELF handler CIE";
      }
    } else {
      if (cie > at + 4) return "invalid ELF handler FDE CIE pointer";
      const auto common = cies.find(at + 4 - cie);
      if (common == cies.end() || record[16] != (common->second ? 4 : 0))
        return "invalid ELF handler FDE augmentation";
      const auto* s = local_anchor(at + 8);
      if (!s || load<std::uint32_t>(record, 8) != 0) return "missing ELF handler frame code relocation";
      const auto& code = o.sections[*s->section];
      const auto range = load<std::uint32_t>(record, 12);
      if (code.segment != "__TEXT" || code.name != "__text" || s->offset >= code.size() ||
          range == 0 || !std::in_range<std::int32_t>(range) || range > code.size() - s->offset)
        return "ELF handler frame range is outside its local code";
      if (common->second) {
        const auto* lsda = local_anchor(at + 17);
        if (!lsda || load<std::uint32_t>(record, 17) != 0 || &o.sections[*lsda->section] != &*tables ||
            lsda->offset >= tables->size() || tables->bytes[lsda->offset] != 0xff)
          return "invalid ELF handler LSDA relocation";
        has_handler = true;
      }
    }
    at += static_cast<std::uint32_t>(size);
  }
  if (!has_handler || !relocs.empty()) return "missing or unexpected ELF handler frame relocation";
  return std::nullopt;
}

std::string_view string_data(std::span<const std::uint8_t> table) {
  return {reinterpret_cast<const char*>(table.data()), table.size()};
}

std::string_view name_at(std::string_view table, std::uint32_t offset) {
  const auto tail = table.substr(offset);
  return tail.substr(0, tail.find('\0'));
}

void write_header(bytes& file, std::uint64_t shoff, std::uint16_t shnum, std::uint16_t shstrndx) {
  bytes header(elf_ident.begin(), elf_ident.end());  // ELFCLASS64, ELFDATA2LSB, EV_CURRENT, ELFOSABI_NONE
  header.resize(16, 0);
  append(header, et_rel);
  append(header, em_x86_64);
  append(header, std::uint32_t{1});            // e_version
  append(header, std::uint64_t{0});            // e_entry
  append(header, std::uint64_t{0});            // e_phoff
  append(header, shoff);
  append(header, std::uint32_t{0});            // e_flags
  append(header, header_size);
  append(header, std::uint16_t{0});            // e_phentsize
  append(header, std::uint16_t{0});            // e_phnum
  append(header, section_header_size);
  append(header, shnum);
  append(header, shstrndx);
  std::ranges::copy(header, file.begin());
}

}  // namespace

std::expected<bytes, std::string> write_object(const object& o) {
  auto fail = [&](std::string reason) {
    return std::unexpected(std::format("{}: {}", o.name.empty() ? "ELF object" : o.name, reason));
  };
  if (o.arch != target::x86_64_linux) return fail("expected an x86-64 Linux object");
  if (auto error = handler_frame_error(o)) return fail(std::string(*error));

  // The null section, .note.GNU-stack, .symtab, .strtab and .shstrtab are always present.
  const std::size_t count = 5 + o.comdat_groups.size() + o.sections.size() +
      std::ranges::count_if(o.sections, [](const section& s) { return !s.relocs.empty(); });
  if (count >= shn_loreserve) return fail("extended ELF section numbering is unsupported");
  if (o.symbols.size() >= std::numeric_limits<std::uint32_t>::max()) return fail("too many ELF symbols");
  const auto symtab_index = static_cast<std::uint32_t>(count - 3);
  const auto strtab_index = symtab_index + 1;
  const auto first_content_index = static_cast<std::uint32_t>(1 + o.comdat_groups.size());
  // Group descriptors name the RELA sections before they can be emitted. Place them in the
  // same target-section order as the relocation pass, after all the content sections.
  std::vector<std::uint32_t> rela_index(o.sections.size());
  auto next_rela = first_content_index + static_cast<std::uint32_t>(o.sections.size());
  for (std::size_t i = 0; i < o.sections.size(); ++i)
    if (!o.sections[i].relocs.empty()) rela_index[i] = next_rela++;

  bytes file(header_size);
  bytes shstrtab{0};
  std::vector<section_header> sections(1);
  sections.reserve(count);
  auto add_section = [&](std::string_view name, section_header header, std::span<const std::uint8_t> contents) {
    header.name = append_name(shstrtab, name);
    header.offset = (file.size() + header.align - 1) & ~(header.align - 1);
    if (header.type != sht_nobits) {
      file.resize(header.offset);
      file.insert(file.end(), contents.begin(), contents.end());
    }
    sections.push_back(header);
  };

  // The gABI requires each descriptor to precede its members in the section header table.
  std::vector<std::uint32_t> group_of(o.sections.size());
  std::set<std::string_view> signatures;
  for (const auto& group : o.comdat_groups) {
    const auto name = group.signature.view();
    if (name.empty() || !signatures.insert(name).second) return fail("empty or duplicate ELF COMDAT signature");
    if (group.members.size() != 1 || group.members.front() >= o.sections.size())
      return fail("ELF COMDAT output requires one valid member section");
    const auto member = group.members.front();
    if (group_of[member] != 0) return fail("ELF section belongs to multiple COMDAT groups");
    group_of[member] = static_cast<std::uint32_t>(sections.size());
    bytes words;
    append(words, grp_comdat);
    append(words, first_content_index + member);
    if (rela_index[member] != 0) append(words, rela_index[member]);
    // Fill sh_info once the signature has an output symbol index.
    add_section(".group", {.type = sht_group, .size = words.size(), .link = symtab_index,
                           .align = 4, .entsize = 4}, words);
  }

  std::vector<std::string_view> section_names;
  section_names.reserve(o.sections.size());
  for (const section& s : o.sections) {
    const auto mapping = std::ranges::find_if(standard_sections, [&](const section_mapping& m) {
      return s.segment == m.segment && s.name == m.section;
    });
    if (mapping == standard_sections.end()) return fail(std::format("unsupported ELF section {},{}", s.segment, s.name));
    const bool grouped = group_of[section_names.size()] != 0;
    if (grouped) {
      const bool supported = s.relocs.empty() ? mapping->kind == standard_section::rodata
                                              : mapping->kind == standard_section::data_const;
      if (!supported || s.flags != section_flags::regular || s.zerofill_size != 0)
        return fail("ELF COMDAT output requires relocation-free .rodata or relocated .data.rel.ro");
    }
    if ((mapping->type == sht_nobits) != s.is_zerofill() ||
        (s.is_zerofill() && (!s.bytes.empty() || !s.relocs.empty()))) {
      return fail(std::format("invalid zero-fill section {}", mapping->name));
    }
    if (s.align_log2 >= std::numeric_limits<std::ptrdiff_t>::digits)
      return fail(std::format("alignment of section {} exceeds the ELF writer's limit", mapping->name));
    const bool init_array = mapping->type == sht_init_array;
    if (init_array && (s.flags != section_flags::regular || s.zerofill_size != 0 || s.align_log2 != 3 || s.size() % 8 != 0))
      return fail("invalid ELF .init_array flags, alignment or size");
    if (s.is_eh_frame() && (s.flags != section_flags::regular || s.zerofill_size != 0 ||
                           !valid_unwind_section(std::uint64_t{1} << s.align_log2, s.size())))
      return fail("invalid ELF .eh_frame flags, alignment or size");
    section_names.push_back(mapping->name);
    add_section(mapping->name, {.type = mapping->type, .flags = mapping->flags | (grouped ? shf_group : 0), .size = s.size(),
                                .align = std::uint64_t{1} << s.align_log2, .entsize = init_array ? 8u : 0u}, s.bytes);
  }

  // The ABI puts local symbols before nonlocal symbols. Keep input order within each group,
  // and remember the new indices for relocations.
  bytes symtab(symbol_size);
  bytes strtab{0};
  std::vector<std::uint32_t> symbol_indices(o.symbols.size());
  std::uint32_t next_symbol = 1;
  std::uint32_t first_nonlocal = 1;
  for (bool local : {true, false}) {
    for (std::size_t i = 0; i < o.symbols.size(); ++i) {
      const symbol& s = o.symbols[i];
      if ((s.bind == binding::local) != local) continue;
      const auto name = s.name_view();
      if (name.contains('\0')) return fail("ELF symbol name contains a NUL byte");
      if (name.size() >= std::numeric_limits<std::uint32_t>::max() - strtab.size())
        return fail("ELF symbol string table is too large");
      if (local && (s.weak || !s.defined())) return fail(std::format("invalid local symbol '{}'", name));
      if (s.defined() && (*s.section >= o.sections.size() || s.offset > o.sections[*s.section].size()))
        return fail(std::format("symbol '{}' is outside its section", name));
      const auto section_index = s.defined() ? first_content_index + *s.section : 0;
      if (s.function_size) {
        if (!s.defined() || !sections[section_index].contains_code_at(s.offset))
          return fail(std::format("function symbol '{}' is not in executable code", name));
        if (*s.function_size > sections[section_index].size - s.offset)
          return fail(std::format("function symbol '{}' is outside its section", name));
      }
      if (s.object_size) {
        if (!s.defined() || s.function_size)
          return fail(std::format("invalid ELF object symbol '{}'", name));
        if (*s.object_size > sections[section_index].size - s.offset)
          return fail(std::format("object symbol '{}' is outside its section", name));
      }

      const auto group_index = s.defined() ? group_of[*s.section] : 0;
      if (group_index != 0 && name == o.comdat_groups[group_index - 1].signature.view()) {
        auto& group = sections[group_index];
        if (group.info != 0 || s.bind != binding::global || !s.weak || s.object_size.value_or(0) == 0)
          return fail(std::format("invalid ELF COMDAT signature symbol '{}'", name));
        group.info = next_symbol;
      }

      symbol_indices[i] = next_symbol++;
      append(symtab, append_name(strtab, name));
      const auto bind = local ? stb_local : s.weak ? stb_weak : stb_global;
      const auto type = s.function_size ? stt_func : s.object_size ? stt_object : stt_notype;
      append(symtab, static_cast<std::uint8_t>(bind << 4 | type));
      append(symtab, s.bind == binding::hidden ? stv_hidden : stv_default);
      append(symtab, static_cast<std::uint16_t>(section_index));  // SHN_UNDEF is 0
      append(symtab, s.defined() ? s.offset : std::uint64_t{0});
      append(symtab, s.function_size.value_or(s.object_size.value_or(0)));
    }
    if (local) first_nonlocal = next_symbol;
  }
  for (std::uint32_t i = 1; i < first_content_index; ++i)
    if (sections[i].info == 0)
      return fail(std::format("missing ELF COMDAT signature symbol '{}'", o.comdat_groups[i - 1].signature.view()));

  for (std::size_t i = 0; i < o.sections.size(); ++i) {
    const section& s = o.sections[i];
    if (rela_index[i] == 0) continue;
    contract_assert(sections.size() == rela_index[i]);
    const bool grouped = group_of[i] != 0;
    bytes rela;
    std::optional<std::uint32_t> previous_offset;
    for (const reloc& r : s.relocs) {
      const auto mapping = std::ranges::find(relocations, r.kind, &relocation_mapping::kind);
      if (mapping == relocations.end() || r.minus)
        return fail(std::format("unsupported x86-64 relocation in section {}", section_names[i]));
      if (r.offset > s.size() || mapping->width > s.size() - r.offset)
        return fail(std::format("relocation outside section {}", section_names[i]));
      if (r.symbol >= symbol_indices.size()) return fail(std::format("invalid relocation symbol in section {}", section_names[i]));
      const auto& target = o.symbols[r.symbol];
      if (grouped && (r.kind != reloc_kind::abs64 || target.name_view().empty() || target.bind == binding::local))
        return fail("ELF COMDAT relocation requires ABS64 to a named nonlocal symbol");
      if (grouped && r.offset == previous_offset) return fail("composed ELF COMDAT relocations are unsupported");
      previous_offset = r.offset;
      if (target.bind == binding::local && target.section && group_of[*target.section] != 0)
        return fail(std::format("relocation in section {} references a local ELF COMDAT symbol", section_names[i]));
      append(rela, std::uint64_t{r.offset});
      append(rela, std::uint64_t{symbol_indices[r.symbol]} << 32 | mapping->type);
      append(rela, static_cast<std::uint64_t>(r.addend));
    }
    add_section(std::format(".rela{}", section_names[i]),
                {.type = sht_rela, .flags = shf_info_link | (grouped ? shf_group : 0), .size = rela.size(), .link = symtab_index,
                 .info = first_content_index + static_cast<std::uint32_t>(i), .align = 8, .entsize = rela_size}, rela);
  }

  // An empty, non-executable stack marker for Linux linkers.
  add_section(".note.GNU-stack", {.type = sht_progbits, .align = 1}, {});
  add_section(".symtab", {.type = sht_symtab, .size = symtab.size(), .link = strtab_index,
                         .info = first_nonlocal, .align = 8, .entsize = symbol_size}, symtab);
  add_section(".strtab", {.type = sht_strtab, .size = strtab.size(), .align = 1}, strtab);
  const auto shstrtab_name = append_name(shstrtab, ".shstrtab");
  sections.push_back({.name = shstrtab_name,
                      .type = sht_strtab, .offset = file.size(), .size = shstrtab.size(), .align = 1});
  file.insert(file.end(), shstrtab.begin(), shstrtab.end());
  const auto shoff = (file.size() + 7) & ~std::uint64_t{7};
  file.resize(shoff);
  for (const section_header& header : sections) header.write(file);
  write_header(file, shoff, static_cast<std::uint16_t>(sections.size()), static_cast<std::uint16_t>(sections.size() - 1));
  return file;
}

std::expected<object, std::string> read_object(std::span<const std::uint8_t> file, std::string name) {
  auto fail = [&](std::string_view reason) {
    return std::unexpected(std::format("{}: {}", name.empty() ? "ELF object" : name, reason));
  };
  auto in_file = [&](std::uint64_t offset, std::uint64_t size) {
    return offset <= file.size() && size <= file.size() - offset;
  };
  if (file.size() < header_size) return fail("truncated ELF header");
  if (file.size() > std::numeric_limits<std::ptrdiff_t>::max()) return fail("ELF file is too large");
  if (!std::ranges::equal(file.first(elf_ident.size()), elf_ident))
    return fail("expected a little-endian ELF64 object with the System V ABI");
  if (load<std::uint16_t>(file, 16) != et_rel || load<std::uint16_t>(file, 18) != em_x86_64)
    return fail("expected an x86-64 relocatable ELF object (ET_REL)");
  if (load<std::uint32_t>(file, 20) != 1 || load<std::uint16_t>(file, 52) != header_size ||
      load<std::uint16_t>(file, 58) != section_header_size)
    return fail("invalid ELF version or header size");
  if (load<std::uint64_t>(file, 24) || load<std::uint64_t>(file, 32) ||
      load<std::uint32_t>(file, 48) || load<std::uint16_t>(file, 56))
    return fail("unsupported ELF entry point, program headers or machine flags");

  const auto shoff = load<std::uint64_t>(file, 40);
  const auto shnum = load<std::uint16_t>(file, 60);
  const auto shstrndx = load<std::uint16_t>(file, 62);
  if (shnum == 0 || shnum >= shn_loreserve || shstrndx >= shn_loreserve)
    return fail("missing or extended ELF section numbering is unsupported");
  if (shstrndx == 0 || shstrndx >= shnum) return fail("invalid section name table index");
  const auto table_size = std::uint64_t{shnum} * section_header_size;
  if (shoff < header_size || shoff % 8 != 0 || !in_file(shoff, table_size))
    return fail("invalid or truncated ELF section header table");
  if (!all_zero(file.subspan(shoff, section_header_size))) return fail("invalid null ELF section header");

  std::vector<section_header> headers(shnum);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges{{0, header_size}, {shoff, shoff + table_size}};
  std::uint32_t symtab_index = 0;
  for (std::uint32_t i = 1; i < shnum; ++i) {
    const auto record = file.subspan(shoff + i * section_header_size, section_header_size);
    auto& h = headers[i];
    h = {.name = load<std::uint32_t>(record, 0), .type = load<std::uint32_t>(record, 4),
         .flags = load<std::uint64_t>(record, 8), .offset = load<std::uint64_t>(record, 24),
         .size = load<std::uint64_t>(record, 32), .link = load<std::uint32_t>(record, 40),
         .info = load<std::uint32_t>(record, 44), .align = load<std::uint64_t>(record, 48),
         .entsize = load<std::uint64_t>(record, 56)};
    if (load<std::uint64_t>(record, 16) != 0) return fail(std::format("nonzero address for ELF section {}", i));
    if (h.align != 0 && (!std::has_single_bit(h.align) ||
                         std::countr_zero(h.align) >= std::numeric_limits<std::ptrdiff_t>::digits))
      return fail(std::format("unsupported alignment for ELF section {}", i));
    if (h.type != sht_nobits) {
      if (!in_file(h.offset, h.size)) return fail(std::format("ELF section {} lies outside the file", i));
      if (h.size != 0) ranges.emplace_back(h.offset, h.offset + h.size);
    }
    if (h.type == sht_symtab) {
      if (symtab_index != 0) return fail("multiple ELF symbol tables are unsupported");
      symtab_index = i;
    }
  }
  std::ranges::sort(ranges);
  for (std::size_t i = 1; i < ranges.size(); ++i)
    if (ranges[i].first < ranges[i - 1].second) return fail("overlapping ELF file sections");
  if (symtab_index == 0) return fail("missing ELF symbol table");

  auto contents = [&](const section_header& h) { return file.subspan(h.offset, h.size); };
  const auto& names_header = headers[shstrndx];
  const auto& symtab = headers[symtab_index];
  if (names_header.size == 0 || !valid_string_table(names_header, contents(names_header)))
    return fail("invalid ELF section name string table");
  if (symtab.link == 0 || symtab.link >= shnum || headers[symtab.link].type != sht_strtab ||
      !valid_string_table(headers[symtab.link], contents(headers[symtab.link])))
    return fail("invalid ELF symbol name string table");
  const auto names = string_data(contents(names_header));
  const auto symbol_names = string_data(contents(headers[symtab.link]));
  const auto symbol_count = symtab.size / symbol_size;
  if (symtab.flags != 0 || symtab.entsize != symbol_size || symtab.size % symbol_size != 0 ||
      symbol_count == 0 || symbol_count > std::numeric_limits<std::uint32_t>::max() ||
      symtab.info == 0 || symtab.info > symbol_count)
    return fail("invalid ELF symbol table");
  const auto symbol_table = contents(symtab);
  if (!all_zero(symbol_table.first(symbol_size))) return fail("invalid null ELF symbol");

  const auto name_storage = std::make_shared<const std::string>(symbol_names);
  std::vector<std::size_t> name_lengths(symbol_count);
  {
    struct name_index { std::uint32_t offset, symbol; };
    std::vector<name_index> named;
    for (std::uint32_t i = 1; i < symbol_count; ++i) {
      const auto offset = load<std::uint32_t>(symbol_table, symbol_size * i);
      if (offset != 0 && offset < symbol_names.size()) named.push_back({offset, i});
    }
    std::ranges::sort(named, {}, &name_index::offset);
    std::size_t end = 0;
    for (const auto [offset, symbol] : named) {
      if (offset > end) end = symbol_names.find('\0', offset);
      name_lengths[symbol] = end - offset;
    }
  }
  const auto read_name = [&](std::uint32_t symbol, std::uint32_t offset) {
    return offset == 0 ? symbol_name{} : symbol_name(name_storage, offset, name_lengths[symbol]);
  };

  object o;
  o.arch = target::x86_64_linux;
  // Keep section-header identities until group flags and the relocation domain are validated.
  std::vector<std::uint32_t> group_owners(shnum);
  std::vector<bool> signature_symbols(symbol_count);
  for (std::uint32_t i = 1; i < shnum; ++i) {
    const auto& h = headers[i];
    if (h.type != sht_group) continue;
    if (h.flags != 0 || h.link != symtab_index || h.entsize != 4 || h.size < 8 || h.size % 4 != 0 ||
        (h.align != 0 && h.offset % h.align != 0))
      return fail(std::format("invalid ELF group section {}", i));
    if (h.info == 0 || h.info >= symbol_count) return fail(std::format("invalid ELF group signature index in section {}", i));
    const auto name_index = load<std::uint32_t>(symbol_table, symbol_size * h.info);
    if (name_index >= symbol_names.size() || symbol_names[name_index] == '\0')
      return fail(std::format("invalid ELF group signature name in section {}", i));
    signature_symbols[h.info] = true;
    const auto words = contents(h);
    if (load<std::uint32_t>(words, 0) != grp_comdat) return fail(std::format("unsupported ELF group flags in section {}", i));
    o.comdat_groups.push_back({read_name(h.info, name_index), {}});
    for (std::size_t offset = 4; offset < words.size(); offset += 4) {
      const auto member = load<std::uint32_t>(words, offset);
      if (member <= i || member >= shnum || (headers[member].flags & shf_group) == 0 || group_owners[member] != 0)
        return fail(std::format("invalid ELF group member {} in section {}", member, i));
      const auto type = headers[member].type;
      if (type != sht_progbits && type != sht_nobits && type != sht_init_array && type != sht_rela)
        return fail(std::format("unsupported ELF group member type in section {}", member));
      group_owners[member] = static_cast<std::uint32_t>(o.comdat_groups.size());
    }
  }
  std::vector<std::optional<std::uint32_t>> sections(shnum);
  std::vector<bool> debug_sections(shnum);
  for (std::uint32_t i = 1; i < shnum; ++i) {
    const auto& h = headers[i];
    if (h.name >= names.size()) return fail(std::format("invalid name for ELF section {}", i));
    const auto owner = group_owners[i];
    if ((h.flags & shf_group) != 0 && owner == 0)
      return fail(std::format("ELF section {} has SHF_GROUP without a group", i));
    if (i == shstrndx || i == symtab_index || i == symtab.link || h.type == sht_rela || h.type == sht_group) continue;
    const auto section_name = name_at(names, h.name);
    if (section_name == ".note.GNU-stack" && h.type == sht_progbits && h.flags == 0 &&
        h.size == 0 && h.link == 0 && h.info == 0 && h.entsize == 0) continue;
    if ((section_name.starts_with(".debug_") || section_name == ".comment") && plain_metadata(h)) {
      debug_sections[i] = section_name.starts_with(".debug_");
      continue;
    }
    const auto canonical_name = section_name == ".data.rel.ro.local" ? ".data.rel.ro" : section_name;
    const auto mapping = std::ranges::find_if(standard_sections, [&](const section_mapping& candidate) {
      return canonical_name == candidate.name ||
             (owner != 0 && candidate.type != sht_init_array && candidate.type != sht_x86_64_unwind &&
              canonical_name.starts_with(candidate.name) &&
              canonical_name.size() > candidate.name.size() + 1 && canonical_name[candidate.name.size()] == '.');
    });
    // Accept the ABI section type and the same ungrouped section spelled SHT_PROGBITS.
    const bool unwind = mapping != standard_sections.end() && mapping->type == sht_x86_64_unwind;
    if (mapping == standard_sections.end() || (h.type != mapping->type && !(unwind && h.type == sht_progbits)) ||
        (h.flags & ~shf_group) != mapping->flags ||
        h.link != 0 || h.info != 0 || (h.entsize != 0 && !(h.type == sht_init_array && h.entsize == 8)))
      return fail(std::format("unsupported ELF section '{}'", section_name));
    if (h.type == sht_init_array && (h.align != 8 || h.size % 8 != 0))
      return fail("invalid ELF .init_array alignment or size");
    if (unwind && (owner != 0 || !valid_unwind_section(h.align, h.size) || h.offset % h.align != 0))
      return fail("invalid ELF .eh_frame group, alignment or size");
    if (mapping->kind == standard_section::gcc_except_table && owner != 0)
      return fail("ELF handler tables cannot be grouped");
    auto s = section::standard(mapping->kind);
    s.align_log2 = static_cast<std::uint8_t>(std::countr_zero(h.align == 0 ? std::uint64_t{1} : h.align));
    if (h.type == sht_nobits) s.zerofill_size = h.size;
    else s.emit(contents(h));
    sections[i] = o.add_section(std::move(s));
    if (owner != 0) o.comdat_groups[owner - 1].members.push_back(*sections[i]);
  }

  for (const auto& group : o.comdat_groups)
    if (group.members.empty())
      return fail(std::format("ELF COMDAT group '{}' has no content sections", group.signature.view()));
  o.symbols.reserve(symbol_count - 1);
  struct input_symbol {
    std::optional<std::uint32_t> retained;
    bool debug = false;
    std::uint32_t local_group = 0;
  };
  std::vector<input_symbol> symbols(symbol_count);
  for (std::uint32_t i = 1; i < symbol_count; ++i) {
    const auto record = symbol_table.subspan(symbol_size * i, symbol_size);
    const auto name_index = load<std::uint32_t>(record, 0);
    const auto bind = record[4] >> 4;
    const auto type = record[4] & 0xf;
    const auto visibility = record[5];
    const auto index = load<std::uint16_t>(record, 6);
    const auto value = load<std::uint64_t>(record, 8);
    const auto size = load<std::uint64_t>(record, 16);
    if (name_index != 0 && name_index >= symbol_names.size()) return fail(std::format("invalid name for ELF symbol {}", i));
    const bool local = bind == stb_local;
    if ((type != stt_notype && type != stt_object && type != stt_func && type != stt_section && type != stt_file) ||
        (type != stt_object && type != stt_func && size != 0) || bind > stb_weak ||
        (visibility != stv_default && (visibility != stv_hidden || local)))
      return fail(std::format("unsupported type, size, binding or visibility for ELF symbol {}", i));
    if ((i < symtab.info) != local) return fail("ELF local symbols must precede nonlocal symbols");
    if (type == stt_file) {
      if (!local || index != shn_abs || value != 0 || name_index == 0 || symbol_names[name_index] == '\0')
        return fail(std::format("invalid ELF file symbol {}", i));
      continue;
    }
    // A local name can serve only as the signature, without denoting allocatable bytes.
    if (signature_symbols[i] && local && type == stt_notype && value == 0 &&
        (index == shn_abs || (index < shnum && headers[index].type == sht_group))) continue;
    if (type == stt_section) {
      if (!local || name_index != 0 || index == 0 || value != 0)
        return fail(std::format("invalid ELF section symbol {}", i));
      if (index < shnum && debug_sections[index]) {
        symbols[i].debug = true;
        continue;
      }
    }
    if (index == 0) {
      if (local || value != 0 || size != 0) return fail(std::format("invalid undefined ELF symbol {}", i));
    } else if (index >= shnum || !sections[index] || value > headers[index].size || size > headers[index].size - value) {
      return fail(std::format("unsupported section index or out-of-section value or size for ELF symbol {}", i));
    }
    std::optional<std::uint64_t> function_size;
    if (type == stt_func && index != 0) {
      if (!headers[index].contains_code_at(value))
        return fail(std::format("ELF function symbol {} is not in executable code", i));
      function_size = size;
    }
    const auto object_size = type == stt_object && group_owners[index] != 0 ? std::optional{size} : std::nullopt;
    const auto model_binding = local ? binding::local : visibility == stv_hidden ? binding::hidden : binding::global;
    symbols[i].retained = o.add_symbol({read_name(i, name_index), model_binding, sections[index], value, bind == stb_weak,
                                       function_size, object_size});
    if (local) symbols[i].local_group = group_owners[index];
  }

  const bool has_handler_tables = std::ranges::any_of(o.sections, &section::is_gcc_except_table);
  std::vector<bool> has_relocations(shnum);
  for (std::uint32_t i = 1; i < shnum; ++i) {
    const auto& h = headers[i];
    if (h.type != sht_rela) continue;
    const auto flags = h.flags & ~shf_group;
    // SHT_RELA specifies the target in sh_info even if a grouped entry omits SHF_INFO_LINK.
    const bool valid_flags = flags == shf_info_link || (group_owners[i] != 0 && flags == 0);
    if (!valid_flags || h.link != symtab_index || h.info >= shnum ||
        (!sections[h.info] && !debug_sections[h.info]) || h.entsize != rela_size || h.size % rela_size != 0)
      return fail(std::format("invalid ELF relocation section {}", i));
    if (group_owners[i] != group_owners[h.info])
      return fail(std::format("ELF relocation section {} and its target have different groups", i));
    if (has_relocations[h.info]) return fail("multiple ELF relocation sections for one target section are unsupported");
    has_relocations[h.info] = true;
    const bool debug = debug_sections[h.info];
    const auto& target_header = headers[h.info];
    if (target_header.type == sht_nobits) return fail("ELF relocations in zero-fill sections are unsupported");
    const auto table = contents(h);
    std::optional<std::uint64_t> previous_offset;
    for (std::size_t at = 0; at < table.size(); at += rela_size) {
      const auto record = table.subspan(at, rela_size);
      const auto site = load<std::uint64_t>(record, 0);
      const auto type = load<std::uint32_t>(record, 8);
      const auto symbol = load<std::uint32_t>(record, 12);
      const auto mapping = std::ranges::find(relocations, type, &relocation_mapping::type);
      if (mapping == relocations.end() && !(debug && type == r_x86_64_32))
        return fail(std::format("unsupported x86-64 relocation type {}", type));
      if (symbol >= symbol_count || (!symbols[symbol].retained && !(debug && symbols[symbol].debug)))
        return fail("invalid ELF relocation symbol");
      if (symbols[symbol].local_group != 0 && symbols[symbol].local_group != group_owners[i])
        return fail(std::format("ELF relocation section {} references a local symbol in a different group", i));
      if (site > std::numeric_limits<std::uint32_t>::max()) return fail("ELF relocation offset exceeds 32 bits");
      const auto width = mapping == relocations.end() ? 4u : mapping->width;
      if (site > target_header.size || width > target_header.size - site)
        return fail("ELF relocation lies outside its section");
      if (site == previous_offset) return fail("composed ELF relocations are unsupported");
      previous_offset = site;
      if (debug) continue;
      auto& relocated = o.sections[*sections[h.info]];
      const auto anchor = *symbols[symbol].retained;
      const auto addend = std::bit_cast<std::int64_t>(load<std::uint64_t>(record, 16));
      if (relocated.is_eh_frame() && !has_handler_tables) {
        const auto& s = o.symbols[anchor];
        if (mapping->kind != reloc_kind::x86_64_pc32 || site % 4 != 0 || s.bind != binding::local || !s.section)
          return fail("ELF .eh_frame requires aligned PC32 relocations to local code");
        const auto& code = o.sections[*s.section];
        const auto delta = static_cast<std::uint64_t>(addend);
        // Bound S + A in its original code section without unsigned wrap.
        const bool in_range = addend < 0 ? s.offset >= 0 - delta && s.offset - (0 - delta) < code.size()
                                        : s.offset < code.size() && delta < code.size() - s.offset;
        if (code.segment != "__TEXT" || code.name != "__text" || !in_range)
          return fail("ELF .eh_frame relocation target is outside local code");
      }
      relocated.relocs.push_back({static_cast<std::uint32_t>(site), mapping->kind, anchor, addend, std::nullopt});
    }
  }

  if (auto error = handler_frame_error(o)) return fail(*error);
  o.name = std::move(name);
  return o;
}

}  // namespace ycxx::obj::elf
