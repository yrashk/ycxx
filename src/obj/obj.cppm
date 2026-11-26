// The in-memory object file (#42): what code generation emits into and what the linker reads.
//
// An `object` is a list of sections and a list of symbols. A section holds bytes (or only a size,
// for zero-fill) and the relocations that patch them; a relocation names a symbol by its index in
// the object's symbol list. Sections are named the Mach-O way, (segment, section) plus Mach-O
// flags, because AArch64 Mach-O is the first target (DECISIONS §1.3); the ELF targets will map
// `standard_section` kinds to their own names.
//
// Determinism (query-engine design §A5.3): every list is kept in the order it was built in.
// Code generation appends functions and variables in the order §A5.3 prescribes, and the writer
// and the linker never reorder by anything but these indexes and names.
//
// Emitting a function:
//
//   obj::object o;
//   auto text = o.add_section(obj::standard_section::text);
//   auto puts = o.reference("_puts");
//   o.define("_main", obj::binding::global, text);          // at the section's current end
//   o.sections[text].emit32(0x94000000);                     // bl _puts
//   o.sections[text].relocate(obj::reloc_kind::branch26, puts);
//
// `relocate` records a relocation at the last emitted word (or at an explicit offset). The bytes
// under a relocation are ignored by the linker; the Mach-O writer stores implicit addends there.
export module ycxx.obj;

import std;

export namespace ycxx::obj {

// Mach-O section types and attributes (<mach-o/loader.h>), the ones ycxx emits or must recognize.
namespace section_flags {
inline constexpr std::uint32_t type_mask = 0x000000ff;
inline constexpr std::uint32_t regular = 0x0;
inline constexpr std::uint32_t zerofill = 0x1;
inline constexpr std::uint32_t cstring_literals = 0x2;
inline constexpr std::uint32_t non_lazy_symbol_pointers = 0x6;
inline constexpr std::uint32_t symbol_stubs = 0x8;
inline constexpr std::uint32_t mod_init_func_pointers = 0x9;
inline constexpr std::uint32_t coalesced = 0xb;
inline constexpr std::uint32_t gb_zerofill = 0xc;
inline constexpr std::uint32_t thread_local_regular = 0x11;
inline constexpr std::uint32_t thread_local_zerofill = 0x12;
inline constexpr std::uint32_t thread_local_variables = 0x13;
inline constexpr std::uint32_t pure_instructions = 0x80000000;
inline constexpr std::uint32_t no_toc = 0x40000000;
inline constexpr std::uint32_t strip_static_syms = 0x20000000;
inline constexpr std::uint32_t live_support = 0x08000000;
inline constexpr std::uint32_t debug = 0x02000000;
inline constexpr std::uint32_t some_instructions = 0x00000400;

constexpr bool is_zerofill(std::uint32_t flags) noexcept {
  const auto t = flags & type_mask;
  return t == zerofill || t == gb_zerofill || t == thread_local_zerofill;
}
}  // namespace section_flags

// The sections code generation uses. `other` sections come from the reader.
enum class standard_section : std::uint8_t {
  text,        // __TEXT,__text: code
  cstring,     // __TEXT,__cstring: NUL-terminated string literals
  rodata,      // __TEXT,__const: read-only data without relocations
  data_const,  // __DATA,__const: read-only after load, may hold pointers (the linker puts it in __DATA_CONST)
  data,        // __DATA,__data: writable data
  bss,         // __DATA,__bss: zero-initialized data
  mod_init_func,  // __DATA,__mod_init_func: pointers to the functions dyld runs before main
  init_array,     // ELF .init_array: pointers to the functions startup runs before main
  eh_frame,       // __TEXT,__eh_frame / ELF .eh_frame: call frame information
  gcc_except_table,  // ELF .gcc_except_table: language-specific exception data
};

// How a relocation patches its site. S is the target symbol's address, A the addend, P the
// site's address, M the address of `minus`, GOT(S) the address of S's GOT slot. Instruction
// fields are defined by the Arm ARM or the System V AMD64 psABI.
enum class reloc_kind : std::uint8_t {
  branch26,       // B, BL: imm26 = (S + A - P) >> 2; an imported S goes through a stub
  page21,         // ADRP: immhi:immlo = page(S + A) - page(P), in 4 KiB pages
  pageoff12,      // ADD (immediate), LDR/STR (unsigned offset): imm12 = (S + A) & 0xfff, scaled by the access size
  got_page21,     // ADRP: page(GOT(S)) - page(P)
  got_pageoff12,  // LDR Xt: imm12 = (GOT(S) & 0xfff) >> 3
  abs64,          // 64-bit word: S + A, or S + A - M when `minus` is set
  abs32,          // 32-bit word: S + A - M; only differences fit
  got_rel32,      // 32-bit word: GOT(S) + A - P
  x86_64_pc32,    // signed 32-bit displacement: S + A - P
  x86_64_plt32,   // signed 32-bit displacement: PLT(S) + A - P
  x86_64_rex_gotpcrelx,  // signed RIP-relative displacement to a relaxable GOT load
};

struct reloc {
  std::uint32_t offset = 0;  // in the section
  reloc_kind kind = reloc_kind::abs64;
  std::uint32_t symbol = 0;  // index into `object::symbols`
  std::int64_t addend = 0;
  std::optional<std::uint32_t> minus;  // abs64/abs32 only: subtract this symbol's address
  constexpr bool operator==(const reloc&) const = default;
};

struct section {
  std::string segment;  // "__TEXT"
  std::string name;     // "__text"
  std::uint32_t flags = section_flags::regular;
  std::uint8_t align_log2 = 0;
  std::vector<std::uint8_t> bytes;  // empty for zero-fill sections
  std::uint64_t zerofill_size = 0;  // size of a zero-fill section
  std::vector<reloc> relocs;

