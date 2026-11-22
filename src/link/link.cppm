// ycxx's own linker (#42): `obj::object`s in, a signed arm64 macOS executable out.
//
// What it does, in order:
//   1. Resolve symbols: definitions across objects (a strong definition beats a weak one; two
//      strong ones are an error), then the remaining undefined names against the dylibs' export
//      lists. A name found nowhere is an error.
//   2. Lay out segments: __PAGEZERO, __TEXT (header, load commands, __text, __stubs, the rest),
//      __DATA_CONST (__got, __DATA,__const, ...), __DATA (data, then zero-fill), __LINKEDIT.
//      Output sections appear in the order of their first contribution, contributions in input
//      order and then section order (query-engine design §A5.3).
//   3. Apply relocations. Calls to imports go through 12-byte stubs, GOT references through
//      __got, and every pointer the loader must adjust or bind becomes a chained fixup
//      (DYLD_CHAINED_PTR_64_OFFSET), so there are no lazy bindings and no stub helpers.
//   4. Write __LINKEDIT: chained fixups, the exports trie, the symbol tables, and the ad-hoc code
//      signature (a SHA-256 code directory over 4 KiB pages) that arm64 macOS requires.
//
// The output is a function of the inputs and the options only: LC_UUID is a fingerprint of the
// unsigned image, and nothing reads clocks, addresses or unordered containers.
//
// Sources: Apple's published <mach-o/loader.h>, <mach-o/nlist.h>, <mach-o/fixup-chains.h>,
// xnu's cs_blobs.h for the signature, and the Arm ARM for the instruction fields relocations
// patch (DECISIONS §2.1).
//
// Incremental linking is not attempted. The seam for it is the per-object contribution table
// (`placed`): an edited object that keeps its section sizes could be re-placed and re-relocated
// alone, with only the fixup chains, the UUID and the signature recomputed.
export module ycxx.link;

export import ycxx.obj;
import ycxx.obj.macho;
import ycxx.link.sha256;
import ycxx.hash;
import std;

export namespace ycxx::link {

// A dynamic library the executable imports from. `exports` lists the names it defines; without
// a list, any name the objects leave undefined is assumed to come from it.
struct dylib {
  std::string install_name;
  std::optional<std::set<std::string, std::less<>>> exports;
  std::uint32_t current_version = 0x00010000;
  std::uint32_t compatibility_version = 0x00010000;
};

// /usr/lib/libSystem.B.dylib with the given export list (see `tbd_exports`).
dylib libsystem(std::optional<std::set<std::string, std::less<>>> exports = std::nullopt) {
  return {"/usr/lib/libSystem.B.dylib", std::move(exports), 0x05470000, 0x00010000};
}

struct options {
  std::string entry = "_main";
  std::string identifier = "a.out";  // the code signature's identifier, usually the output's file name
  std::vector<dylib> dylibs = {libsystem()};
  std::uint32_t min_os = 0x000c0000;  // macOS 12.0, as xxxx.yy.zz
  std::uint32_t sdk = 0x000f0000;     // 15.0
};

struct error {
  std::string message;
  bool operator==(const error&) const = default;
};

std::expected<std::vector<std::uint8_t>, std::vector<error>> link(std::span<const obj::object> objects, const options& opts = {});

// The names a text-based stub (.tbd, version 4) exports for arm64 macOS: the `symbols` and
// `weak-symbols` lists under `exports:` and `reexports:` whose targets include arm64 or arm64e
// macOS, across all documents of the file (libSystem's sub-libraries are documents of its .tbd).
std::set<std::string, std::less<>> tbd_exports(std::string_view text);

}  // namespace ycxx::link

namespace ycxx::link {

namespace {

namespace mo = ycxx::obj::macho;
using obj::reloc_kind;

constexpr std::uint64_t image_base = 0x100000000;  // __PAGEZERO's size: the low 4 GiB are unmapped
constexpr std::uint64_t page = 0x4000;              // arm64 macOS pages
constexpr std::uint64_t sign_page = 0x1000;         // code directory pages
constexpr std::uint32_t stub_size = 12;

constexpr bool fits32(std::int64_t v) { return v >= std::numeric_limits<std::int32_t>::min() && v <= std::numeric_limits<std::int32_t>::max(); }
constexpr std::uint64_t align_up(std::uint64_t v, std::uint64_t a) { return (v + a - 1) / a * a; }

enum class seg : std::uint8_t { text, data_const, data };
constexpr std::array<std::string_view, 3> seg_names = {"__TEXT", "__DATA_CONST", "__DATA"};

struct out_section {
  seg segment;
  std::string name;
  std::uint32_t flags = 0;
  std::uint8_t align_log2 = 0;
  std::uint64_t size = 0;
  std::uint64_t addr = 0;
  std::uint64_t fileoff = 0;
  std::vector<std::uint8_t> bytes;
  std::uint32_t reserved1 = 0, reserved2 = 0;
  bool zerofill() const { return obj::section_flags::is_zerofill(flags); }
};

struct place {
  std::uint32_t out;
  std::uint64_t offset;
  std::uint64_t size;
};

struct symref {
  std::uint32_t obj, sym;
  auto operator<=>(const symref&) const = default;
};

// What a relocation's symbol resolves to.
struct defined_at {
  std::uint64_t addr;
};
struct imported {
  std::uint32_t index;
};
using resolved = std::variant<defined_at, imported>;

struct fixup {
  std::uint64_t addr;
  bool bind;
  std::uint64_t target;  // rebase: offset from the image base; bind: import index
  std::uint8_t addend;   // bind only
};

struct segment_info {
  std::string name;
  std::uint64_t vmaddr = 0, vmsize = 0, fileoff = 0, filesize = 0;
  std::uint32_t prot = 0, flags = 0;
  std::vector<std::uint32_t> sections;
};

void put_uleb(std::vector<std::uint8_t>& out, std::uint64_t v) {
  do {
    std::uint8_t b = v & 0x7f;
    v >>= 7;
    if (v != 0) b |= 0x80;
    out.push_back(b);
  } while (v != 0);
}
std::size_t uleb_size(std::uint64_t v) {
  std::size_t n = 1;
  while (v >>= 7) ++n;
  return n;
}

// The exports trie (<mach-o/loader.h>, "export information"): a radix tree over symbol names.
// Each node is a terminal size (0, or the size of `flags, address` as ULEB128s), the terminal
// payload, a child count, and per child the edge's characters, a NUL and the child's offset.
struct export_entry {
  std::string name;
  std::uint64_t offset;  // from the image base
  std::uint32_t flags;
};
constexpr std::uint32_t export_weak_definition = 0x04;  // EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION

std::vector<std::uint8_t> exports_trie(const std::vector<export_entry>& sorted) {
  struct node {
    std::optional<std::uint64_t> address;
    std::uint32_t flags = 0;
    std::vector<std::pair<std::string, std::size_t>> children;  // edge, node index
    std::size_t offset = 0;
  };
  std::vector<node> nodes;
  // Build over the sorted range [lo, hi) whose names share their first `depth` characters.
  auto build = [&](this auto&& self, std::size_t lo, std::size_t hi, std::size_t depth) -> std::size_t {
    const std::size_t me = nodes.size();
    nodes.emplace_back();
    if (lo < hi && sorted[lo].name.size() == depth) {
      nodes[me].address = sorted[lo].offset;
      nodes[me].flags = sorted[lo++].flags;
    }
    while (lo < hi) {
      const char c = sorted[lo].name[depth];
      std::size_t end = lo;
      while (end < hi && sorted[end].name[depth] == c) ++end;
      // The group's common prefix: the first and last names bound it, as the range is sorted.
      const auto& a = sorted[lo].name;
      const auto& b = sorted[end - 1].name;
      std::size_t common = depth;
      while (common < a.size() && common < b.size() && a[common] == b[common]) ++common;
      const std::size_t child = self(lo, end, common);
      nodes[me].children.emplace_back(a.substr(depth, common - depth), child);
      lo = end;
    }
    return me;
  };
  build(std::size_t{0}, sorted.size(), std::size_t{0});

  // Node offsets depend on ULEB128 sizes of offsets: iterate to a fixed point.
  auto payload = [](const node& n) { return n.address ? uleb_size(n.flags) + uleb_size(*n.address) : 0; };
  for (bool changed = true; changed;) {
    changed = false;
    std::size_t off = 0;
    for (auto& n : nodes) {
      if (n.offset != off) {
        n.offset = off;
        changed = true;
      }
      const std::size_t p = payload(n);
      off += uleb_size(p) + p + 1;
      for (const auto& [edge, child] : n.children) off += edge.size() + 1 + uleb_size(nodes[child].offset);
    }
  }
  std::vector<std::uint8_t> out;
  for (const auto& n : nodes) {
    const std::size_t p = payload(n);
    put_uleb(out, p);
    if (n.address) {
      put_uleb(out, n.flags);
      put_uleb(out, *n.address);
    }
    out.push_back(static_cast<std::uint8_t>(n.children.size()));
    for (const auto& [edge, child] : n.children) {
      out.insert(out.end(), edge.begin(), edge.end());
      out.push_back(0);
      put_uleb(out, nodes[child].offset);
    }
  }
  return out;
}

// Instruction fields (Arm ARM, A64 encodings).
bool is_add_immediate(std::uint32_t insn) { return (insn & 0x7f800000) == 0x11000000; }
bool is_load_store_unsigned_offset(std::uint32_t insn) { return (insn & 0x3b000000) == 0x39000000; }
std::uint32_t load_store_scale(std::uint32_t insn) {
  const std::uint32_t size = insn >> 30;
  const bool simd = (insn >> 26) & 1, opc_high = (insn >> 23) & 1;
  return simd && opc_high && size == 0 ? 4 : size;  // a 128-bit Q register access
}
std::uint32_t with_adrp_pages(std::uint32_t insn, std::int64_t pages) {
  const auto p = static_cast<std::uint32_t>(pages);
  return (insn & 0x9f00001f) | (p & 3) << 29 | ((p >> 2) & 0x7ffff) << 5;
}

std::span<const std::uint8_t> frame_record(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 8) return {};
  const auto length = mo::load32(bytes, 0);
  if (length < 4 || length % 4 != 0 || length > bytes.size() - 4) return {};
  return bytes.first(length + 4);
}

constexpr std::uint8_t cleanup_cie_prefix[]{1, 'z', 'P', 'L', 'R', 0, 1, 0x78, 30, 7, 0x9b};

bool has_cleanup_cie(std::span<const std::uint8_t> bytes) {
  while (bytes.size() >= 14) {
    if (mo::load32(bytes, 4) == 0 &&
        std::ranges::equal(bytes.subspan(8, 6), std::span(cleanup_cie_prefix).first<6>()))
      return true;
    const auto record = frame_record(bytes);
    if (record.empty()) break;
    bytes = bytes.subspan(record.size());
  }
  return false;
}

std::optional<std::uint64_t> read_uleb(std::span<const std::uint8_t>& bytes) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64 && !bytes.empty(); shift += 7) {
    const auto byte = bytes.front();
    bytes = bytes.subspan(1);
    if (shift == 63 && (byte & 0x7f) > 1) return std::nullopt;
    value |= std::uint64_t(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) return value;
  }
  return std::nullopt;
}

