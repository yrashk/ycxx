// ycxx's AArch64 back end (optimization design §5, §10; #28): the target description as data
// (isa.cppm's forms and encodings, aarch64.patterns, abi.cppm's register classes and calling
// convention), maximal-munch selection to L3, stack-slot allocation, and emission into the
// in-memory object file (`ycxx.obj`) that the Mach-O writer and ycxx's linker read.
//
// `compile_function` is level 0 of `optimized(unit, …)`, one function's machine code; `assemble`
// puts functions and data into an object in the order its caller gives (the pipeline's `tu` query:
// definition order, query-engine design §A5.3). `codegen(unit)` does both, in entity-id order.
export module ycxx.target.aarch64;

export import :isa;
export import :abi;
export import :mir;
export import :patterns;
export import :select;
export import :emit;

import std;
import ycxx.ir;
import ycxx.ir.l2;
import ycxx.obj;
import ycxx.obj.macho;

namespace ycxx::aarch64 {

// cstring: NUL-terminated string literals; rodata: read-only without pointers; data_const: read-only
// after load (holds pointers the loader adjusts); data: writable; bss: writable, zero.
// `init_func` is a pointer in the list of functions run before `main` ([basic.start.dynamic]).
export enum class DataKind : std::uint8_t { cstring, rodata, data_const, data, bss, init_func };

// A pointer inside a variable's initial bytes: the address of `target` plus `addend`, at `offset`.
export struct DataReloc {
  std::uint64_t offset = 0;
  ir::EntityId target{};
  std::int64_t addend = 0;
};

// A namespace-scope variable or a string literal with its constant initializer, as constant
// evaluation produced it. A `bss` variable has only a size.
export struct DataDef {
  ir::EntityId entity{};
  DataKind kind = DataKind::data;
  std::vector<std::uint8_t> bytes;
  std::uint64_t size = 0;  // bss
  std::uint32_t align = 1;
  std::vector<DataReloc> relocs;
  bool weak = false;
};

export struct FunctionDef {
  ir::EntityId entity{};
  l2::Function body;
};

// The linkage name and binding of every entity the unit defines or refers to.
export struct Symbol {
  ir::EntityId entity{};
  std::string name;  // as the object file spells it ("_main")
  obj::binding bind = obj::binding::global;
};

export struct Unit {
  std::string name;  // the object's name, for diagnostics
  std::vector<FunctionDef> functions;
  std::vector<DataDef> data;
  std::vector<Symbol> symbols;
};

export struct CodegenError {
  ir::EntityId entity{};
  std::string message;
};

// One function's machine code, apart from any object (the value of `optimized(unit, …, level 0)`):
// its bytes, which start 4-byte aligned, and their relocations, each naming its target by an index
// into `targets`, in order of first use, and the emitted changes in the frame rules.
export struct MachineCode {
  std::vector<std::uint8_t> bytes;
  std::vector<obj::reloc> relocs;
  std::vector<ir::EntityId> targets;
  std::vector<FrameRow> frame_rows;
  std::optional<CleanupSite> cleanup;
};

// Assembly text of a function's code, for reading (the engine trace, #245): one line per word,
// `offset:  word  mnemonic operands`, decoded with the form table the encoder uses, so it shows
// exactly the forms code generation emits. A relocated operand is the target's `name`; a branch
// shows its destination's offset. A word no form matches is `.word`.
export std::string disassemble(const MachineCode& m, const std::function<std::string(ir::EntityId)>& name);

// Selection, slot allocation and emission of one function. `defined` lists the entities whose
// addresses are formed directly, sorted by handle (`select`).
export std::expected<MachineCode, std::string> compile_function(const l2::Function& body, std::span<const ir::EntityId> defined,
                                                                const CallingConvention& cc = apple_arm64);

// The object of compiled functions and data, in the order given: defined symbols first, functions
// then data, then undefined ones in order of first use (query-engine design §A5.3).
export struct CompiledFunction {
  ir::EntityId entity{};
  MachineCode code;
  bool cleanup_free_unwind = false;
  bool weak = false;
};
export std::expected<obj::object, CodegenError> assemble(std::string name, std::span<const CompiledFunction> functions,
                                                         std::span<const DataDef> data, std::span<const Symbol> symbols);

// The whole unit, functions and variables in entity-id order.
export std::expected<obj::object, CodegenError> codegen(const Unit& unit, const CallingConvention& cc = apple_arm64);

}  // namespace ycxx::aarch64