  static section standard(standard_section kind);

  bool is_init_array() const noexcept { return segment == "__DATA" && name == "__init_array"; }
  bool is_eh_frame() const noexcept { return segment == "__TEXT" && name == "__eh_frame"; }
  bool is_gcc_except_table() const noexcept { return segment == "__TEXT" && name == "__gcc_except"; }
  bool is_zerofill() const noexcept { return section_flags::is_zerofill(flags); }
  std::uint64_t size() const noexcept { return is_zerofill() ? zerofill_size : bytes.size(); }

  // Appending. Words are little-endian; `align` pads with zeros (zero-fill sections grow).
  void align(std::uint8_t log2) {
    align_log2 = std::max(align_log2, log2);
    const std::uint64_t a = std::uint64_t{1} << log2;
    if (is_zerofill()) zerofill_size = (zerofill_size + a - 1) & ~(a - 1);
    else bytes.resize((bytes.size() + a - 1) & ~(a - 1), 0);
  }
  void emit(std::span<const std::uint8_t> b) { bytes.insert(bytes.end(), b.begin(), b.end()); }
  void emit(std::string_view s) { bytes.insert(bytes.end(), s.begin(), s.end()); }
  void emit32(std::uint32_t w) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
  }
  void emit64(std::uint64_t w) {
    for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
  }
  void reserve_zerofill(std::uint64_t n) { zerofill_size += n; }

  // A relocation at `offset`, or at the last 4 bytes (abs64: 8 bytes) emitted.
  void relocate(reloc_kind kind, std::uint32_t symbol, std::int64_t addend = 0,
                std::optional<std::uint32_t> at = std::nullopt, std::optional<std::uint32_t> minus = std::nullopt) {
    const std::uint32_t width = kind == reloc_kind::abs64 ? 8 : 4;
    relocs.push_back({at.value_or(static_cast<std::uint32_t>(bytes.size()) - width), kind, symbol, addend, minus});
  }

  bool operator==(const section&) const = default;
};

enum class binding : std::uint8_t {
  local,   // visible only inside its object
  global,  // resolved across objects and exported from the executable
  hidden,  // resolved across objects, not exported (Mach-O private extern)
};

// Generated names own their spelling. ELF input names can share one immutable string table.
class symbol_name {
  struct table_slice {
    std::shared_ptr<const std::string> table;
    std::size_t offset, length;
  };
  std::variant<std::string, table_slice> storage_;

public:
  symbol_name() = default;
  symbol_name(std::string name) : storage_(std::move(name)) {}
  symbol_name(const char* name) : symbol_name(std::string(name)) {}
  symbol_name(std::shared_ptr<const std::string> table, std::size_t offset, std::size_t length)
    pre(table && offset <= table->size() && length <= table->size() - offset)
    : storage_(table_slice{std::move(table), offset, length}) {}