// The supported LSDA omits LPStart and the type table. Its ULEB128 call-site rows refer
// to offsets within the FDE's own function and carry no catch actions (Itanium EH ABI).
std::optional<std::uint64_t> cleanup_table_size(const obj::object& o, const obj::symbol& symbol,
                                               std::uint64_t code_size) {
  if (symbol.bind != obj::binding::local || symbol.weak || !symbol.section || *symbol.section >= o.sections.size())
    return std::nullopt;
  const auto& section = o.sections[*symbol.section];
  if (section.segment != "__TEXT" || section.name != "__gcc_except_tab" || section.flags != obj::section_flags::regular ||
      section.zerofill_size != 0 || !section.relocs.empty() || symbol.offset >= section.bytes.size())
    return std::nullopt;
  const auto bytes = std::span(section.bytes).subspan(symbol.offset);
  constexpr std::uint8_t prefix[]{0xff, 0xff, 0x01};  // omitted LPStart and types, ULEB128 sites
  if (bytes.size() < sizeof(prefix) || !std::ranges::equal(bytes.first(sizeof(prefix)), prefix)) return std::nullopt;
  auto after_header = bytes.subspan(sizeof(prefix));
  const auto length = read_uleb(after_header);
  if (!length || *length > after_header.size()) return std::nullopt;
  auto rows = after_header.first(*length);
  std::uint64_t end = 0;
  bool has_cleanup = false;
  while (!rows.empty()) {
    const auto begin = read_uleb(rows);
    const auto size = read_uleb(rows);
    const auto landing = read_uleb(rows);
    const auto action = read_uleb(rows);
    if (!begin || !size || !landing || !action || *action != 0 || *size == 0 || *begin < end ||
        *begin > code_size || *size > code_size - *begin || *landing >= code_size ||
        (*begin | *size | *landing) % 4 != 0)
      return std::nullopt;
    end = *begin + *size;
    if (*landing != 0 && *landing >= *begin && *landing < end) return std::nullopt;
    has_cleanup |= *landing != 0;
  }
  return has_cleanup ? std::optional(bytes.size() - after_header.size() + *length) : std::nullopt;
}

// Preserve version 1 zR and cleanup-only zPLR DWARF records. Validate ownership of all
// relocatable fields before section placement. CFI instructions are preserved as supplied.
std::optional<std::uint32_t> unwind_frame_size(const obj::object& o, std::uint32_t si) {
  const auto& section = o.sections[si];
  const std::span<const std::uint8_t> bytes = section.bytes;
  constexpr std::uint32_t coalesced = 0x0b;  // S_COALESCED, used by Darwin assemblers
  const auto type = section.flags & obj::section_flags::type_mask;
  if ((type != obj::section_flags::regular && type != coalesced) || (section.flags & obj::section_flags::debug) != 0 ||
      section.align_log2 < 2 || section.align_log2 > 3 || bytes.size() % 4 != 0 ||
      !std::in_range<std::uint32_t>(bytes.size()))
    return std::nullopt;

  std::map<std::uint32_t, const obj::reloc*> relocations;
  for (const auto& r : section.relocs) {
    if (r.symbol >= o.symbols.size() || (r.minus && *r.minus >= o.symbols.size()) ||
        !relocations.emplace(r.offset, &r).second)
      return std::nullopt;
  }
  const auto take = [&](std::uint32_t offset) -> const obj::reloc* {
    const auto entry = relocations.find(offset);
    if (entry == relocations.end()) return nullptr;
    const auto* r = entry->second;
    relocations.erase(entry);
    return r;
  };
  const auto difference = [&](std::uint32_t field, std::uint32_t width) -> const obj::symbol* {
    const auto* r = take(field);
    if (!r || !r->minus || r->kind != (width == 4 ? reloc_kind::abs32 : reloc_kind::abs64)) return nullptr;
    const auto& base = o.symbols[*r->minus];
    if (base.bind != obj::binding::local || base.section != si || base.offset > field ||
        r->addend != static_cast<std::int64_t>(base.offset) - field)
      return nullptr;
    return &o.symbols[r->symbol];
  };

  constexpr std::uint8_t ordinary_cie_prefix[]{1, 'z', 'R', 0, 1, 0x78, 30, 1};
  struct cie_layout {
    std::uint32_t pc_width;
    bool cleanup;
  };
  std::map<std::uint32_t, cie_layout> cies;
  std::vector<std::tuple<std::uint32_t, std::uint64_t, std::uint64_t>> code_ranges, table_ranges;
  std::uint32_t at = 0;
  while (at < bytes.size()) {
    const auto length = mo::load32(bytes, at);
    if (length == 0) {
      if (std::ranges::any_of(bytes.subspan(at), [](auto byte) { return byte != 0; })) return std::nullopt;
      break;
    }
    const auto record = frame_record(bytes.subspan(at));
    if (record.empty()) return std::nullopt;
    const auto cie = mo::load32(record, 4);
    if (cie == 0) {
      if (record.size() >= 25 && std::ranges::equal(record.subspan(8, sizeof(cleanup_cie_prefix)), cleanup_cie_prefix)) {
        // L and R are direct, pointer-sized PC-relative encodings. P is an indirect
        // PC-relative signed word to the GOT entry of the Darwin C++ personality.
        if (record[23] != 0x10 || record[24] != 0x10) return std::nullopt;
        const auto* personality = take(at + 19);
        if (!personality || personality->kind != reloc_kind::got_rel32 || personality->addend != 0 || personality->minus)
          return std::nullopt;
        const auto& symbol = o.symbols[personality->symbol];
        if (symbol.bind != obj::binding::global || symbol.weak || symbol.name_view() != "___gxx_personality_v0")
          return std::nullopt;
        cies.emplace(at, cie_layout{8, true});
      } else {
        if (record.size() < 17 || !std::ranges::equal(record.subspan(8, sizeof(ordinary_cie_prefix)), ordinary_cie_prefix))
          return std::nullopt;
        // DW_EH_PE_pcrel paired with absptr, sdata4, or sdata8.
        const auto encoding = record[16];
        if (encoding != 0x10 && encoding != 0x1b && encoding != 0x1c) return std::nullopt;
        cies.emplace(at, cie_layout{encoding == 0x1b ? 4u : 8u, false});
      }
    } else {
      if (cie > at + 4) return std::nullopt;
      const auto common = cies.find(at + 4 - cie);
      if (common == cies.end()) return std::nullopt;
      const auto [width, cleanup] = common->second;
      const auto augmentation = 8 + 2 * width;
      const auto lsda_width = cleanup ? 8u : 0u;
      if (record.size() < augmentation + 1 + lsda_width || record[augmentation] != lsda_width)
        return std::nullopt;
      const auto* code = difference(at + 8, width);
      if (!code || !code->section || *code->section >= o.sections.size() || code->weak) return std::nullopt;
      const auto& text = o.sections[*code->section];
      const auto size = width == 4 ? mo::load32(record, 8 + width) : mo::load64(record, 8 + width);
      if (text.segment != "__TEXT" || text.name != "__text" || text.is_zerofill() ||
          (text.flags & obj::section_flags::debug) != 0 ||
          code->offset > text.size() || code->offset % 4 != 0 || size == 0 || size % 4 != 0 || size > text.size() - code->offset)
        return std::nullopt;
      code_ranges.emplace_back(*code->section, code->offset, code->offset + size);
      if (cleanup) {
        const auto* table = difference(at + augmentation + 1, lsda_width);
        const auto extent = table ? cleanup_table_size(o, *table, size) : std::nullopt;
        if (!extent) return std::nullopt;
        table_ranges.emplace_back(*table->section, table->offset, table->offset + *extent);
      }
    }
    at += length + 4;
  }
  const auto disjoint = [](auto& ranges) {
    std::ranges::sort(ranges);
    return std::ranges::adjacent_find(ranges, [](const auto& first, const auto& second) {
      return std::get<0>(first) == std::get<0>(second) && std::get<2>(first) > std::get<1>(second);
    }) == ranges.end();
  };
  if (code_ranges.empty() || !relocations.empty() || !disjoint(code_ranges) || !disjoint(table_ranges)) return std::nullopt;
  return at;
}

