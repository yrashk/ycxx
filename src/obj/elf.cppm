// ELF64 relocatable objects for x86-64, from the ELF gABI and the System V AMD64 psABI.
export module ycxx.obj.elf;

export import ycxx.obj;
import std;

export namespace ycxx::obj::elf {

// The standard code and data sections, symbols and explicit-addend relocations for x86-64.
// Returns a diagnostic if the in-memory object uses a feature outside this subset.
std::expected<std::vector<std::uint8_t>, std::string> write_object(const object& o);

}  // namespace ycxx::obj::elf

namespace ycxx::obj::elf {
namespace {

constexpr std::uint16_t et_rel = 1, em_x86_64 = 62, shn_loreserve = 0xff00;
constexpr std::uint16_t header_size = 64, section_header_size = 64;
constexpr std::uint64_t symbol_size = 24, rela_size = 24;
constexpr std::uint32_t sht_progbits = 1, sht_symtab = 2, sht_strtab = 3, sht_rela = 4, sht_nobits = 8;
constexpr std::uint64_t shf_write = 1, shf_alloc = 2, shf_execinstr = 4, shf_info_link = 0x40;

struct section_mapping {
  std::string_view segment;
  std::string_view section;
  std::string_view name;
  std::uint32_t type;
  std::uint64_t flags;
};

constexpr std::array standard_sections{
    section_mapping{"__TEXT", "__text", ".text", sht_progbits, shf_alloc | shf_execinstr},
    section_mapping{"__TEXT", "__cstring", ".rodata.str", sht_progbits, shf_alloc},
    section_mapping{"__TEXT", "__const", ".rodata", sht_progbits, shf_alloc},
    section_mapping{"__DATA", "__const", ".data.rel.ro", sht_progbits, shf_alloc | shf_write},
    section_mapping{"__DATA", "__data", ".data", sht_progbits, shf_alloc | shf_write},
    section_mapping{"__DATA", "__bss", ".bss", sht_nobits, shf_alloc | shf_write},
};

using bytes = std::vector<std::uint8_t>;

template <std::unsigned_integral T>
void append(bytes& out, T n) {
  for (std::size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
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

std::optional<std::uint32_t> elf_relocation(reloc_kind kind) {
  switch (kind) {
    case reloc_kind::abs64: return 1;          // R_X86_64_64
    case reloc_kind::x86_64_pc32: return 2;    // R_X86_64_PC32
    case reloc_kind::x86_64_plt32: return 4;   // R_X86_64_PLT32
    default: return std::nullopt;
  }
}

void write_header(bytes& file, std::uint64_t shoff, std::uint16_t shnum, std::uint16_t shstrndx) {
  bytes header{0x7f, 'E', 'L', 'F', 2, 1, 1};  // ELFCLASS64, ELFDATA2LSB, EV_CURRENT
  header.resize(16, 0);                        // ELFOSABI_NONE and padding
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

  // The null section, .note.GNU-stack, .symtab, .strtab and .shstrtab are always present.
  const std::size_t count = 5 + o.sections.size() + std::ranges::count_if(o.sections, [](const section& s) {
    return !s.relocs.empty();
  });
  if (count >= shn_loreserve) return fail("extended ELF section numbering is unsupported");
  if (o.symbols.size() >= std::numeric_limits<std::uint32_t>::max()) return fail("too many ELF symbols");
  const auto symtab_index = static_cast<std::uint32_t>(count - 3);
  const auto strtab_index = symtab_index + 1;

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

  std::vector<std::string_view> section_names;
  section_names.reserve(o.sections.size());
  for (const section& s : o.sections) {
    const auto mapping = std::ranges::find_if(standard_sections, [&](const section_mapping& m) {
      return s.segment == m.segment && s.name == m.section;
    });
    if (mapping == standard_sections.end()) return fail(std::format("unsupported ELF section {},{}", s.segment, s.name));
    if ((mapping->type == sht_nobits) != s.is_zerofill() ||
        (s.is_zerofill() && (!s.bytes.empty() || !s.relocs.empty()))) {
      return fail(std::format("invalid zero-fill section {}", mapping->name));
    }
    if (s.align_log2 >= std::numeric_limits<std::ptrdiff_t>::digits)
      return fail(std::format("alignment of section {} exceeds the ELF writer's limit", mapping->name));
    section_names.push_back(mapping->name);
    add_section(mapping->name, {.type = mapping->type, .flags = mapping->flags, .size = s.size(),
                                .align = std::uint64_t{1} << s.align_log2}, s.bytes);
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
      if (s.name.find('\0') != s.name.npos) return fail("ELF symbol name contains a NUL byte");
      if (s.name.size() >= std::numeric_limits<std::uint32_t>::max() - strtab.size())
        return fail("ELF symbol string table is too large");
      if (local && (s.weak || !s.defined())) return fail(std::format("invalid local symbol '{}'", s.name));
      if (s.defined() && (*s.section >= o.sections.size() || s.offset > o.sections[*s.section].size()))
        return fail(std::format("symbol '{}' is outside its section", s.name));

      symbol_indices[i] = next_symbol++;
      append(symtab, append_name(strtab, s.name));
      const std::uint8_t bind = local ? 0 : s.weak ? 2 : 1;  // STB_LOCAL, STB_WEAK, STB_GLOBAL
      append(symtab, static_cast<std::uint8_t>(bind << 4));  // STT_NOTYPE
      append(symtab, static_cast<std::uint8_t>(s.bind == binding::hidden ? 2 : 0));  // STV_HIDDEN or STV_DEFAULT
      append(symtab, static_cast<std::uint16_t>(s.defined() ? *s.section + 1 : 0));   // SHN_UNDEF is 0
      append(symtab, s.defined() ? s.offset : std::uint64_t{0});
      append(symtab, std::uint64_t{0});  // size unknown
    }
    if (local) first_nonlocal = next_symbol;
  }

  for (std::size_t i = 0; i < o.sections.size(); ++i) {
    const section& s = o.sections[i];
    if (s.relocs.empty()) continue;
    bytes rela;
    for (const reloc& r : s.relocs) {
      const auto type = elf_relocation(r.kind);
      if (!type || r.minus) return fail(std::format("unsupported x86-64 relocation in section {}", section_names[i]));
      const std::uint64_t width = r.kind == reloc_kind::abs64 ? 8 : 4;
      if (r.offset > s.size() || width > s.size() - r.offset)
        return fail(std::format("relocation outside section {}", section_names[i]));
      if (r.symbol >= symbol_indices.size()) return fail(std::format("invalid relocation symbol in section {}", section_names[i]));
      append(rela, std::uint64_t{r.offset});
      append(rela, std::uint64_t{symbol_indices[r.symbol]} << 32 | *type);
      append(rela, static_cast<std::uint64_t>(r.addend));
    }
    add_section(std::format(".rela{}", section_names[i]),
                {.type = sht_rela, .flags = shf_info_link, .size = rela.size(), .link = symtab_index,
                 .info = static_cast<std::uint32_t>(i + 1), .align = 8, .entsize = rela_size}, rela);
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

}  // namespace ycxx::obj::elf