  std::string_view view() const noexcept {
    if (const auto* owned = std::get_if<std::string>(&storage_)) return *owned;
    const auto& slice = std::get<table_slice>(storage_);
    return slice.table ? std::string_view(slice.table->data() + slice.offset, slice.length) : std::string_view{};
  }
  // The structural fingerprint treats a name as text, regardless of its storage.
  operator std::string_view() const noexcept { return view(); }
  bool operator==(const symbol_name& other) const noexcept { return view() == other.view(); }
};

struct symbol {
  symbol_name name;
  binding bind = binding::global;
  std::optional<std::uint32_t> section;  // index into `object::sections`; none: undefined
  std::uint64_t offset = 0;              // in the section
  bool weak = false;                     // a weak definition, or a weak reference when undefined
  // Present for an ELF function definition. Zero records an unknown byte extent.
  std::optional<std::uint64_t> function_size = std::nullopt;
  std::optional<std::uint64_t> object_size = std::nullopt;  // sets STT_OBJECT and st_size on ELF output

  std::string_view name_view() const noexcept { return name.view(); }
  bool defined() const noexcept { return section.has_value(); }
  bool operator==(const symbol&) const = default;
};

enum class target : std::uint8_t { aarch64_macos, x86_64_linux };

// An ELF COMDAT group. On input, relocation sections have been attached to their targets.
struct comdat_group {
  symbol_name signature;
  std::vector<std::uint32_t> members;  // indices into object::sections

  bool operator==(const comdat_group&) const = default;
};

struct object {
  std::string name;  // for diagnostics: the file name or the TU
  target arch = target::aarch64_macos;
  std::vector<section> sections;
  std::vector<symbol> symbols;
  std::vector<comdat_group> comdat_groups;
  std::optional<std::uint32_t> elf_x86_feature_1_and = std::nullopt;  // GNU property, if present

  std::uint32_t add_section(section s) {
    sections.push_back(std::move(s));
    return static_cast<std::uint32_t>(sections.size() - 1);
  }
  std::uint32_t add_section(standard_section kind) { return add_section(section::standard(kind)); }

  std::uint32_t add_symbol(symbol s) {
    symbols.push_back(std::move(s));
    return static_cast<std::uint32_t>(symbols.size() - 1);
  }
  // A symbol defined at `offset` in `sec`, by default at the section's current end.
  std::uint32_t define(std::string name, binding bind, std::uint32_t sec, std::optional<std::uint64_t> offset = std::nullopt) {
    return add_symbol({std::move(name), bind, sec, offset.value_or(sections[sec].size()), false});
  }
  // The undefined symbol `name`, added on first use.
  std::uint32_t reference(std::string_view name) {
    for (std::uint32_t i = 0; i < symbols.size(); ++i) {
      if (!symbols[i].defined() && symbols[i].name_view() == name) return i;
    }
    return add_symbol({std::string(name), binding::global, std::nullopt, 0, false});
  }

  bool operator==(const object&) const = default;
};

section section::standard(standard_section kind) {
  using namespace section_flags;
  switch (kind) {
    case standard_section::text: return {"__TEXT", "__text", regular | pure_instructions | some_instructions, 2, {}, 0, {}};
    case standard_section::cstring: return {"__TEXT", "__cstring", cstring_literals, 0, {}, 0, {}};
    case standard_section::rodata: return {"__TEXT", "__const", regular, 0, {}, 0, {}};
    case standard_section::data_const: return {"__DATA", "__const", regular, 3, {}, 0, {}};
    case standard_section::data: return {"__DATA", "__data", regular, 0, {}, 0, {}};
    case standard_section::bss: return {"__DATA", "__bss", zerofill, 0, {}, 0, {}};
    case standard_section::mod_init_func: return {"__DATA", "__mod_init_func", mod_init_func_pointers, 3, {}, 0, {}};
    case standard_section::init_array: return {"__DATA", "__init_array", regular, 3, {}, 0, {}};
    case standard_section::eh_frame: return {"__TEXT", "__eh_frame", regular, 3, {}, 0, {}};
    case standard_section::gcc_except_table: return {"__TEXT", "__gcc_except", regular, 0, {}, 0, {}};
  }
  std::unreachable();
}

}  // namespace ycxx::obj
