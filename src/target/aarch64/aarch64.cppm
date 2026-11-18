// ycxx's AArch64 back end (optimization design §5, §10; #28): the target description as data
// (isa.cppm's forms and encodings, aarch64.patterns, abi.cppm's register classes and calling
// convention), maximal-munch selection to L3, stack-slot allocation, and emission into the
// in-memory object file (`ycxx.obj`) that the Mach-O writer and ycxx's linker read.
//
// `codegen(unit)` is level 0 of `optimized(unit, …)`: a pure function of its input, so it can be a
// query as it stands. Functions and variables are emitted in entity-id order, and symbols are
// created in that order too (query-engine design §A5.3), so equal units give equal objects.
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

export std::expected<obj::object, CodegenError> codegen(const Unit& unit, const CallingConvention& cc = apple_arm64);

}  // namespace ycxx::aarch64

namespace ycxx::aarch64 {

std::expected<obj::object, CodegenError> codegen(const Unit& unit, const CallingConvention& cc) {
  auto by_entity = [](const auto& x) { return ir::raw(x.entity); };
  std::vector<const FunctionDef*> functions;
  for (const auto& f : unit.functions) functions.push_back(&f);
  std::ranges::sort(functions, {}, [&](const FunctionDef* f) { return by_entity(*f); });
  std::vector<const DataDef*> data;
  for (const auto& d : unit.data) data.push_back(&d);
  std::ranges::sort(data, {}, [&](const DataDef* d) { return by_entity(*d); });

  std::map<std::uint32_t, const Symbol*> names;
  for (const Symbol& s : unit.symbols) names[ir::raw(s.entity)] = &s;
  auto name_of = [&](ir::EntityId e) -> std::expected<const Symbol*, CodegenError> {
    auto it = names.find(ir::raw(e));
    if (it == names.end()) return std::unexpected(CodegenError{e, std::format("@{} has no symbol name", ir::raw(e))});
    return it->second;
  };

  std::vector<ir::EntityId> defined;
  for (auto* f : functions) defined.push_back(f->entity);
  for (auto* d : data) defined.push_back(d->entity);
  std::ranges::sort(defined, {}, [](ir::EntityId e) { return ir::raw(e); });

  obj::object o;
  o.name = unit.name;
  std::map<std::uint32_t, std::uint32_t> symbol_of;  // entity → symbol index

  // Sections in a fixed order, only those with content.
  std::optional<std::uint32_t> text;
  std::map<DataKind, std::uint32_t> sections;
  if (!functions.empty()) text = o.add_section(obj::standard_section::text);
  for (DataKind k : {DataKind::cstring, DataKind::rodata, DataKind::data_const, DataKind::data, DataKind::bss}) {
    if (!std::ranges::any_of(data, [&](const DataDef* d) { return d->kind == k; })) continue;
    obj::standard_section s = k == DataKind::cstring  ? obj::standard_section::cstring
                              : k == DataKind::rodata ? obj::standard_section::rodata
                              : k == DataKind::data_const ? obj::standard_section::data_const
                              : k == DataKind::data   ? obj::standard_section::data
                                                      : obj::standard_section::bss;
    sections[k] = o.add_section(s);
  }

  // Defined symbols first, functions then variables, at offsets filled in as they are emitted.
  for (auto* f : functions) {
    auto s = name_of(f->entity);
    if (!s) return std::unexpected(s.error());
    symbol_of[ir::raw(f->entity)] = o.add_symbol({(*s)->name, (*s)->bind, *text, 0, false});
  }
  for (auto* d : data) {
    auto s = name_of(d->entity);
    if (!s) return std::unexpected(s.error());
    symbol_of[ir::raw(d->entity)] = o.add_symbol({(*s)->name, (*s)->bind, sections[d->kind], 0, false});
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

  for (auto* d : data) {
    obj::section& sec = o.sections[sections[d->kind]];
    std::uint8_t log2 = 0;
    while ((1u << log2) < d->align) ++log2;
    sec.align(log2);
    o.symbols[symbol_of[ir::raw(d->entity)]].offset = sec.size();
    if (d->kind == DataKind::bss) {
      sec.reserve_zerofill(d->size);
      continue;
    }
    std::uint64_t at = sec.bytes.size();
    sec.emit(d->bytes);
    for (const DataReloc& r : d->relocs)
      sec.relocate(obj::reloc_kind::abs64, symbol(r.target), r.addend, std::uint32_t(at + r.offset));
  }

  for (auto* f : functions) {
    auto m = select(f->body, defined, cc);
    if (!m) return std::unexpected(CodegenError{f->entity, m.error()});
    allocate_slots(*m, cc);
    o.symbols[symbol_of[ir::raw(f->entity)]].offset = o.sections[*text].bytes.size();
    auto e = emit(*m, o.sections[*text], symbol, cc);
    if (!e) return std::unexpected(CodegenError{f->entity, e.error()});
  }
  if (missing) return std::unexpected(*missing);
  return o;
}

}  // namespace ycxx::aarch64