namespace ycxx::aarch64 {

std::expected<MachineCode, std::string> compile_function(const l2::Function& body, std::span<const ir::EntityId> defined,
                                                         const CallingConvention& cc) {
  auto m = select(body, defined, cc);
  if (!m) return std::unexpected(m.error());
  allocate_slots(*m, cc);
  MachineCode out;
  obj::section text = obj::section::standard(obj::standard_section::text);
  auto target = [&](ir::EntityId e) -> std::uint32_t {
    for (std::uint32_t i = 0; i < out.targets.size(); ++i)
      if (out.targets[i] == e) return i;
    out.targets.push_back(e);
    return static_cast<std::uint32_t>(out.targets.size() - 1);
  };
  auto e = emit(*m, text, target, cc);
  if (!e) return std::unexpected(e.error());
  out.bytes = std::move(text.bytes);
  out.relocs = std::move(text.relocs);
  out.frame_rows = std::move(e->frame_rows);
  out.cleanup = e->cleanup;
  return out;
}

namespace {

struct EhFrameCies {
  std::optional<std::uint32_t> ordinary, cleanup;
};

struct EhFrameFields {
  std::uint32_t pc;
  std::optional<std::uint32_t> lsda;
};

// DWARF §6.4 and the AArch64 DWARF register mapping.
constexpr std::uint8_t dwarf_fp = 29, dwarf_lr = 30, dwarf_sp = 31;
constexpr std::uint8_t def_cfa = 0x0c, same_value = 0x08, offset = 0x80, restore = 0xc0;
constexpr std::uint8_t pcrel_absptr = 0x10;

std::expected<std::uint32_t, std::string> append_cie(obj::section& section, bool cleanup,
                                                      std::optional<std::uint32_t> personality) {
  obj::macho::byte_writer record;
  record.u32(0);  // length
  record.u32(0);  // CIE ID
  record.u8(1);   // version
  record.raw(cleanup ? "zPLR" : "zR");
  record.u8(0);
  record.u8(1);     // code alignment
  record.u8(0x78);  // data alignment -8
  record.u8(dwarf_lr);
  record.u8(cleanup ? 7 : 1);  // augmentation length
  std::optional<std::size_t> field;
  if (cleanup) {
    if (!personality) return std::unexpected("a hosted cleanup needs a personality symbol");
    record.u8(0x9b);  // indirect PC-relative sdata4
    field = section.bytes.size() + record.size();
    record.u32(0);
    record.u8(pcrel_absptr);  // LSDA encoding
  }
  record.u8(pcrel_absptr);  // FDE initial-location encoding
  constexpr std::uint8_t entry[]{def_cfa, dwarf_sp, 0, same_value, dwarf_fp, same_value, dwarf_lr};
  record.raw(entry);
  record.align(8);
  const auto start = section.bytes.size();
  if (!std::in_range<std::int32_t>(start + record.size()))
    return std::unexpected("the unwind section exceeds the Mach-O relocation offset limit");
  obj::macho::store32(record.bytes, 0, static_cast<std::uint32_t>(record.size() - 4));
  section.emit(record.bytes);
  if (field) section.relocate(obj::reloc_kind::got_rel32, *personality, 0, static_cast<std::uint32_t>(*field));
  return static_cast<std::uint32_t>(start);
}

std::expected<EhFrameFields, std::string> append_eh_frame(obj::section& section, EhFrameCies& cies,
                                                          const MachineCode& code,
                                                          std::optional<std::uint32_t> personality) {
  if (code.bytes.size() % 4 != 0 || code.frame_rows.size() < 2 ||
      code.frame_rows[0].offset != 4 || code.frame_rows[0].state != FrameState::saved_pair ||
      code.frame_rows[1].offset != 8 || code.frame_rows[1].state != FrameState::frame_pointer)
    return std::unexpected("hosted unwind requires an Apple frame record");

  const bool cleanup = code.cleanup.has_value();
  auto& cie = cleanup ? cies.cleanup : cies.ordinary;
  if (!cie) {
    auto appended = append_cie(section, cleanup, personality);
    if (!appended) return std::unexpected(appended.error());
    cie = *appended;
  }
  obj::macho::byte_writer fde;
  const auto start = section.bytes.size();
  constexpr std::size_t fde_header = 4 + 4 + 8 + 8 + 1;  // through the augmentation length
  if (!std::in_range<std::int32_t>(start + fde_header + (cleanup ? 8 : 0)))
    return std::unexpected("the unwind section exceeds the Mach-O relocation offset limit");
  EhFrameFields fields{static_cast<std::uint32_t>(start + 8), std::nullopt};
  fde.u32(0);  // record length, filled after emission
  fde.u32(static_cast<std::uint32_t>(start + 4 - *cie));
  fde.u64(0);  // PC-relative initial location, relocated by the caller
  fde.u64(code.bytes.size());
  fde.u8(cleanup ? 8 : 0);
  if (cleanup) {
    fields.lsda = static_cast<std::uint32_t>(start + fde.size());
    fde.u64(0);  // PC-relative LSDA pointer, relocated by the caller
  }
  std::uint32_t previous = 0;
  for (const auto& row : code.frame_rows) {
    if (row.offset <= previous || row.offset >= code.bytes.size() || row.offset % 4 != 0)
      return std::unexpected("an unwind row is outside the function's instruction range");
    const auto advance = row.offset - previous;
    if (advance <= 0x3f) {
      fde.u8(0x40 | static_cast<std::uint8_t>(advance));  // DW_CFA_advance_loc
    } else if (advance <= 0xff) {
      fde.u8(0x02);
      fde.u8(static_cast<std::uint8_t>(advance));
    } else if (advance <= 0xffff) {
      fde.u8(0x03);
      fde.u16(static_cast<std::uint16_t>(advance));
    } else {
      fde.u8(0x04);
      fde.u32(advance);
    }
    fde.u8(def_cfa);
    fde.u8(row.state == FrameState::frame_pointer ? dwarf_fp : dwarf_sp);
    if (row.state == FrameState::entry) {
      fde.u8(0);
      fde.u8(restore | dwarf_fp);
      fde.u8(restore | dwarf_lr);
    } else {
      fde.u8(16);
      fde.u8(offset | dwarf_fp);
      fde.u8(2);  // FP is at CFA - 16
      fde.u8(offset | dwarf_lr);
      fde.u8(1);  // LR is at CFA - 8
    }
    previous = row.offset;
  }
  fde.align(8);  // zero padding is DW_CFA_nop
  if (!std::in_range<std::int32_t>(start + fde.size()))
    return std::unexpected("the unwind section exceeds the Mach-O relocation offset limit");
  obj::macho::store32(fde.bytes, 0, static_cast<std::uint32_t>(fde.size() - 4));
  section.emit(fde.bytes);
  return fields;
}

std::expected<std::vector<std::uint8_t>, std::string> cleanup_lsda(const MachineCode& code) {
  const auto [call, landing] = *code.cleanup;
  const auto size = code.bytes.size();
  if (size < 4 || call > size - 4 || call % 4 || landing % 4 || landing == 0 || landing >= size || landing == call ||
      (obj::macho::load32(code.bytes, call) & 0xfc000000) != 0x94000000 ||
      !std::ranges::any_of(code.relocs, [&](const obj::reloc& r) {
        return r.kind == obj::reloc_kind::branch26 && r.offset == call;
      }))
    return std::unexpected("a hosted cleanup needs a relocated direct call and a distinct landing pad");

  obj::macho::byte_writer sites;
  auto row = [&](std::size_t begin, std::size_t end, std::uint32_t pad) {
    if (begin == end) return;
    sites.uleb(begin);
    sites.uleb(end - begin);
    sites.uleb(pad);
    sites.uleb(0);  // action zero with a landing pad is a cleanup
  };
  row(0, call, 0);
  row(call, call + 4, landing);
  row(call + 4, size, 0);

  obj::macho::byte_writer table;
  table.u8(0xff);  // omit LPStart, use the FDE's initial PC
  table.u8(0xff);  // omit the type table
  table.u8(0x01);  // call-site fields use ULEB128
  table.uleb(sites.size());
  table.raw(sites.bytes);
  return std::move(table.bytes);
}

}  // namespace

std::expected<obj::object, CodegenError> assemble(std::string name, std::span<const CompiledFunction> functions,
                                                  std::span<const DataDef> data, std::span<const Symbol> symbols) {
  std::map<std::uint32_t, const Symbol*> names;
  for (const Symbol& s : symbols) names[ir::raw(s.entity)] = &s;
  auto name_of = [&](ir::EntityId e) -> std::expected<const Symbol*, CodegenError> {
    auto it = names.find(ir::raw(e));
    if (it == names.end()) return std::unexpected(CodegenError{e, std::format("@{} has no symbol name", ir::raw(e))});
    return it->second;
  };

  obj::object o;
  o.name = std::move(name);
  std::map<std::uint32_t, std::uint32_t> symbol_of;  // entity → symbol index

  // Sections in a fixed order, only those with content.
  std::optional<std::uint32_t> text;
  std::map<DataKind, std::uint32_t> sections;
  if (!functions.empty()) text = o.add_section(obj::standard_section::text);
  for (DataKind k : {DataKind::cstring, DataKind::rodata, DataKind::data_const, DataKind::data, DataKind::bss,
                     DataKind::init_func}) {
    if (!std::ranges::any_of(data, [&](const DataDef& d) { return d.kind == k; })) continue;
    obj::standard_section s = k == DataKind::cstring  ? obj::standard_section::cstring
                              : k == DataKind::rodata ? obj::standard_section::rodata
                              : k == DataKind::data_const ? obj::standard_section::data_const
                              : k == DataKind::data   ? obj::standard_section::data
                              : k == DataKind::bss    ? obj::standard_section::bss
                                                      : obj::standard_section::mod_init_func;
    sections[k] = o.add_section(s);
  }
  const bool has_cleanup = std::ranges::any_of(functions, [](const CompiledFunction& f) {
    return f.code.cleanup.has_value();
  });
  std::optional<std::uint32_t> frames, tables;
  if (has_cleanup || std::ranges::any_of(functions, &CompiledFunction::cleanup_free_unwind)) {
    frames = o.add_section(obj::standard_section::eh_frame);
    o.sections[*frames].flags = obj::section_flags::coalesced | obj::section_flags::no_toc |
                                obj::section_flags::strip_static_syms | obj::section_flags::live_support;
  }
  if (has_cleanup)
    tables = o.add_section(obj::section{"__TEXT", "__gcc_except_tab", obj::section_flags::regular, 2, {}, 0, {}});

  // Defined symbols first, functions then variables, at offsets filled in as they are emitted.
  for (const CompiledFunction& f : functions) {
    auto s = name_of(f.entity);
    if (!s) return std::unexpected(s.error());
    const bool weak = f.weak && (*s)->bind != obj::binding::local;
    symbol_of[ir::raw(f.entity)] = o.add_symbol({(*s)->name, (*s)->bind, *text, 0, weak});
  }
  for (const DataDef& d : data) {
    auto s = name_of(d.entity);
    if (!s) return std::unexpected(s.error());
    const bool weak = d.weak && (*s)->bind != obj::binding::local;
    symbol_of[ir::raw(d.entity)] = o.add_symbol({(*s)->name, (*s)->bind, sections[d.kind], 0, weak});
  }
  const auto frame_base = frames ? o.define("ltmp_eh_frame", obj::binding::local, *frames) : 0;
  std::optional<std::uint32_t> personality;
  if (has_cleanup) personality = o.reference("___gxx_personality_v0");
  EhFrameCies cies;
  std::optional<CodegenError> missing;
  auto symbol = [&](ir::EntityId e) -> std::uint32_t {
    if (auto it = symbol_of.find(ir::raw(e)); it != symbol_of.end()) return it->second;
    auto s = name_of(e);
    if (!s) {
      if (!missing) missing = s.error();
      return 0;
    }
    // Another entity may define the name: a builtin's implicit C declaration and a written
    // definition of another type are two entities with one symbol (#1095). Name the strong
    // definition where a weak one shares its spelling (#694), as the linker resolves it.
    auto defines = [&](const obj::symbol& d) {
      return d.defined() && d.bind != obj::binding::local && d.name_view() == (*s)->name;
    };
    auto defined = o.symbols.end();
    if ((*s)->bind != obj::binding::local) {
      defined = std::ranges::find_if(o.symbols, [&](const obj::symbol& d) { return defines(d) && !d.weak; });
      if (defined == o.symbols.end()) defined = std::ranges::find_if(o.symbols, defines);
    }
    const std::uint32_t i = defined != o.symbols.end() ? static_cast<std::uint32_t>(defined - o.symbols.begin())
                                                        : o.reference((*s)->name);
    symbol_of[ir::raw(e)] = i;
    return i;
  };

  for (const DataDef& d : data) {
    obj::section& sec = o.sections[sections[d.kind]];
    std::uint8_t log2 = 0;
    while ((1u << log2) < d.align) ++log2;
    sec.align(log2);
    o.symbols[symbol_of[ir::raw(d.entity)]].offset = sec.size();
    if (d.kind == DataKind::bss) {
      sec.reserve_zerofill(d.size);
      continue;
    }
    std::uint64_t at = sec.bytes.size();
    sec.emit(d.bytes);
    for (const DataReloc& r : d.relocs)
      sec.relocate(obj::reloc_kind::abs64, symbol(r.target), r.addend, std::uint32_t(at + r.offset));
  }

  for (const CompiledFunction& f : functions) {
    obj::section& sec = o.sections[*text];
    sec.align(2);
    const auto base = static_cast<std::uint32_t>(sec.bytes.size());
    const auto fn_symbol = symbol_of[ir::raw(f.entity)];
    o.symbols[fn_symbol].offset = base;
    sec.emit(f.code.bytes);
    for (obj::reloc r : f.code.relocs) {
      r.offset += base;
      r.symbol = symbol(f.code.targets[r.symbol]);
      sec.relocs.push_back(r);
    }
    if (frames && (f.cleanup_free_unwind || f.code.cleanup)) {
      auto& unwind = o.sections[*frames];
      auto fields = append_eh_frame(unwind, cies, f.code, personality);
      if (!fields) return std::unexpected(CodegenError{f.entity, fields.error()});
      // A weak definition's name may resolve to another object's code, but this FDE describes
      // these bytes: name them with a local label (as the ELF path does).
      const auto code = o.symbols[fn_symbol].weak ? o.define(std::format("ltmp_code_{}", base), obj::binding::local, *text, base)
                                                  : fn_symbol;
      unwind.relocate(obj::reloc_kind::abs64, code, -static_cast<std::int64_t>(fields->pc), fields->pc, frame_base);
      if (fields->lsda && tables) {
        auto table = cleanup_lsda(f.code);
        if (!table) return std::unexpected(CodegenError{f.entity, table.error()});
        auto& section = o.sections[*tables];
        section.align(2);
        const auto offset = section.size();
        if (!std::in_range<std::int32_t>(offset + table->size()))
          return std::unexpected(CodegenError{f.entity, "the exception tables exceed the Mach-O object offset limit"});
        const auto anchor = o.define(std::format("ltmp_lsda_{}", offset), obj::binding::local, *tables);
        section.emit(*table);
        unwind.relocate(obj::reloc_kind::abs64, anchor, -static_cast<std::int64_t>(*fields->lsda), *fields->lsda, frame_base);
      }
    }
  }
  if (missing) return std::unexpected(*missing);
  return o;
}

std::expected<obj::object, CodegenError> codegen(const Unit& unit, const CallingConvention& cc) {
  auto by_entity = [](const auto& x) { return ir::raw(x.entity); };
  std::vector<const FunctionDef*> functions;
  for (const auto& f : unit.functions) functions.push_back(&f);
  std::ranges::sort(functions, {}, [&](const FunctionDef* f) { return by_entity(*f); });
  std::vector<DataDef> data(unit.data.begin(), unit.data.end());
  std::ranges::sort(data, {}, by_entity);

  std::vector<ir::EntityId> defined;
  for (auto* f : functions) defined.push_back(f->entity);
  for (const DataDef& d : data) defined.push_back(d.entity);
  std::ranges::sort(defined, {}, [](ir::EntityId e) { return ir::raw(e); });

  std::vector<CompiledFunction> compiled;
  for (auto* f : functions) {
    auto m = compile_function(f->body, defined, cc);
    if (!m) return std::unexpected(CodegenError{f->entity, m.error()});
    compiled.push_back({f->entity, std::move(*m)});
  }
  return assemble(unit.name, compiled, data, unit.symbols);
}

}  // namespace ycxx::aarch64

