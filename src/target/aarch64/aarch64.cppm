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

namespace ycxx::aarch64 {

// cstring: NUL-terminated string literals; rodata: read-only without pointers; data_const: read-only
// after load (holds pointers the loader adjusts); data: writable; bss: writable, zero.
export enum class DataKind : std::uint8_t { cstring, rodata, data_const, data, bss };

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
// into `targets`, in order of first use.
export struct MachineCode {
  std::vector<std::uint8_t> bytes;
  std::vector<obj::reloc> relocs;
  std::vector<ir::EntityId> targets;
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
  return out;
}

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
  for (DataKind k : {DataKind::cstring, DataKind::rodata, DataKind::data_const, DataKind::data, DataKind::bss}) {
    if (!std::ranges::any_of(data, [&](const DataDef& d) { return d.kind == k; })) continue;
    obj::standard_section s = k == DataKind::cstring  ? obj::standard_section::cstring
                              : k == DataKind::rodata ? obj::standard_section::rodata
                              : k == DataKind::data_const ? obj::standard_section::data_const
                              : k == DataKind::data   ? obj::standard_section::data
                                                      : obj::standard_section::bss;
    sections[k] = o.add_section(s);
  }

  // Defined symbols first, functions then variables, at offsets filled in as they are emitted.
  for (const CompiledFunction& f : functions) {
    auto s = name_of(f.entity);
    if (!s) return std::unexpected(s.error());
    symbol_of[ir::raw(f.entity)] = o.add_symbol({(*s)->name, (*s)->bind, *text, 0, false});
  }
  for (const DataDef& d : data) {
    auto s = name_of(d.entity);
    if (!s) return std::unexpected(s.error());
    symbol_of[ir::raw(d.entity)] = o.add_symbol({(*s)->name, (*s)->bind, sections[d.kind], 0, false});
  }
  std::optional<CodegenError> missing;
  auto symbol = [&](ir::EntityId e) -> std::uint32_t {
    if (auto it = symbol_of.find(ir::raw(e)); it != symbol_of.end()) return it->second;
    auto s = name_of(e);
    if (!s) {
      if (!missing) missing = s.error();
      return 0;
    }
    std::uint32_t i = o.reference((*s)->name);
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
    o.symbols[symbol_of[ir::raw(f.entity)]].offset = base;
    sec.emit(f.code.bytes);
    for (obj::reloc r : f.code.relocs) {
      r.offset += base;
      r.symbol = symbol(f.code.targets[r.symbol]);
      sec.relocs.push_back(r);
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
    const auto reg = [&](Field f) {
      const std::uint32_t n = field(f);
      const bool x = memory ? (f == Field::rn ? true : access == 3 || form.ends_with("pre") || form.ends_with("post") || form == "LDRgot") : wide;
      if (n == 31) return std::string((f == Field::rn && sp_base) || (f == Field::rd && sp_dest) ? (x ? "sp" : "wsp") : (x ? "xzr" : "wzr"));
      return reg_name(R(n), x);
    };
    std::string mn = mnemonic(form);
    std::vector<std::string> ops;
    std::string mem_base, mem_off;
    for (const Opnd& o : s.ops.view()) {
      switch (o.role) {
        case Role::def:
        case Role::use:
          if (memory && o.field == Field::rn) mem_base = reg(o.field);
          else ops.push_back(reg(o.field));
          break;
        case Role::imm:
          switch (o.field) {
            case Field::uoff12: mem_off = std::format("#{}", field(o.field) << access); break;
            case Field::imm7x8: mem_off = std::format("#{}", sign_extend(field(o.field), 7) * 8); break;
            case Field::hw:
              if (field(o.field)) ops.back() += std::format(", lsl #{}", field(o.field) * 16);
              break;
            case Field::tbit: ops.push_back(std::format("#{}", (w >> 31) << 5 | ((w >> 19) & 31))); break;
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
          if (o.field == Field::bcond) mn += "." + std::string(cond_names[field(o.field)]);
          else ops.push_back(std::string(cond_names[field(o.field) & 15]));
          break;
        case Role::frame: break;
      }
    }
    if (form.ends_with("12") && form.starts_with("ADDri") ) ops.back() += ", lsl #12";
    else if (form.ends_with("12") && form.starts_with("SUBri")) ops.back() += ", lsl #12";
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