class linker {
public:
  linker(std::span<const obj::object> objects, const options& opts) : objs_(objects), opts_(opts) {}

  std::expected<std::vector<std::uint8_t>, std::vector<error>> run() {
    resolve();
    if (!errors_.empty()) return std::unexpected(std::move(errors_));
    place_sections();
    if (!errors_.empty()) return std::unexpected(std::move(errors_));
    layout();
    relocate();
    if (!errors_.empty()) return std::unexpected(std::move(errors_));
    return write();
  }

private:
  std::span<const obj::object> objs_;
  const options& opts_;
  std::vector<error> errors_;

  std::map<std::string, symref, std::less<>> globals_;
  // Names some object defines with default visibility: exported even when the definition kept is
  // a private-extern (hidden) weak one from another object, as the system linker does.
  std::set<std::string, std::less<>> exported_;
  std::map<std::string, std::uint8_t, std::less<>> common_align_;  // name -> largest alignment of its tentative definitions
  struct import_info {
    std::string name;
    std::uint32_t ordinal;
    bool weak;
  };
  std::vector<import_info> imports_;  // sorted by name
  std::map<std::string, std::uint32_t, std::less<>> import_index_;

  std::vector<out_section> sections_;
  std::vector<std::vector<std::optional<place>>> placed_;  // [object][section]
  std::optional<std::uint32_t> stubs_, got_;
  std::vector<std::uint32_t> stub_imports_;  // stub i calls import stub_imports_[i]
  std::map<std::uint32_t, std::uint32_t> stub_of_;
  struct got_target {
    bool is_import;
    std::uint32_t a, b;  // import index, or the defining symref
    auto operator<=>(const got_target&) const = default;
  };
  std::vector<got_target> got_entries_;
  std::map<got_target, std::uint32_t> got_of_;
  std::vector<segment_info> segments_;  // __PAGEZERO, then the non-empty of __TEXT, __DATA_CONST, __DATA, then __LINKEDIT
  std::uint32_t header_size_ = 0;
  std::vector<fixup> fixups_;

  void fail(std::string m) { errors_.push_back({std::move(m)}); }

  static bool dropped(const obj::section& s) {
    return (s.flags & obj::section_flags::debug) != 0 || s.segment == "__LD" || s.segment == "__LLVM" ||
           (s.segment == "__TEXT" && s.name == "__eh_frame");
  }

  // A tentative definition, as the Mach-O reader represents one: a weak definition alone at the
  // start of a __DATA,__common zero-fill section.
  static bool is_common(const obj::object& o, const obj::symbol& s) {
    const auto& sec = o.sections[*s.section];
    return s.offset == 0 && sec.segment == "__DATA" && sec.name == "__common" && sec.is_zerofill();
  }

  // Symbols the linker defines when no object does: both name the Mach-O header.
  static bool synthetic(std::string_view name) { return name == "__mh_execute_header" || name == "___dso_handle"; }
  static constexpr std::uint32_t synthetic_obj = ~0u;

  // 1. Symbol resolution.
  void resolve() {
    for (std::uint32_t i = 0; i < objs_.size(); ++i) {
      const auto& o = objs_[i];
      for (std::uint32_t j = 0; j < o.symbols.size(); ++j) {
        const auto& s = o.symbols[j];
        if (!s.defined()) continue;
        const auto name = s.name_view();
        if (*s.section >= o.sections.size()) {
          fail(std::format("{}: symbol {} names a bad section", o.name, name));
          continue;
        }
        if (s.bind == obj::binding::local || dropped(o.sections[*s.section])) continue;
        if (s.bind == obj::binding::global) exported_.emplace(name);
        if (is_common(o, s)) {
          auto& al = common_align_[name];
          al = std::max(al, o.sections[*s.section].align_log2);
        }
        auto [it, inserted] = globals_.try_emplace(name, symref{i, j});
        if (inserted) continue;
        const auto& prev_obj = objs_[it->second.obj];
        const auto& prev = prev_obj.symbols[it->second.sym];
        if (prev.weak && !s.weak) it->second = {i, j};
        else if (!prev.weak && !s.weak) fail(std::format("duplicate symbol {} in {} and {}", name, prev_obj.name, o.name));
        else if (prev.weak && s.weak && is_common(o, s) && is_common(prev_obj, prev) &&
                 o.sections[*s.section].size() > prev_obj.sections[*prev.section].size()) {
          it->second = {i, j};  // tentative definitions merge to the largest
        }
      }
    }
    if (!globals_.contains(opts_.entry)) fail(std::format("entry point {} is not defined", opts_.entry));

    // Undefined names, weak only if every reference is weak, with the first referencing object.
    std::map<std::string, std::pair<bool, std::uint32_t>, std::less<>> undefined;
    for (std::uint32_t i = 0; i < objs_.size(); ++i) {
      for (const auto& s : objs_[i].symbols) {
        const auto name = s.name_view();
        if (s.defined() || globals_.contains(name) || synthetic(name)) continue;
        auto [it, inserted] = undefined.try_emplace(name, s.weak, i);
        if (!inserted) it->second.first = it->second.first && s.weak;
      }
    }
    std::map<std::string, std::pair<std::uint32_t, bool>, std::less<>> wanted;  // name -> (ordinal, weak)
    for (const auto& [name, ref] : undefined) {
      const auto [weak, first] = ref;
      std::optional<std::uint32_t> ordinal;
      for (std::uint32_t d = 0; d < opts_.dylibs.size() && !ordinal; ++d) {
        const auto& ex = opts_.dylibs[d].exports;
        if (!ex || ex->contains(name)) ordinal = d + 1;
      }
      // A weak reference no dylib exports is still imported, weakly, from the first dylib: the
      // loader binds a missing weak import to 0.
      if (!ordinal && weak && !opts_.dylibs.empty()) ordinal = 1;
      if (ordinal) wanted.emplace(name, std::pair{*ordinal, weak});
      else fail(std::format("undefined symbol {}, referenced from {}", name, objs_[first].name));
    }
    for (const auto& [name, info] : wanted) {
      import_index_.emplace(name, static_cast<std::uint32_t>(imports_.size()));
      imports_.push_back({name, info.first, info.second});
    }
  }