namespace ycxx::aarch64 {
namespace {

// The bits of a word a form leaves to its operands and its width.
constexpr std::uint32_t variable_bits(const FormSpec& s) {
  std::uint32_t v = 0;
  for (const Opnd& o : s.ops.view()) {
    if (o.field == Field::tbit) v |= 1u << 31 | 0x1fu << 19;
    else if (auto [lsb, width] = place(o.field); width) v |= ((width == 32 ? 0u : (1u << width)) - 1) << lsb;
  }
  switch (s.size) {
    case Size::fixed: break;
    case Size::sf: v |= 1u << 31; break;
    case Size::sfn: v |= 1u << 31 | 1u << 22; break;
    case Size::mem: v |= 3u << 30; break;
    case Size::ftype: v |= 1u << 22; break;
    case Size::sfft: v |= 1u << 31 | 1u << 22; break;
  }
  return v;
}

constexpr Reloc reloc_of(obj::reloc_kind k) {
  switch (k) {
    case obj::reloc_kind::branch26: return Reloc::branch26;
    case obj::reloc_kind::page21: return Reloc::page21;
    case obj::reloc_kind::pageoff12: return Reloc::pageoff12;
    case obj::reloc_kind::got_page21: return Reloc::got_page21;
    case obj::reloc_kind::got_pageoff12: return Reloc::got_pageoff12;
    default: return Reloc::none;
  }
}

// `ADDri12` -> `add`, `Bcc` -> `b`, `TST1` -> `tst`: the instruction's own name, lower case.
std::string mnemonic(std::string_view form) {
  std::string out;
  for (const char c : form) {
    if (c < 'A' || c > 'Z') break;
    out += static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::int64_t sign_extend(std::uint32_t v, std::uint32_t width) {
  const std::uint32_t m = 1u << (width - 1);
  return static_cast<std::int64_t>((v ^ m)) - m;
}

}  // namespace

std::string disassemble(const MachineCode& m, const std::function<std::string(ir::EntityId)>& name) {
  std::string out;
  for (std::size_t at = 0; at + 4 <= m.bytes.size(); at += 4) {
    const std::uint32_t w = m.bytes[at] | m.bytes[at + 1] << 8 | m.bytes[at + 2] << 16 | std::uint32_t(m.bytes[at + 3]) << 24;
    const obj::reloc* rel = nullptr;
    for (const obj::reloc& r : m.relocs) {
      if (r.offset == at) rel = &r;
    }
    const Reloc want = rel ? reloc_of(rel->kind) : Reloc::none;
    // The most specific real form whose fixed bits match and whose relocation is the one here.
    std::optional<std::size_t> best;
    int best_fixed = -1;
    for (std::size_t f = 0; f < form_count; ++f) {
      const FormSpec& s = form_table[f].spec;
      if (s.pseudo || s.reloc != want) continue;
      const std::uint32_t fixed = ~variable_bits(s);
      if ((w & fixed) != (s.bits & fixed)) continue;
      const int n = std::popcount(fixed);
      if (n > best_fixed) best = f, best_fixed = n;
    }
    std::format_to(std::back_inserter(out), "{:4x}:  {:08x}  ", at, w);
    if (!best) {
      std::format_to(std::back_inserter(out), ".word 0x{:08x}\n", w);
      continue;
    }
    const FormInfo& fi = form_table[*best];
    const FormSpec& s = fi.spec;
    const std::string_view form = fi.name;
    const bool wide = s.size == Size::sf || s.size == Size::sfn ? (w >> 31) != 0 : true;
    const std::uint32_t access = s.size == Size::mem ? w >> 30 : 3;
    const bool memory = form.starts_with("LDR") || form.starts_with("STR") || form.starts_with("LDP") || form.starts_with("STP");
    // Register 31 is sp as an ADD/SUB (immediate)'s destination or source and as a base, else zr.
    const bool sp_dest = form.starts_with("ADDri") || form.starts_with("SUBri") || form == "ADDlo";
    const bool sp_base = memory || sp_dest || form == "SUBSri";
    const auto field = [&](Field f) {
      auto [lsb, width] = place(f);
      return (w >> lsb) & ((1u << width) - 1);
    };
    // A SIMD&FP register's view: by the precision bit, the access size, or the form's name.
    const auto fp_view = [&](const Opnd& o) {
      if (s.size == Size::mem) return "bhsd"[access];
      if (s.size == Size::ftype || s.size == Size::sfft) return (w >> 22) & 1 ? 'd' : 's';
      if (form == "FCVTsd") return o.role == Role::def ? 'd' : 's';
      if (form == "FCVTds") return o.role == Role::def ? 's' : 'd';
      return form.back();  // a conversion's floating operand: FCVTZSd, SCVTFs
    };
    const auto reg = [&](const Opnd& o) {
      const Field f = o.field;
      const std::uint32_t n = field(f);
      if (o.fp) return std::format("{}{}", fp_view(o), n);
      const bool x = memory ? (f == Field::rn ? true : access == 3 || form.ends_with("pre") || form.ends_with("post") || form == "LDRgot") : wide;
      if (n == 31) return std::string((f == Field::rn && sp_base) || (f == Field::rd && sp_dest) ? (x ? "sp" : "wsp") : (x ? "xzr" : "wzr"));
      return reg_name(R(n), x);
    };
    std::string mn = mnemonic(form);
    // A byte or halfword access says so (`ldrb`, `strh`); the register is then a w register.
    if (s.size == Size::mem && access < 2 && !s.ops.at[0].fp) mn += access == 0 ? "b" : "h";
    std::vector<std::string> ops;
    std::string mem_base, mem_off;
    for (const Opnd& o : s.ops.view()) {
      switch (o.role) {
        case Role::def:
        case Role::use:
          if (memory && o.field == Field::rn) mem_base = reg(o);
          else ops.push_back(reg(o));
          break;
        case Role::imm:
          switch (o.field) {
            case Field::uoff12: mem_off = std::format("#{}", field(o.field) << access); break;
            case Field::imm7x8: mem_off = std::format("#{}", sign_extend(field(o.field), 7) * 8); break;
            case Field::hw:
              if (field(o.field)) ops.back() += std::format(", lsl #{}", field(o.field) * 16);
              break;
            case Field::tbit:
              // b5 clear tests a w register.
              if ((w >> 31) == 0 && !ops.empty() && ops.back().starts_with('x')) ops.back()[0] = 'w';
              ops.push_back(std::format("#{}", (w >> 31) << 5 | ((w >> 19) & 31)));
              break;
            default: ops.push_back(std::format("#{}", field(o.field)));
          }
          break;
        case Role::sym: {
          std::string target = rel && rel->symbol < m.targets.size() ? name(m.targets[rel->symbol]) : "?";
          if (rel && rel->addend) target += std::format("+{}", rel->addend);
          if (s.reloc == Reloc::pageoff12 || s.reloc == Reloc::got_pageoff12) target = std::format(":lo12:{}{}", target, s.reloc == Reloc::got_pageoff12 ? "@got" : "");
          else if (s.reloc == Reloc::page21 || s.reloc == Reloc::got_page21) target += s.reloc == Reloc::got_page21 ? "@gotpage" : "@page";
          if (memory) mem_off = target;
          else ops.push_back(target);
          break;
        }
        case Role::label: {
          auto [lsb, width] = place(o.field);
          const std::int64_t off = sign_extend(field(o.field), width) * 4;
          ops.push_back(std::format("0x{:x}", static_cast<std::int64_t>(at) + off));
          break;
        }
        case Role::cond:
        {
          const std::uint32_t c = field(o.field);
          const std::string name = c < std::size(cond_names) ? std::string(cond_names[c]) : "nv";
          if (o.field == Field::bcond) mn += "." + name;
          else ops.push_back(name);
        }
          break;
        case Role::frame: break;
      }
    }
    if (form.ends_with("12") && form.starts_with("ADDri") ) ops.back() += ", lsl #12";
    else if (form.ends_with("12") && form.starts_with("SUBri")) ops.back() += ", lsl #12";
    if (form == "TST1") {  // ANDS wzr, Wn, #1: a 32-bit test of bit 0
      ops = {"w" + ops.front().substr(1), "#1"};
    }
    if (memory) {
      const std::string addr = mem_off.empty() ? std::format("[{}]", mem_base) : std::format("[{}, {}]", mem_base, mem_off);
      if (form.ends_with("pre")) ops.push_back(addr + "!");
      else if (form.ends_with("post")) ops.push_back(std::format("[{}], {}", mem_base, mem_off));
      else ops.push_back(addr);
    }
    out += mn;
    for (std::size_t i = 0; i < ops.size(); ++i) out += (i ? ", " : " ") + ops[i];
    out += '\n';
  }
  return out;
}

}  // namespace ycxx::aarch64
