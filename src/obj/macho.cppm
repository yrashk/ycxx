// Mach-O for AArch64 (#42): the format's constants, little-endian byte buffers, and the MH_OBJECT
// writer and reader for `ycxx::obj::object`.
//
// Sources: Apple's published <mach-o/loader.h>, <mach-o/nlist.h>, <mach-o/reloc.h> and
// <mach-o/arm64/reloc.h> (DECISIONS §2.1). The reader exists so that mixed-ABI tests can link
// objects that Clang or GCC produced; it reads them as data, nothing more (DECISIONS §2.3).
//
// Relocations map one to one onto `obj::reloc_kind`, with two pairings the format needs:
// ARM64_RELOC_ADDEND carries the addend of a following BRANCH26, PAGE21 or PAGEOFF12, and
// ARM64_RELOC_SUBTRACTOR carries `minus` for a following UNSIGNED. An UNSIGNED relocation's
// addend is stored in the section bytes. Every relocation the writer emits is external (it names
// a symbol); the reader also accepts section-relative UNSIGNED relocations, which it turns into
// a reference to a nameless local symbol at the start of the section.
export module ycxx.obj.macho;

export import ycxx.obj;
import std;

export namespace ycxx::obj::macho {

// <mach-o/loader.h>
inline constexpr std::uint32_t mh_magic_64 = 0xfeedfacf;
inline constexpr std::uint32_t cpu_type_arm64 = 0x0100000c;
inline constexpr std::uint32_t cpu_subtype_arm64_all = 0;
inline constexpr std::uint32_t mh_object = 0x1;
inline constexpr std::uint32_t mh_execute = 0x2;
inline constexpr std::uint32_t mh_noundefs = 0x1;
inline constexpr std::uint32_t mh_dyldlink = 0x4;
inline constexpr std::uint32_t mh_twolevel = 0x80;
inline constexpr std::uint32_t mh_subsections_via_symbols = 0x2000;
inline constexpr std::uint32_t mh_weak_defines = 0x8000;
inline constexpr std::uint32_t mh_pie = 0x200000;
inline constexpr std::uint32_t lc_req_dyld = 0x80000000;
inline constexpr std::uint32_t lc_segment_64 = 0x19;
inline constexpr std::uint32_t lc_symtab = 0x2;
inline constexpr std::uint32_t lc_dysymtab = 0xb;
inline constexpr std::uint32_t lc_load_dylib = 0xc;
inline constexpr std::uint32_t lc_load_dylinker = 0xe;
inline constexpr std::uint32_t lc_uuid = 0x1b;
inline constexpr std::uint32_t lc_code_signature = 0x1d;
inline constexpr std::uint32_t lc_main = 0x28 | lc_req_dyld;
inline constexpr std::uint32_t lc_build_version = 0x32;
inline constexpr std::uint32_t lc_dyld_exports_trie = 0x33 | lc_req_dyld;
inline constexpr std::uint32_t lc_dyld_chained_fixups = 0x34 | lc_req_dyld;
inline constexpr std::uint32_t platform_macos = 1;
inline constexpr std::uint32_t vm_prot_read = 1;
inline constexpr std::uint32_t vm_prot_write = 2;
inline constexpr std::uint32_t vm_prot_execute = 4;
inline constexpr std::uint32_t sg_read_only = 0x10;
inline constexpr std::uint32_t indirect_symbol_local = 0x80000000;
inline constexpr std::size_t mach_header_64_size = 32;
inline constexpr std::size_t segment_command_64_size = 72;
inline constexpr std::size_t section_64_size = 80;

// <mach-o/nlist.h>
inline constexpr std::uint8_t n_stab = 0xe0;
inline constexpr std::uint8_t n_pext = 0x10;
inline constexpr std::uint8_t n_type = 0x0e;
inline constexpr std::uint8_t n_ext = 0x01;
inline constexpr std::uint8_t n_undf = 0x0;
inline constexpr std::uint8_t n_abs = 0x2;
inline constexpr std::uint8_t n_sect = 0xe;
inline constexpr std::uint16_t n_weak_ref = 0x40;
inline constexpr std::uint16_t n_weak_def = 0x80;
inline constexpr std::uint16_t referenced_dynamically = 0x10;
inline constexpr std::size_t nlist_64_size = 16;

// <mach-o/arm64/reloc.h>
enum class reloc_type : std::uint8_t {
  unsigned_ = 0,
  subtractor = 1,
  branch26 = 2,
  page21 = 3,
  pageoff12 = 4,
  got_load_page21 = 5,
  got_load_pageoff12 = 6,
  pointer_to_got = 7,
  tlvp_load_page21 = 8,
  tlvp_load_pageoff12 = 9,
  addend = 10,
};

// A growing little-endian byte buffer.
class byte_writer {
public:
  std::vector<std::uint8_t> bytes;