  resolved resolve_symbol(std::uint32_t oi, std::uint32_t si) const {
    const auto& s = objs_[oi].symbols[si];
    symref def{oi, si};
    if (!s.defined() || s.bind != obj::binding::local) {
      const auto name = s.name_view();
      if (auto it = globals_.find(name); it != globals_.end()) def = it->second;
      else if (synthetic(name)) return defined_at{image_base};
      else return imported{import_index_.at(name)};
    }
    const auto& d = objs_[def.obj].symbols[def.sym];
    const auto& p = *placed_[def.obj][*d.section];
    return defined_at{sections_[p.out].addr + p.offset + d.offset};
  }

  got_target got_key(std::uint32_t oi, std::uint32_t si) const {
    const auto& s = objs_[oi].symbols[si];
    if (s.defined() && s.bind == obj::binding::local) return {false, oi, si};
    const auto name = s.name_view();
    if (auto it = globals_.find(name); it != globals_.end()) return {false, it->second.obj, it->second.sym};
    if (synthetic(name)) return {false, synthetic_obj, 0};
    return {true, import_index_.at(name), 0};
  }

  // 2. Output sections, contributions, stubs and GOT slots.
  std::optional<seg> segment_of(const obj::section& s) const {
    if (s.segment == "__TEXT") return seg::text;
    if (s.segment == "__DATA_CONST") return seg::data_const;
    if (s.segment == "__DATA") return s.name == "__const" || s.name == "__mod_init_func" ? seg::data_const : seg::data;
    return std::nullopt;
  }

  std::uint32_t output_section(seg sg, std::string_view name, std::uint32_t flags) {
    for (std::uint32_t k = 0; k < sections_.size(); ++k) {
      if (sections_[k].segment == sg && sections_[k].name == name) return k;
    }
    out_section s;
    s.segment = sg;
    s.name = name;
    s.flags = flags;
    sections_.push_back(std::move(s));
    return static_cast<std::uint32_t>(sections_.size() - 1);
  }

  void place_sections() {
    using namespace obj::section_flags;
    std::optional<std::uint32_t> unwind;
    placed_.resize(objs_.size());
    for (std::uint32_t i = 0; i < objs_.size(); ++i) {
      const auto& o = objs_[i];
      placed_[i].resize(o.sections.size());
      const bool one_unwind = std::ranges::count_if(o.sections, &obj::section::is_eh_frame) == 1;
      for (std::uint32_t j = 0; j < o.sections.size(); ++j) {
        const auto& s = o.sections[j];
        const auto frame_size = one_unwind && s.is_eh_frame() ? unwind_frame_size(o, j) : std::nullopt;
        if (dropped(s) && !frame_size) {
          if (s.is_eh_frame() && has_cleanup_cie(s.bytes))
            fail(std::format("{}: invalid cleanup-only DWARF frame, personality, or LSDA", o.name));
          continue;
        }
        const auto t = s.flags & type_mask;
        if (t == thread_local_regular || t == thread_local_zerofill || t == thread_local_variables) {
          fail(std::format("{}: thread-local section {},{} is not supported", o.name, s.segment, s.name));
          continue;
        }
        const auto sg = segment_of(s);
        if (!sg) {
          fail(std::format("{}: section {},{} is in an unsupported segment", o.name, s.segment, s.name));
          continue;
        }
        const std::uint32_t k = output_section(*sg, s.name, frame_size ? regular : s.flags);
        if (frame_size) unwind = k;
        auto& out = sections_[k];
        if (out.zerofill() != s.is_zerofill()) {
          fail(std::format("{}: section {},{} mixes zero-fill and contents", o.name, s.segment, s.name));
          continue;
        }
        // A merged tentative definition takes the largest alignment of all its copies.
        std::uint8_t align = s.align_log2;
        for (const auto& sym : o.symbols) {
          if (sym.section == j && sym.bind != obj::binding::local && is_common(o, sym)) {
            if (auto c = common_align_.find(sym.name_view()); c != common_align_.end()) align = std::max(align, c->second);
          }
        }
        const std::uint64_t a = frame_size ? 4 : std::uint64_t{1} << align;
        out.align_log2 = std::max(out.align_log2, align);
        out.size = align_up(out.size, a);
        const auto size = frame_size ? *frame_size : s.size();
        placed_[i][j] = place{k, out.size, size};
        out.size += size;
      }
    }
    // A single terminator follows the complete record stream.
    if (unwind) sections_[*unwind].size += 4;

    // Stubs for calls to imports, GOT slots for GOT references, in first-reference order.
    for (std::uint32_t i = 0; i < objs_.size(); ++i) {
      const auto& o = objs_[i];
      for (std::uint32_t j = 0; j < o.sections.size(); ++j) {
        if (!placed_[i][j]) continue;
        for (const auto& r : o.sections[j].relocs) {
          if (r.symbol >= o.symbols.size() || (r.minus && *r.minus >= o.symbols.size())) {
            fail(std::format("{}: relocation names a bad symbol", o.name));
            continue;
          }
          if (!check_placed(i, r.symbol) || (r.minus && !check_placed(i, *r.minus))) continue;
          if (r.kind == reloc_kind::branch26) {
            const auto key = got_key(i, r.symbol);
            if (key.is_import && !stub_of_.contains(key.a)) {
              stub_of_.emplace(key.a, static_cast<std::uint32_t>(stub_imports_.size()));
              stub_imports_.push_back(key.a);
              add_got_slot(key);  // the stub loads its target from a GOT slot
            }
          } else if (r.kind == reloc_kind::got_page21 || r.kind == reloc_kind::got_pageoff12 || r.kind == reloc_kind::got_rel32) {
            add_got_slot(got_key(i, r.symbol));
          }
        }
      }
    }
    if (!stub_imports_.empty()) {
      stubs_ = output_section(seg::text, "__stubs", symbol_stubs | pure_instructions | some_instructions);
      sections_[*stubs_].align_log2 = 2;
      sections_[*stubs_].size = stub_size * stub_imports_.size();
      sections_[*stubs_].reserved2 = stub_size;
    }
    if (!got_entries_.empty()) {
      got_ = output_section(seg::data_const, "__got", non_lazy_symbol_pointers);
      sections_[*got_].align_log2 = 3;
      sections_[*got_].size = 8 * got_entries_.size();
    }
  }

  void add_got_slot(const got_target& key) {
    if (got_of_.contains(key)) return;
    got_of_.emplace(key, static_cast<std::uint32_t>(got_entries_.size()));
    got_entries_.push_back(key);
  }

  // A relocation's symbol must not live in a dropped section.
  bool check_placed(std::uint32_t oi, std::uint32_t si) {
    const auto& o = objs_[oi];
    const auto& s = o.symbols[si];
    const auto name = s.name_view();
    if (!s.defined()) return true;
    if (*s.section >= o.sections.size()) {
      fail(std::format("{}: symbol {} names a bad section", o.name, name));
      return false;
    }
    if (s.bind == obj::binding::local ? !placed_[oi][*s.section] : !globals_.contains(name)) {
      const auto& sec = o.sections[*s.section];
      fail(std::format("{}: relocation refers to {} in the discarded section {},{}", o.name, name, sec.segment, sec.name));
      return false;
    }
    return true;
  }

  // 3. Addresses: sections sorted within each segment, segments page-aligned.
  std::uint32_t load_commands_size(std::size_t nsegments_with_sections) const {
    std::uint32_t n = static_cast<std::uint32_t>(mo::segment_command_64_size * (nsegments_with_sections + 2) +
                                                 mo::section_64_size * sections_.size());
    n += 16 + 16 + 24 + 80;               // chained fixups, exports trie, symtab, dysymtab
    n += align_up(12 + 14, 8);            // LC_LOAD_DYLINKER "/usr/lib/dyld"
    n += 24 + 24 + 24;                    // uuid, build version, main
    for (const auto& d : opts_.dylibs) n += static_cast<std::uint32_t>(align_up(24 + d.install_name.size() + 1, 8));
    n += 16;                              // code signature
    return n;
  }

  std::uint32_t command_count() const { return static_cast<std::uint32_t>(segments_.size()) + 9 + static_cast<std::uint32_t>(opts_.dylibs.size()); }

  void layout() {
    // Section order inside each segment: fixed leaders, then first appearance; zero-fill last.
    auto rank = [&](const out_section& s) {
      if (s.zerofill()) return 3;
      if (s.name == "__text" && s.segment == seg::text) return 0;
      if (s.name == "__stubs" && s.segment == seg::text) return 1;
      if (s.name == "__got" && s.segment == seg::data_const) return 0;
      return 2;
    };
    std::vector<std::uint32_t> order(sections_.size());
    std::iota(order.begin(), order.end(), 0u);
    std::ranges::stable_sort(order, [&](std::uint32_t a, std::uint32_t b) {
      return std::pair{sections_[a].segment, rank(sections_[a])} < std::pair{sections_[b].segment, rank(sections_[b])};
    });
    // Renumber so that section indexes are output order.
    std::vector<std::uint32_t> new_index(sections_.size());
    std::vector<out_section> sorted;
    for (std::uint32_t k = 0; k < order.size(); ++k) {
      new_index[order[k]] = k;
      sorted.push_back(std::move(sections_[order[k]]));
    }
    sections_ = std::move(sorted);
    for (auto& per_obj : placed_) {
      for (auto& p : per_obj) {
        if (p) p->out = new_index[p->out];
      }
    }
    if (stubs_) stubs_ = new_index[*stubs_];
    if (got_) got_ = new_index[*got_];

    std::size_t used = 0;
    for (std::size_t s = 0; s < 3; ++s) {
      if (std::ranges::any_of(sections_, [&](const out_section& o) { return std::to_underlying(o.segment) == s; }) || s == 0) ++used;
    }
    header_size_ = static_cast<std::uint32_t>(mo::mach_header_64_size) + load_commands_size(used);

    segments_.push_back({"__PAGEZERO", 0, image_base, 0, 0, 0, 0, {}});
    std::uint64_t vm = image_base, file = 0;
    for (std::size_t s = 0; s < 3; ++s) {
      const auto sg = static_cast<seg>(s);
      segment_info info{std::string(seg_names[s]), vm, 0, file, 0, 0, 0, {}};
      info.prot = sg == seg::text ? (mo::vm_prot_read | mo::vm_prot_execute) : (mo::vm_prot_read | mo::vm_prot_write);
      info.flags = sg == seg::data_const ? mo::sg_read_only : 0;
      std::uint64_t cursor = sg == seg::text ? header_size_ : 0;
      std::uint64_t file_end = cursor;
      for (std::uint32_t k = 0; k < sections_.size(); ++k) {
        auto& o = sections_[k];
        if (o.segment != sg) continue;
        cursor = align_up(cursor, std::uint64_t{1} << o.align_log2);
        o.addr = vm + cursor;
        o.fileoff = o.zerofill() ? 0 : file + cursor;
        cursor += o.size;
        if (!o.zerofill()) {
          file_end = cursor;
          o.bytes.assign(o.size, 0);
        }
        info.sections.push_back(k);
      }
      if (info.sections.empty() && sg != seg::text) continue;
      info.vmsize = align_up(cursor, page);
      info.filesize = align_up(file_end, page);
      vm += info.vmsize;
      file += info.filesize;
      segments_.push_back(std::move(info));
    }
    segments_.push_back({"__LINKEDIT", vm, 0, file, 0, mo::vm_prot_read, 0, {}});

    for (std::uint32_t i = 0; i < objs_.size(); ++i) {
      for (std::uint32_t j = 0; j < objs_[i].sections.size(); ++j) {
        const auto& p = placed_[i][j];
        if (!p || objs_[i].sections[j].is_zerofill()) continue;
        std::ranges::copy(std::span(objs_[i].sections[j].bytes).first(p->size),
                          sections_[p->out].bytes.begin() + static_cast<std::ptrdiff_t>(p->offset));
      }
    }
  }

  // 4. Relocations, stubs and GOT slots.
  void add_fixup(std::uint64_t addr, const resolved& target, std::int64_t addend, std::string_view where) {
    const auto it = std::ranges::find_if(segments_, [&](const segment_info& s) { return addr >= s.vmaddr && addr < s.vmaddr + s.vmsize; });
    if (it == segments_.end() || it->name == "__TEXT") {
      fail(std::format("{}: an absolute address in __TEXT would need a text relocation", where));
      return;
    }
    if (addr % 4 != 0) {
      fail(std::format("{}: a pointer the loader must fix up is not 4-byte aligned", where));
      return;
    }
    if (const auto* d = std::get_if<defined_at>(&target)) {
      const std::uint64_t off = d->addr + static_cast<std::uint64_t>(addend) - image_base;
      if (off >> 36) fail(std::format("{}: pointer target out of range", where));
      fixups_.push_back({addr, false, off, 0});
    } else {
      if (addend < 0 || addend > 255) fail(std::format("{}: pointer to an import with an addend outside 0..255", where));
      fixups_.push_back({addr, true, std::get<imported>(target).index, static_cast<std::uint8_t>(addend)});
    }
  }

  std::uint64_t got_address(std::uint32_t oi, std::uint32_t si) const { return sections_[*got_].addr + 8 * got_of_.at(got_key(oi, si)); }