  std::size_t size() const noexcept { return bytes.size(); }
  void u8(std::uint8_t v) { bytes.push_back(v); }
  void u16(std::uint16_t v) { put(v, 2); }
  void u32(std::uint32_t v) { put(v, 4); }
  void u64(std::uint64_t v) { put(v, 8); }
  void raw(std::span<const std::uint8_t> b) { bytes.insert(bytes.end(), b.begin(), b.end()); }
  void raw(std::string_view s) { bytes.insert(bytes.end(), s.begin(), s.end()); }
  // A NUL-padded fixed-width name (segment and section names are 16 bytes).
  void name16(std::string_view s) {
    for (std::size_t i = 0; i < 16; ++i) u8(i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
  }
  void uleb(std::uint64_t v) {
    do {
      std::uint8_t b = v & 0x7f;
      v >>= 7;
      if (v != 0) b |= 0x80;
      u8(b);
    } while (v != 0);
  }
  void zeros(std::size_t n) { bytes.resize(bytes.size() + n, 0); }
  void align(std::size_t a) { bytes.resize((bytes.size() + a - 1) / a * a, 0); }

private:
  void put(std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
};

constexpr std::uint32_t load32(std::span<const std::uint8_t> b, std::size_t at) {
  return std::uint32_t{b[at]} | std::uint32_t{b[at + 1]} << 8 | std::uint32_t{b[at + 2]} << 16 |
         std::uint32_t{b[at + 3]} << 24;
}
constexpr std::uint64_t load64(std::span<const std::uint8_t> b, std::size_t at) {
  return std::uint64_t{load32(b, at)} | std::uint64_t{load32(b, at + 4)} << 32;
}
constexpr void store32(std::span<std::uint8_t> b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
constexpr void store64(std::span<std::uint8_t> b, std::size_t at, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// The minimum OS version written into objects' LC_BUILD_VERSION, as xxxx.yy.zz nibbles.
inline constexpr std::uint32_t default_min_os = 0x000c0000;  // macOS 12.0

// Preconditions, checked by contract assertions (codegen bugs, not input errors): the object and
// relocations target arm64 macOS; addends of branch26, page21 and pageoff12 fit
// ARM64_RELOC_ADDEND's 24 bits; GOT loads have no addend.
std::vector<std::uint8_t> write_object(const object& o);
std::expected<object, std::string> read_object(std::span<const std::uint8_t> bytes, std::string name);

}  // namespace ycxx::obj::macho

namespace ycxx::obj::macho {

namespace {

std::uint32_t reloc_word(std::uint32_t symbolnum, bool pcrel, std::uint32_t length_log2, bool external, reloc_type t) {
  return (symbolnum & 0xffffff) | std::uint32_t{pcrel} << 24 | length_log2 << 25 | std::uint32_t{external} << 27 |
         std::uint32_t{std::to_underlying(t)} << 28;
}

struct sorted_symbols {
  std::vector<std::uint32_t> order;     // file index -> model index
  std::vector<std::uint32_t> file_index;  // model index -> file index
  std::uint32_t nlocal = 0, nextdef = 0, nundef = 0;
};

// MH_OBJECT symbol tables are grouped: locals in model order, then external definitions and
// undefined symbols, each sorted by name (stable, so equal names keep model order).
sorted_symbols sort_symbols(const object& o) {
  sorted_symbols s;
  std::vector<std::uint32_t> ext, undef;
  for (std::uint32_t i = 0; i < o.symbols.size(); ++i) {
    const auto& sym = o.symbols[i];
    if (!sym.defined()) undef.push_back(i);
    else if (sym.bind == binding::local) s.order.push_back(i);
    else ext.push_back(i);
  }
  auto by_name = [&](std::uint32_t a, std::uint32_t b) { return o.symbols[a].name_view() < o.symbols[b].name_view(); };
  std::ranges::stable_sort(ext, by_name);
  std::ranges::stable_sort(undef, by_name);
  s.nlocal = static_cast<std::uint32_t>(s.order.size());
  s.nextdef = static_cast<std::uint32_t>(ext.size());
  s.nundef = static_cast<std::uint32_t>(undef.size());
  s.order.insert(s.order.end(), ext.begin(), ext.end());
  s.order.insert(s.order.end(), undef.begin(), undef.end());
  s.file_index.resize(o.symbols.size());
  for (std::uint32_t f = 0; f < s.order.size(); ++f) s.file_index[s.order[f]] = f;
  return s;
}

}  // namespace

std::vector<std::uint8_t> write_object(const object& o) {
  contract_assert(o.arch == target::aarch64_macos);
  const auto syms = sort_symbols(o);
  const std::size_t nsect = o.sections.size();

  // Section addresses in the object's own address space, in section order.
  std::vector<std::uint64_t> addr(nsect);
  std::uint64_t vm = 0;
  for (std::size_t i = 0; i < nsect; ++i) {
    const std::uint64_t a = std::uint64_t{1} << o.sections[i].align_log2;
    vm = (vm + a - 1) & ~(a - 1);
    addr[i] = vm;
    vm += o.sections[i].size();
  }

  const std::uint32_t sizeofcmds =
      static_cast<std::uint32_t>(segment_command_64_size + section_64_size * nsect + 24 + 24 + 80);
  const std::uint64_t data_start = mach_header_64_size + sizeofcmds;

  // Section contents follow the load commands at their address offsets (zero-fill takes no space).
  std::vector<std::uint64_t> file_off(nsect, 0);
  std::uint64_t file_end = data_start;
  for (std::size_t i = 0; i < nsect; ++i) {
    if (o.sections[i].is_zerofill()) continue;
    file_off[i] = data_start + addr[i];
    file_end = std::max(file_end, file_off[i] + o.sections[i].size());
  }
  std::uint64_t file_size_of_segment = 0;
  for (std::size_t i = 0; i < nsect; ++i) {
    if (!o.sections[i].is_zerofill()) file_size_of_segment = std::max(file_size_of_segment, addr[i] + o.sections[i].size());
  }

  // Section bytes with UNSIGNED addends stored, and the relocation entries.
  std::vector<std::vector<std::uint8_t>> contents(nsect);
  std::vector<byte_writer> relocs(nsect);
  for (std::size_t i = 0; i < nsect; ++i) {
    const auto& sec = o.sections[i];
    contents[i] = sec.bytes;
    auto& out = relocs[i];
    auto entry = [&](std::uint32_t at, std::uint32_t word) {
      out.u32(at);
      out.u32(word);
    };
    for (const auto& r : sec.relocs) {
      const std::uint32_t sym = syms.file_index[r.symbol];
      auto with_addend = [&](reloc_type t, bool pcrel) {
        contract_assert(r.addend >= -(std::int64_t{1} << 23) && r.addend < (std::int64_t{1} << 23));
        if (r.addend != 0) entry(r.offset, reloc_word(static_cast<std::uint32_t>(r.addend), false, 2, false, reloc_type::addend));
        entry(r.offset, reloc_word(sym, pcrel, 2, true, t));
      };
      switch (r.kind) {
        case reloc_kind::branch26: with_addend(reloc_type::branch26, true); break;
        case reloc_kind::page21: with_addend(reloc_type::page21, true); break;
        case reloc_kind::pageoff12: with_addend(reloc_type::pageoff12, false); break;
        case reloc_kind::got_page21:
          contract_assert(r.addend == 0);
          entry(r.offset, reloc_word(sym, true, 2, true, reloc_type::got_load_page21));
          break;
        case reloc_kind::got_pageoff12:
          contract_assert(r.addend == 0);
          entry(r.offset, reloc_word(sym, false, 2, true, reloc_type::got_load_pageoff12));
          break;
        case reloc_kind::got_rel32:
          entry(r.offset, reloc_word(sym, true, 2, true, reloc_type::pointer_to_got));
          store32(contents[i], r.offset, static_cast<std::uint32_t>(r.addend));
          break;
        case reloc_kind::abs64:
        case reloc_kind::abs32: {
          const std::uint32_t len = r.kind == reloc_kind::abs64 ? 3 : 2;
          if (r.minus) entry(r.offset, reloc_word(syms.file_index[*r.minus], false, len, true, reloc_type::subtractor));
          entry(r.offset, reloc_word(sym, false, len, true, reloc_type::unsigned_));
          if (len == 3) store64(contents[i], r.offset, static_cast<std::uint64_t>(r.addend));
          else store32(contents[i], r.offset, static_cast<std::uint32_t>(r.addend));
          break;
        }
        case reloc_kind::x86_64_pc32:
        case reloc_kind::x86_64_plt32:
        case reloc_kind::x86_64_rex_gotpcrelx:
          contract_assert(false && "x86-64 relocation in an arm64 Mach-O object");
          std::unreachable();
      }
    }
  }

  // String table: a leading space and NUL as Apple's tools write it, then names in file order.
  byte_writer strtab;
  strtab.u8(' ');
  strtab.u8(0);
  std::vector<std::uint32_t> strx(syms.order.size());
  for (std::size_t f = 0; f < syms.order.size(); ++f) {
    const auto name = o.symbols[syms.order[f]].name_view();
    if (name.empty()) continue;
    strx[f] = static_cast<std::uint32_t>(strtab.size());
    strtab.raw(name);
    strtab.u8(0);
  }
  strtab.align(8);

  // Layout after the section contents: relocations, symbol table, string table.
  std::uint64_t cursor = (file_end + 7) & ~std::uint64_t{7};
  std::vector<std::uint64_t> reloff(nsect);
  for (std::size_t i = 0; i < nsect; ++i) {
    reloff[i] = relocs[i].size() == 0 ? 0 : cursor;
    cursor += relocs[i].size();
  }
  const std::uint64_t symoff = cursor;
  cursor += nlist_64_size * syms.order.size();
  const std::uint64_t stroff = cursor;

  byte_writer w;
  w.u32(mh_magic_64);
  w.u32(cpu_type_arm64);
  w.u32(cpu_subtype_arm64_all);
  w.u32(mh_object);
  w.u32(4);  // ncmds
  w.u32(sizeofcmds);
  // A weak definition is coalesced per symbol only where the linker may split sections at
  // symbols; every section's code and data is reached through its own symbols (#124).
  const bool weak_definitions = std::ranges::any_of(o.symbols, [](const symbol& s) { return s.weak && s.defined(); });
  w.u32(weak_definitions ? mh_subsections_via_symbols : 0);  // flags
  w.u32(0);  // reserved

  w.u32(lc_segment_64);
  w.u32(static_cast<std::uint32_t>(segment_command_64_size + section_64_size * nsect));
  w.name16("");
  w.u64(0);
  w.u64(vm);
  w.u64(nsect == 0 ? 0 : data_start);
  w.u64(file_size_of_segment);
  w.u32(vm_prot_read | vm_prot_write | vm_prot_execute);
  w.u32(vm_prot_read | vm_prot_write | vm_prot_execute);
  w.u32(static_cast<std::uint32_t>(nsect));
  w.u32(0);
  for (std::size_t i = 0; i < nsect; ++i) {
    const auto& sec = o.sections[i];
    w.name16(sec.name);
    w.name16(sec.segment);
    w.u64(addr[i]);
    w.u64(sec.size());
    w.u32(static_cast<std::uint32_t>(file_off[i]));
    w.u32(sec.align_log2);
    w.u32(static_cast<std::uint32_t>(reloff[i]));
    w.u32(static_cast<std::uint32_t>(relocs[i].size() / 8));
    w.u32(sec.flags);
    w.u32(0);
    w.u32(0);
    w.u32(0);
  }

  w.u32(lc_build_version);
  w.u32(24);
  w.u32(platform_macos);
  w.u32(default_min_os);
  w.u32(0);  // sdk
  w.u32(0);  // ntools

  w.u32(lc_symtab);
  w.u32(24);
  w.u32(static_cast<std::uint32_t>(symoff));
  w.u32(static_cast<std::uint32_t>(syms.order.size()));
  w.u32(static_cast<std::uint32_t>(stroff));
  w.u32(static_cast<std::uint32_t>(strtab.size()));

  w.u32(lc_dysymtab);
  w.u32(80);
  w.u32(0);
  w.u32(syms.nlocal);
  w.u32(syms.nlocal);
  w.u32(syms.nextdef);
  w.u32(syms.nlocal + syms.nextdef);
  w.u32(syms.nundef);
  for (int i = 0; i < 12; ++i) w.u32(0);  // toc, modules, referenced, indirect, external and local relocations

  contract_assert(w.size() == data_start);
  w.bytes.resize(file_end, 0);
  for (std::size_t i = 0; i < nsect; ++i) {
    if (!o.sections[i].is_zerofill()) std::ranges::copy(contents[i], w.bytes.begin() + static_cast<std::ptrdiff_t>(file_off[i]));
  }
  w.align(8);
  for (auto& r : relocs) w.raw(r.bytes);
  for (std::size_t f = 0; f < syms.order.size(); ++f) {
    const auto& sym = o.symbols[syms.order[f]];
    std::uint8_t type = sym.defined() ? n_sect : n_undf;
    if (sym.bind != binding::local) type |= n_ext;
    if (sym.bind == binding::hidden) type |= n_pext;
    std::uint16_t desc = 0;
    if (sym.weak) desc |= sym.defined() ? n_weak_def : n_weak_ref;
    w.u32(strx[f]);
    w.u8(type);
    w.u8(sym.defined() ? static_cast<std::uint8_t>(*sym.section + 1) : 0);
    w.u16(desc);
    w.u64(sym.defined() ? addr[*sym.section] + sym.offset : 0);
  }
  w.raw(strtab.bytes);
  return std::move(w.bytes);
}

namespace {

std::string fixed_name(std::span<const std::uint8_t> b, std::size_t at) {
  std::string s;
  for (std::size_t i = 0; i < 16 && b[at + i] != 0; ++i) s.push_back(static_cast<char>(b[at + i]));
  return s;
}

std::int64_t sign_extend(std::uint64_t v, int bits) {
  const std::uint64_t m = std::uint64_t{1} << (bits - 1);
  v &= (m << 1) - 1;
  return static_cast<std::int64_t>((v ^ m) - m);
}

}  // namespace

std::expected<object, std::string> read_object(std::span<const std::uint8_t> b, std::string name) {
  auto fail = [&](std::string_view why) { return std::unexpected(std::format("{}: {}", name, why)); };
  auto in_bounds = [&](std::uint64_t off, std::uint64_t len) { return off <= b.size() && len <= b.size() - off; };
  if (!in_bounds(0, mach_header_64_size) || load32(b, 0) != mh_magic_64) return fail("not a 64-bit Mach-O file");
  if (load32(b, 4) != cpu_type_arm64) return fail("not an arm64 object");
  if (load32(b, 12) != mh_object) return fail("not a relocatable object (MH_OBJECT)");
  const std::uint32_t ncmds = load32(b, 16);

  object o;
  o.name = name;
  struct raw_section {
    std::uint64_t addr, size;
    std::uint32_t offset, reloff, nreloc;
  };
  std::vector<raw_section> raw;
  std::uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
  std::size_t at = mach_header_64_size;
  for (std::uint32_t c = 0; c < ncmds; ++c) {
    if (!in_bounds(at, 8)) return fail("truncated load commands");
    const std::uint32_t cmd = load32(b, at), size = load32(b, at + 4);
    if (size < 8 || !in_bounds(at, size)) return fail("bad load command size");
    if (cmd == lc_segment_64) {
      if (size < segment_command_64_size) return fail("bad segment command");
      const std::uint32_t n = load32(b, at + 64);
      if (size < segment_command_64_size + section_64_size * n) return fail("bad segment command");
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t s = at + segment_command_64_size + section_64_size * i;
        section sec;
        sec.name = fixed_name(b, s);
        sec.segment = fixed_name(b, s + 16);
        const std::uint64_t addr = load64(b, s + 32), sz = load64(b, s + 40);
        const std::uint32_t offset = load32(b, s + 48);
        const std::uint32_t align = load32(b, s + 52);
        if (align > 15) return fail(std::format("section {},{}: alignment 2^{} is too large", sec.segment, sec.name, align));
        sec.align_log2 = static_cast<std::uint8_t>(align);
        sec.flags = load32(b, s + 64);
        if (sec.is_zerofill()) sec.zerofill_size = sz;
        else {
          if (!in_bounds(offset, sz)) return fail(std::format("section {},{} lies outside the file", sec.segment, sec.name));
          sec.bytes.assign(b.begin() + offset, b.begin() + static_cast<std::ptrdiff_t>(offset + sz));
        }
        raw.push_back({addr, sz, offset, load32(b, s + 56), load32(b, s + 60)});
        o.sections.push_back(std::move(sec));
      }
    } else if (cmd == lc_symtab) {
      if (size < 24) return fail("bad symbol table command");
      symoff = load32(b, at + 8);
      nsyms = load32(b, at + 12);
      stroff = load32(b, at + 16);
      strsize = load32(b, at + 20);
    }
    at += size;
  }
  if (!in_bounds(symoff, std::uint64_t{nsyms} * nlist_64_size) || !in_bounds(stroff, strsize)) return fail("symbol table lies outside the file");

  // Symbols, keeping a map from file index to model index (stabs are dropped).
  std::vector<std::optional<std::uint32_t>> model_index(nsyms);
  for (std::uint32_t i = 0; i < nsyms; ++i) {
    const std::size_t s = symoff + nlist_64_size * i;
    const std::uint32_t strx = load32(b, s);
    const std::uint8_t type = b[s + 4], sect = b[s + 5];
    const std::uint16_t desc = static_cast<std::uint16_t>(b[s + 6] | b[s + 7] << 8);
    const std::uint64_t value = load64(b, s + 8);
    if (type & n_stab) continue;
    if (strx >= strsize && strx != 0) return fail("symbol name lies outside the string table");
    std::string name;
    for (std::size_t k = stroff + strx; strx != 0 && k < stroff + strsize && b[k] != 0; ++k)
      name.push_back(static_cast<char>(b[k]));
    symbol sym;
    sym.name = std::move(name);
    sym.bind = !(type & n_ext) ? binding::local : (type & n_pext) ? binding::hidden : binding::global;
    switch (type & n_type) {
      case n_undf:
        if (value != 0) {
          // A tentative definition (common symbol): a weak definition alone in a zero-fill
          // __DATA,__common section, which the linker merges to the largest size.
          if (!(type & n_ext)) return fail(std::format("common symbol {} is not external", sym.name_view()));
          auto common = section::standard(standard_section::bss);
          common.name = "__common";
          common.align_log2 = static_cast<std::uint8_t>((desc >> 8) & 0x0f);  // GET_COMM_ALIGN
          common.zerofill_size = value;
          sym.section = o.add_section(std::move(common));
          sym.weak = true;
          break;
        }
        sym.weak = (desc & n_weak_ref) != 0;
        break;
      case n_sect:
        if (sect == 0 || sect > raw.size()) return fail(std::format("symbol {} has a bad section", sym.name_view()));
        sym.section = sect - 1u;
        sym.offset = value - raw[sect - 1].addr;
        sym.weak = (desc & n_weak_def) != 0;
        break;
      default: return fail(std::format("symbol {}: absolute and indirect symbols are not supported", sym.name_view()));
    }
    model_index[i] = o.add_symbol(std::move(sym));
  }

  // A nameless local symbol at the start of each section, for section-relative relocations.
  std::vector<std::optional<std::uint32_t>> section_symbol(raw.size());
  auto section_start = [&](std::uint32_t sect) {
    if (!section_symbol[sect]) section_symbol[sect] = o.add_symbol({"", binding::local, sect, 0, false});
    return *section_symbol[sect];
  };

  for (std::uint32_t si = 0; si < raw.size(); ++si) {
    auto& sec = o.sections[si];
    const auto& rs = raw[si];
    if (!in_bounds(rs.reloff, std::uint64_t{rs.nreloc} * 8)) return fail("relocations lie outside the file");
    std::optional<std::int64_t> pending_addend;
    std::optional<std::uint32_t> pending_minus;
    for (std::uint32_t k = 0; k < rs.nreloc; ++k) {
      const std::size_t e = rs.reloff + 8 * std::size_t{k};
      const std::uint32_t address = load32(b, e), word = load32(b, e + 4);
      if (address & 0x80000000) return fail("scattered relocations are not supported");
      const std::uint32_t symnum = word & 0xffffff;
      const std::uint32_t length = (word >> 25) & 3;
      const bool external = (word >> 27) & 1;
      const auto type = static_cast<reloc_type>(word >> 28);
      if (type == reloc_type::addend) {
        pending_addend = sign_extend(symnum, 24);
        continue;
      }
      const std::uint32_t width = 1u << length;
      if (address > sec.size() || width > sec.size() - address || sec.is_zerofill()) return fail("relocation outside its section");
      // The target as (model symbol, extra addend).
      std::uint32_t target = 0;
      std::int64_t extra = 0;
      if (external) {
        if (symnum >= nsyms || !model_index[symnum]) return fail("relocation names a bad symbol");
        target = *model_index[symnum];
      } else {
        if (type != reloc_type::unsigned_) return fail("section-relative relocations other than UNSIGNED are not supported");
        if (symnum == 0 || symnum > raw.size()) return fail("relocation names a bad section");
        target = section_start(symnum - 1);
        // The stored value is an address in the object; make it relative to the section.
        extra = -static_cast<std::int64_t>(raw[symnum - 1].addr);
      }
      auto take_addend = [&] {
        const std::int64_t a = pending_addend.value_or(0);
        pending_addend.reset();
        return a;
      };
      reloc r{address, reloc_kind::abs64, target, 0, std::nullopt};
      switch (type) {
        case reloc_type::subtractor:
          if (!external) return fail("SUBTRACTOR must name a symbol");
          pending_minus = target;
          continue;
        case reloc_type::unsigned_: {
          if (length != 2 && length != 3) return fail("UNSIGNED relocation of an unsupported width");
          r.kind = length == 3 ? reloc_kind::abs64 : reloc_kind::abs32;
          const std::uint64_t stored = length == 3 ? load64(sec.bytes, address) : load32(sec.bytes, address);
          r.addend = (length == 3 ? static_cast<std::int64_t>(stored) : sign_extend(stored, 32)) + extra;
          r.minus = pending_minus;
          pending_minus.reset();
          std::ranges::fill(std::span(sec.bytes).subspan(address, width), 0);
          break;
        }
        case reloc_type::branch26: r.kind = reloc_kind::branch26; r.addend = take_addend(); break;
        case reloc_type::page21: r.kind = reloc_kind::page21; r.addend = take_addend(); break;
        case reloc_type::pageoff12: r.kind = reloc_kind::pageoff12; r.addend = take_addend(); break;
        case reloc_type::got_load_page21: r.kind = reloc_kind::got_page21; break;
        case reloc_type::got_load_pageoff12: r.kind = reloc_kind::got_pageoff12; break;
        case reloc_type::pointer_to_got:
          if (length != 2 || !((word >> 24) & 1)) return fail("POINTER_TO_GOT other than 32-bit pc-relative is not supported");
          r.kind = reloc_kind::got_rel32;
          r.addend = sign_extend(load32(sec.bytes, address), 32);
          std::ranges::fill(std::span(sec.bytes).subspan(address, 4), 0);
          break;
        default: return fail(std::format("relocation type {} is not supported", std::to_underlying(type)));
      }
      if (pending_minus) return fail("SUBTRACTOR not followed by UNSIGNED");
      if (pending_addend) return fail("ADDEND not followed by BRANCH26, PAGE21 or PAGEOFF12");
      sec.relocs.push_back(r);
    }
    if (pending_minus || pending_addend) return fail("dangling ADDEND or SUBTRACTOR relocation");
    // Mach-O lists relocations in no particular order; the model keeps them by offset.
    std::ranges::stable_sort(sec.relocs, {}, &reloc::offset);
  }
  return o;
}

}  // namespace ycxx::obj::macho