  void relocate() {
    for (std::uint32_t i = 0; i < objs_.size(); ++i) {
      const auto& o = objs_[i];
      for (std::uint32_t j = 0; j < o.sections.size(); ++j) {
        const auto& p = placed_[i][j];
        if (!p) continue;
        auto& out = sections_[p->out];
        for (const auto& r : o.sections[j].relocs) {
          const std::uint64_t at = p->offset + r.offset;
          const std::uint64_t P = out.addr + at;
          const auto where = std::format("{}: {},{}+{:#x}", o.name, o.sections[j].segment, o.sections[j].name, r.offset);
          const std::uint32_t width = r.kind == reloc_kind::abs64 ? 8 : 4;
          if (r.offset > o.sections[j].size() || width > o.sections[j].size() - r.offset || out.zerofill()) {
            fail(std::format("{}: relocation outside its section", where));
            continue;
          }
          const auto target = resolve_symbol(i, r.symbol);
          const auto* def = std::get_if<defined_at>(&target);
          const std::uint64_t S = def ? def->addr : 0;
          const std::uint64_t SA = S + static_cast<std::uint64_t>(r.addend);
          auto insn = [&] { return mo::load32(out.bytes, at); };
          auto set_insn = [&](std::uint32_t v) { mo::store32(out.bytes, at, v); };
          auto needs_definition = [&] {
            if (def) return true;
            fail(std::format("{}: {} is imported and must be reached through the GOT", where, o.symbols[r.symbol].name_view()));
            return false;
          };
          switch (r.kind) {
            case reloc_kind::branch26: {
              const std::uint64_t dest = def ? SA : sections_[*stubs_].addr + stub_size * stub_of_.at(std::get<imported>(target).index) +
                                                        static_cast<std::uint64_t>(r.addend);
              const auto delta = static_cast<std::int64_t>(dest - P);
              if (delta % 4 != 0 || delta < -(std::int64_t{1} << 27) || delta >= (std::int64_t{1} << 27)) {
                fail(std::format("{}: branch target out of range", where));
                break;
              }
              set_insn((insn() & 0xfc000000) | (static_cast<std::uint32_t>(delta >> 2) & 0x03ffffff));
              break;
            }
            case reloc_kind::page21:
            case reloc_kind::got_page21: {
              std::uint64_t dest = 0;
              if (r.kind == reloc_kind::got_page21) dest = got_address(i, r.symbol);
              else if (needs_definition()) dest = SA;
              else break;
              const auto pages = static_cast<std::int64_t>((dest & ~std::uint64_t{0xfff}) - (P & ~std::uint64_t{0xfff})) >> 12;
              if (pages < -(std::int64_t{1} << 20) || pages >= (std::int64_t{1} << 20)) {
                fail(std::format("{}: ADRP target out of range", where));
                break;
              }
              if ((insn() & 0x9f000000) != 0x90000000) {
                fail(std::format("{}: PAGE21 relocation on an instruction that is not ADRP", where));
                break;
              }
              set_insn(with_adrp_pages(insn(), pages));
              break;
            }
            case reloc_kind::pageoff12:
            case reloc_kind::got_pageoff12: {
              std::uint64_t dest = 0;
              if (r.kind == reloc_kind::got_pageoff12) dest = got_address(i, r.symbol);
              else if (needs_definition()) dest = SA;
              else break;
              const std::uint32_t low = static_cast<std::uint32_t>(dest & 0xfff);
              std::uint32_t scale = 0;
              if (is_add_immediate(insn())) scale = 0;
              else if (is_load_store_unsigned_offset(insn())) scale = load_store_scale(insn());
              else {
                fail(std::format("{}: PAGEOFF12 relocation on an instruction that is neither ADD nor LDR/STR", where));
                break;
              }
              if (low % (1u << scale) != 0) {
                fail(std::format("{}: PAGEOFF12 target is not aligned to the access size", where));
                break;
              }
              set_insn((insn() & ~(0xfffu << 10)) | (low >> scale) << 10);
              break;
            }
            case reloc_kind::abs64:
            case reloc_kind::abs32: {
              if (r.minus) {
                const auto m = resolve_symbol(i, *r.minus);
                if (!def || !std::holds_alternative<defined_at>(m)) {
                  fail(std::format("{}: a difference between addresses needs both symbols defined", where));
                  break;
                }
                const std::uint64_t v = SA - std::get<defined_at>(m).addr;
                if (r.kind == reloc_kind::abs64) mo::store64(out.bytes, at, v);
                else if (!fits32(static_cast<std::int64_t>(v))) fail(std::format("{}: address difference does not fit in 32 bits", where));
                else mo::store32(out.bytes, at, static_cast<std::uint32_t>(v));
              } else if (r.kind == reloc_kind::abs32) {
                fail(std::format("{}: a 32-bit absolute address cannot be represented", where));
              } else {
                mo::store64(out.bytes, at, 0);
                add_fixup(P, target, r.addend, where);
              }
              break;
            }
            case reloc_kind::got_rel32: {
              const auto v = static_cast<std::int64_t>(got_address(i, r.symbol) + static_cast<std::uint64_t>(r.addend) - P);
              if (!fits32(v)) fail(std::format("{}: GOT slot out of 32-bit range", where));
              else mo::store32(out.bytes, at, static_cast<std::uint32_t>(v));
              break;
            }
            case reloc_kind::x86_64_pc32:
            case reloc_kind::x86_64_plt32:
            case reloc_kind::x86_64_rex_gotpcrelx:
              fail(std::format("{}: x86-64 relocation in an arm64 Mach-O object", where));
              break;
          }
        }
      }
    }

    if (got_) {
      auto& g = sections_[*got_];
      for (std::uint32_t k = 0; k < got_entries_.size(); ++k) {
        const auto& e = got_entries_[k];
        const resolved t = e.is_import ? resolved{imported{e.a}} : e.a == synthetic_obj ? resolved{defined_at{image_base}} : resolve_symbol(e.a, e.b);
        add_fixup(g.addr + 8 * k, t, 0, "__DATA_CONST,__got");
      }
    }
    std::ranges::sort(fixups_, {}, &fixup::addr);
  }

  // 5. __LINKEDIT and the file.
  std::vector<std::uint8_t> chained_fixups() const {
    mo::byte_writer w;
    // dyld_chained_fixups_header, filled in at the end.
    w.zeros(28);
    w.align(8);
    const std::uint32_t starts_offset = static_cast<std::uint32_t>(w.size());
    const std::uint32_t seg_count = static_cast<std::uint32_t>(segments_.size());
    w.u32(seg_count);
    const std::size_t table = w.size();
    w.zeros(4 * seg_count);
    for (std::uint32_t s = 0; s < seg_count; ++s) {
      const auto& sg = segments_[s];
      auto first = std::ranges::lower_bound(fixups_, sg.vmaddr, {}, &fixup::addr);
      auto last = std::ranges::lower_bound(fixups_, sg.vmaddr + sg.vmsize, {}, &fixup::addr);
      if (first == last) continue;
      w.align(8);
      mo::store32(w.bytes, table + 4 * s, static_cast<std::uint32_t>(w.size() - starts_offset));
      const auto page_count = static_cast<std::uint16_t>(sg.vmsize / page);
      w.u32(22 + 2u * page_count);  // size of dyld_chained_starts_in_segment with its page_start array
      w.u16(static_cast<std::uint16_t>(page));
      w.u16(6);  // DYLD_CHAINED_PTR_64_OFFSET
      w.u64(sg.vmaddr - image_base);
      w.u32(0);  // max_valid_pointer
      w.u16(page_count);
      std::vector<std::uint16_t> starts(page_count, 0xffff);  // DYLD_CHAINED_PTR_START_NONE
      for (auto it = first; it != last; ++it) {
        const std::uint64_t p = (it->addr - sg.vmaddr) / page;
        if (starts[p] == 0xffff) starts[p] = static_cast<std::uint16_t>((it->addr - sg.vmaddr) % page);
      }
      for (auto v : starts) w.u16(v);
    }
    w.align(4);
    const std::uint32_t imports_offset = static_cast<std::uint32_t>(w.size());
    mo::byte_writer names;
    names.u8(0);
    for (const auto& im : imports_) {
      w.u32(im.ordinal | std::uint32_t{im.weak} << 8 | static_cast<std::uint32_t>(names.size()) << 9);
      names.raw(im.name);
      names.u8(0);
    }
    const std::uint32_t symbols_offset = static_cast<std::uint32_t>(w.size());
    w.raw(names.bytes);
    w.align(8);
    mo::store32(w.bytes, 0, 0);  // fixups_version
    mo::store32(w.bytes, 4, starts_offset);
    mo::store32(w.bytes, 8, imports_offset);
    mo::store32(w.bytes, 12, symbols_offset);
    mo::store32(w.bytes, 16, static_cast<std::uint32_t>(imports_.size()));
    mo::store32(w.bytes, 20, 1);  // DYLD_CHAINED_IMPORT
    mo::store32(w.bytes, 24, 0);  // uncompressed symbol names
    return std::move(w.bytes);
  }

  // Each fixup's word: dyld_chained_ptr_64_rebase or _bind, with `next` in 4-byte strides to the
  // next fixup on the same page.
  void write_chains(std::vector<std::uint8_t>& file) const {
    for (std::size_t k = 0; k < fixups_.size(); ++k) {
      const auto& f = fixups_[k];
      const auto sg = std::ranges::find_if(segments_, [&](const segment_info& s) { return f.addr >= s.vmaddr && f.addr < s.vmaddr + s.vmsize; });
      std::uint64_t next = 0;
      if (k + 1 < fixups_.size() && (fixups_[k + 1].addr - sg->vmaddr) / page == (f.addr - sg->vmaddr) / page) {
        next = (fixups_[k + 1].addr - f.addr) / 4;
      }
      std::uint64_t word = f.bind ? (f.target & 0xffffff) | std::uint64_t{f.addend} << 24 | std::uint64_t{1} << 63
                                  : (f.target & 0xfffffffff);
      word |= next << 51;
      mo::store64(file, sg->fileoff + (f.addr - sg->vmaddr), word);
    }
  }

  std::vector<std::uint8_t> write() {
    using mo::byte_writer;
    // The symbol table: locals (named, non-temporary), external definitions, imports.
    struct sym_entry {
      std::string name;
      std::uint8_t type, sect;
      std::uint16_t desc;
      std::uint64_t value;
    };
    std::vector<sym_entry> locals, extdefs, undefs;
    for (std::uint32_t i = 0; i < objs_.size(); ++i) {
      for (std::uint32_t j = 0; j < objs_[i].symbols.size(); ++j) {
        const auto& s = objs_[i].symbols[j];
        const auto name = s.name_view();
        if (!s.defined() || !placed_[i][*s.section] || name.empty()) continue;
        if (s.bind == obj::binding::local && (name[0] == 'l' || name[0] == 'L')) continue;  // temporary labels
        if (s.bind != obj::binding::local) {
          const auto it = globals_.find(name);
          if (it == globals_.end() || it->second != symref{i, j}) continue;
        }
        const std::uint64_t a = std::get<defined_at>(resolve_symbol(i, j)).addr;
        const auto sect = static_cast<std::uint8_t>(placed_[i][*s.section]->out + 1);
        // A merged tentative definition is an ordinary definition in the executable.
        const bool weak = s.weak && !is_common(objs_[i], s);
        const sym_entry e{std::string(name), mo::n_sect, sect, static_cast<std::uint16_t>(weak ? mo::n_weak_def : 0), a};
        if (s.bind != obj::binding::local && exported_.contains(name))
          extdefs.push_back({e.name, static_cast<std::uint8_t>(mo::n_sect | mo::n_ext), e.sect, e.desc, e.value});
        else locals.push_back(e);
      }
    }
    if (!globals_.contains("__mh_execute_header")) {
      extdefs.push_back({"__mh_execute_header", static_cast<std::uint8_t>(mo::n_sect | mo::n_ext), 1, mo::referenced_dynamically, image_base});
    }
    if (!globals_.contains("___dso_handle")) locals.push_back({"___dso_handle", mo::n_sect, 1, 0, image_base});
    std::ranges::sort(extdefs, {}, &sym_entry::name);
    for (const auto& im : imports_) {
      undefs.push_back({im.name, static_cast<std::uint8_t>(mo::n_undf | mo::n_ext), 0,
                        static_cast<std::uint16_t>(im.ordinal << 8 | (im.weak ? mo::n_weak_ref : 0)), 0});
    }
    const std::uint32_t nlocal = static_cast<std::uint32_t>(locals.size()), nextdef = static_cast<std::uint32_t>(extdefs.size());
    std::vector<sym_entry> all = std::move(locals);
    all.insert(all.end(), extdefs.begin(), extdefs.end());
    all.insert(all.end(), undefs.begin(), undefs.end());

    // Indirect symbols: one per stub, then one per GOT slot.
    std::vector<std::uint32_t> indirect;
    if (stubs_) sections_[*stubs_].reserved1 = 0;
    for (auto im : stub_imports_) indirect.push_back(nlocal + nextdef + im);
    if (got_) sections_[*got_].reserved1 = static_cast<std::uint32_t>(indirect.size());
    for (const auto& e : got_entries_) indirect.push_back(e.is_import ? nlocal + nextdef + e.a : mo::indirect_symbol_local);

    // __LINKEDIT contents, in the order Apple's tools lay them out.
    byte_writer le;
    const std::uint64_t le_off = segments_.back().fileoff;
    const auto fixups_blob = chained_fixups();
    const std::uint64_t fixups_off = le_off + le.size();
    le.raw(fixups_blob);
    std::vector<export_entry> exported;
    bool weak_defines = false;
    for (const auto& e : extdefs) {
      const bool weak = (e.desc & mo::n_weak_def) != 0;
      weak_defines = weak_defines || weak;
      exported.push_back({e.name, e.value - image_base, weak ? export_weak_definition : 0u});
    }
    const auto trie = exports_trie(exported);
    const std::uint64_t trie_off = le_off + le.size();
    le.raw(trie);
    le.align(8);
    byte_writer strtab;
    strtab.u8(' ');
    strtab.u8(0);
    const std::uint64_t sym_off = le_off + le.size();
    for (const auto& e : all) {
      le.u32(static_cast<std::uint32_t>(strtab.size()));
      strtab.raw(e.name);
      strtab.u8(0);
      le.u8(e.type);
      le.u8(e.sect);
      le.u16(e.desc);
      le.u64(e.value);
    }
    const std::uint64_t indirect_off = le_off + le.size();
    for (auto v : indirect) le.u32(v);
    strtab.align(8);
    const std::uint64_t str_off = le_off + le.size();
    le.raw(strtab.bytes);
    le.align(16);
    const std::uint64_t sig_off = le_off + le.size();
    const std::uint64_t nslots = align_up(sig_off, sign_page) / sign_page;
    const std::uint64_t cd_size = 88 + opts_.identifier.size() + 1 + 32 * nslots;
    const std::uint64_t sig_size = 12 + 8 + cd_size;
    auto& linkedit = segments_.back();
    linkedit.filesize = le.size() + sig_size;
    linkedit.vmsize = align_up(linkedit.filesize, page);

    // The header and load commands.
    byte_writer h;
    h.u32(mo::mh_magic_64);
    h.u32(mo::cpu_type_arm64);
    h.u32(mo::cpu_subtype_arm64_all);
    h.u32(mo::mh_execute);
    h.u32(command_count());
    h.u32(header_size_ - static_cast<std::uint32_t>(mo::mach_header_64_size));
    h.u32(mo::mh_noundefs | mo::mh_dyldlink | mo::mh_twolevel | mo::mh_pie | (weak_defines ? mo::mh_weak_defines : 0));
    h.u32(0);
    for (const auto& sg : segments_) {
      h.u32(mo::lc_segment_64);
      h.u32(static_cast<std::uint32_t>(mo::segment_command_64_size + mo::section_64_size * sg.sections.size()));
      h.name16(sg.name);
      h.u64(sg.vmaddr);
      h.u64(sg.vmsize);
      h.u64(sg.fileoff);
      h.u64(sg.filesize);
      h.u32(sg.prot);
      h.u32(sg.prot);
      h.u32(static_cast<std::uint32_t>(sg.sections.size()));
      h.u32(sg.flags);
      for (auto k : sg.sections) {
        const auto& s = sections_[k];
        h.name16(s.name);
        h.name16(sg.name);
        h.u64(s.addr);
        h.u64(s.size);
        h.u32(static_cast<std::uint32_t>(s.fileoff));
        h.u32(s.align_log2);
        h.u32(0);
        h.u32(0);
        h.u32(s.flags);
        h.u32(s.reserved1);
        h.u32(s.reserved2);
        h.u32(0);
      }
    }
    auto linkedit_data = [&](std::uint32_t cmd, std::uint64_t off, std::uint64_t size) {
      h.u32(cmd);
      h.u32(16);
      h.u32(static_cast<std::uint32_t>(off));
      h.u32(static_cast<std::uint32_t>(size));
    };
    linkedit_data(mo::lc_dyld_chained_fixups, fixups_off, fixups_blob.size());
    linkedit_data(mo::lc_dyld_exports_trie, trie_off, trie.size());
    h.u32(mo::lc_symtab);
    h.u32(24);
    h.u32(static_cast<std::uint32_t>(sym_off));
    h.u32(static_cast<std::uint32_t>(all.size()));
    h.u32(static_cast<std::uint32_t>(str_off));
    h.u32(static_cast<std::uint32_t>(strtab.size()));
    h.u32(mo::lc_dysymtab);
    h.u32(80);
    h.u32(0);
    h.u32(nlocal);
    h.u32(nlocal);
    h.u32(nextdef);
    h.u32(nlocal + nextdef);
    h.u32(static_cast<std::uint32_t>(undefs.size()));
    h.u32(0);  // toc
    h.u32(0);
    h.u32(0);  // module table
    h.u32(0);
    h.u32(0);  // external references
    h.u32(0);
    h.u32(static_cast<std::uint32_t>(indirect.empty() ? 0 : indirect_off));
    h.u32(static_cast<std::uint32_t>(indirect.size()));
    h.u32(0);  // external relocations
    h.u32(0);
    h.u32(0);  // local relocations
    h.u32(0);
    h.u32(mo::lc_load_dylinker);
    h.u32(32);
    h.u32(12);
    h.raw("/usr/lib/dyld");
    h.zeros(32 - 12 - 13);
    const std::size_t uuid_at = h.size() + 8;
    h.u32(mo::lc_uuid);
    h.u32(24);
    h.zeros(16);
    h.u32(mo::lc_build_version);
    h.u32(24);
    h.u32(mo::platform_macos);
    h.u32(opts_.min_os);
    h.u32(opts_.sdk);
    h.u32(0);
    h.u32(mo::lc_main);
    h.u32(24);
    h.u64(std::get<defined_at>(resolve_symbol(globals_.at(opts_.entry).obj, globals_.at(opts_.entry).sym)).addr - image_base);
    h.u64(0);
    for (const auto& d : opts_.dylibs) {
      const auto size = static_cast<std::uint32_t>(align_up(24 + d.install_name.size() + 1, 8));
      h.u32(mo::lc_load_dylib);
      h.u32(size);
      h.u32(24);
      h.u32(2);  // timestamp
      h.u32(d.current_version);
      h.u32(d.compatibility_version);
      h.raw(d.install_name);
      h.zeros(size - 24 - d.install_name.size());
    }
    h.u32(mo::lc_code_signature);
    h.u32(16);
    h.u32(static_cast<std::uint32_t>(sig_off));
    h.u32(static_cast<std::uint32_t>(sig_size));
    contract_assert(h.size() == header_size_);

    // Assemble the file.
    std::vector<std::uint8_t> file(sig_off + sig_size, 0);
    std::ranges::copy(h.bytes, file.begin());
    for (const auto& s : sections_) {
      if (!s.zerofill()) std::ranges::copy(s.bytes, file.begin() + static_cast<std::ptrdiff_t>(s.fileoff));
    }
    write_stubs(file);
    write_chains(file);
    std::ranges::copy(le.bytes, file.begin() + static_cast<std::ptrdiff_t>(le_off));

    // LC_UUID: a fingerprint of everything before the signature, with the UUID itself zero.
    const auto fp = fingerprint128(std::span<const std::uint8_t>(file).first(sig_off));
    for (int b = 0; b < 8; ++b) {
      file[uuid_at + b] = static_cast<std::uint8_t>(fp.lo >> (8 * b));
      file[uuid_at + 8 + b] = static_cast<std::uint8_t>(fp.hi >> (8 * b));
    }

    sign(file, sig_off, nslots);
    return file;
  }

  void write_stubs(std::vector<std::uint8_t>& file) const {
    if (!stubs_) return;
    const auto& st = sections_[*stubs_];
    for (std::uint32_t k = 0; k < stub_imports_.size(); ++k) {
      const std::uint64_t P = st.addr + stub_size * k;
      const std::uint64_t slot = sections_[*got_].addr + 8 * got_of_.at(got_target{true, stub_imports_[k], 0});
      const auto pages = static_cast<std::int64_t>((slot & ~std::uint64_t{0xfff}) - (P & ~std::uint64_t{0xfff})) >> 12;
      const std::uint64_t at = st.fileoff + stub_size * k;
      mo::store32(file, at, with_adrp_pages(0x90000010, pages));                                    // adrp x16, slot@PAGE
      mo::store32(file, at + 4, 0xf9400210 | static_cast<std::uint32_t>((slot & 0xfff) >> 3) << 10);  // ldr x16, [x16, slot@PAGEOFF]
      mo::store32(file, at + 8, 0xd61f0200);                                                         // br x16
    }
  }

  // The ad-hoc signature: a SuperBlob holding one CodeDirectory (version 0x20400, with the
  // executable segment fields), big-endian, hashing every 4 KiB page before the signature.
  void sign(std::vector<std::uint8_t>& file, std::uint64_t sig_off, std::uint64_t nslots) const {
    std::vector<std::uint8_t> s;
    auto be32 = [&](std::uint32_t v) {
      for (int i = 3; i >= 0; --i) s.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto be64 = [&](std::uint64_t v) {
      for (int i = 7; i >= 0; --i) s.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    const std::uint32_t ident_offset = 88;
    const std::uint32_t hash_offset = ident_offset + static_cast<std::uint32_t>(opts_.identifier.size()) + 1;
    const std::uint32_t cd_size = hash_offset + 32 * static_cast<std::uint32_t>(nslots);
    be32(0xfade0cc0);  // CSMAGIC_EMBEDDED_SIGNATURE
    be32(12 + 8 + cd_size);
    be32(1);           // count
    be32(0);           // CSSLOT_CODEDIRECTORY
    be32(20);          // offset
    be32(0xfade0c02);  // CSMAGIC_CODEDIRECTORY
    be32(cd_size);
    be32(0x20400);                 // version
    be32(0x2 | 0x20000);           // CS_ADHOC | CS_LINKER_SIGNED
    be32(hash_offset);
    be32(ident_offset);
    be32(0);                       // nSpecialSlots
    be32(static_cast<std::uint32_t>(nslots));
    be32(static_cast<std::uint32_t>(sig_off));  // codeLimit
    s.push_back(32);               // hashSize
    s.push_back(2);                // CS_HASHTYPE_SHA256
    s.push_back(0);                // platform
    s.push_back(12);               // log2(page size)
    be32(0);                       // spare2
    be32(0);                       // scatterOffset
    be32(0);                       // teamOffset
    be32(0);                       // spare3
    be64(0);                       // codeLimit64
    const auto& text = segments_[1];
    be64(text.fileoff);            // execSegBase
    be64(text.filesize);           // execSegLimit
    be64(1);                       // CS_EXECSEG_MAIN_BINARY
    s.insert(s.end(), opts_.identifier.begin(), opts_.identifier.end());
    s.push_back(0);
    for (std::uint64_t p = 0; p < nslots; ++p) {
      const std::uint64_t lo = p * sign_page, hi = std::min(sig_off, lo + sign_page);
      const auto d = sha256_of(std::span<const std::uint8_t>(file).subspan(lo, hi - lo));
      s.insert(s.end(), d.begin(), d.end());
    }
    contract_assert(s.size() == file.size() - sig_off);
    std::ranges::copy(s, file.begin() + static_cast<std::ptrdiff_t>(sig_off));
  }
};

}  // namespace

std::expected<std::vector<std::uint8_t>, std::vector<error>> link(std::span<const obj::object> objects, const options& opts) {
  for (const obj::object& o : objects) {
    if (o.arch != obj::target::aarch64_macos)
      return std::unexpected(std::vector<error>{{std::format("{}: expected an arm64 Mach-O object", o.name)}});
  }
  return linker(objects, opts).run();
}

std::set<std::string, std::less<>> tbd_exports(std::string_view text) {
  std::set<std::string, std::less<>> out;
  bool in_exports = false;   // inside a top-level `exports:` or `reexports:` block
  bool arm64_target = false;  // the current list item's targets include arm64 macOS
  std::string_view list_key;  // the key whose [ ... ] list is open, if any
  std::size_t pos = 0;
  auto take_names = [&](std::string_view s) {
    // Names are separated by commas and may be quoted.
    std::size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '[' || s[i] == '\t')) ++i;
      if (i >= s.size() || s[i] == ']') break;
      std::string name;
      if (s[i] == '\'' || s[i] == '"') {
        const char q = s[i++];
        while (i < s.size() && s[i] != q) name.push_back(s[i++]);
        ++i;
      } else {
        while (i < s.size() && s[i] != ',' && s[i] != ' ' && s[i] != ']') name.push_back(s[i++]);
      }
      if (arm64_target && (list_key == "symbols" || list_key == "weak-symbols")) out.insert(std::move(name));
    }
  };
  while (pos < text.size()) {
    std::size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) eol = text.size();
    std::string_view line = text.substr(pos, eol - pos);
    pos = eol + 1;
    if (!list_key.empty()) {
      // A continuation line of an open list.
      take_names(line);
      if (line.find(']') != std::string_view::npos) list_key = {};
      continue;
    }
    if (line.starts_with("---")) {
      in_exports = false;
      continue;
    }
    if (!line.empty() && line[0] != ' ' && line[0] != '-') {
      in_exports = line.starts_with("exports:") || line.starts_with("reexports:");
      continue;
    }
    if (!in_exports) continue;
    std::size_t k = line.find_first_not_of(" -");
    if (k == std::string_view::npos) continue;
    const std::size_t colon = line.find(':', k);
    if (colon == std::string_view::npos) continue;
    const std::string_view key = line.substr(k, colon - k);
    const std::string_view rest = line.substr(colon + 1);
    if (key == "targets") {
      arm64_target = rest.find("arm64-macos") != std::string_view::npos || rest.find("arm64e-macos") != std::string_view::npos;
      if (rest.find('[') != std::string_view::npos && rest.find(']') == std::string_view::npos) {
        // The targets list wraps: read its continuation lines.
        while (pos < text.size()) {
          std::size_t e = text.find('\n', pos);
          if (e == std::string_view::npos) e = text.size();
          const std::string_view cont = text.substr(pos, e - pos);
          pos = e + 1;
          if (cont.find("arm64-macos") != std::string_view::npos || cont.find("arm64e-macos") != std::string_view::npos) arm64_target = true;
          if (cont.find(']') != std::string_view::npos) break;
        }
      }
      continue;
    }
    if (rest.find('[') == std::string_view::npos) continue;
    list_key = key;
    take_names(rest.substr(rest.find('[')));
    if (rest.find(']') != std::string_view::npos) list_key = {};
  }
  return out;
}

}  // namespace ycxx::link
