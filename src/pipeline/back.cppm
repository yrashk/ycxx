// The back half of a translation unit's pipeline (#43): every function definition through
// `function_ir` (ycxx.irgen), `lower` (ycxx.ir.lower) and AArch64 code generation, and every
// variable definition and string literal into data, as one object (`ycxx.obj`).
//
// Each step is the pure function its query will call (`function_ir`, `lower`, `codegen`); the
// queries themselves wait for contextual queries (#16). The `layout` query (AST design Part D) is
// `Layout` here, over the `types` and `values` interners and the target.
//
// α subset (#39): the target is AArch64 Darwin; functions and variables of the global namespace;
// constants are integers, null pointers and addresses of string literal objects.
export module ycxx.pipeline:back;

import std;
import ycxx.ast;
import ycxx.ir;
import ycxx.ir.l2;
import ycxx.ir.lower;
import ycxx.irgen;
import ycxx.obj;
import ycxx.target.aarch64;
import :mangle;

export namespace ycxx::pipeline {

namespace ast = ycxx::ast;
namespace ir = ycxx::ir;

using ast::ConstId;
using ast::EntityId;
using ast::QualType;

// What the back end asks of constants beyond integers and null pointers (ast.md §5.5): string
// literal objects and their addresses. The front end answers from its value interner; tests
// answer from tables.
class Constants {
 public:
  virtual ~Constants() = default;
  // The code units of a string literal object, the terminating null included, one byte each.
  virtual std::optional<std::vector<std::uint8_t>> string_object(ConstId c) const = 0;
  // An address constant: the string literal object it points into and the byte offset.
  struct Address {
    ConstId object = ConstId::none;
    std::int64_t offset = 0;
  };
  virtual std::optional<Address> address(ConstId c) const = 0;
  // The address of a string literal object, as `function_ir` asks for it (`Facts::string_address`).
  virtual ConstId address_of(ConstId object) = 0;
};

// An entity the translation unit declares (or defines): what its symbol is made of.
struct Entity {
  EntityId id = EntityId::none;
  std::string name;  // its identifier
  QualType type{};
  bool is_function = false;
  ast::LangLinkage lang = ast::LangLinkage::cxx;
  ast::Linkage linkage = ast::Linkage::external;
  bool is_constexpr = false;
  std::optional<ast::Loc> at;  // its first declarator, for diagnostics
};

// A variable definition at namespace scope with its constant initializer, or `ConstId::none` for
// zero-initialization ([basic.start.static]).
struct Variable {
  EntityId entity = EntityId::none;
  QualType type{};
  ConstId init = ConstId::none;
  std::optional<ast::Loc> at;  // its declarator, for diagnostics
};

struct TranslationUnit {
  std::string name;  // the object's name, for diagnostics
  std::vector<Entity> entities;
  std::vector<irgen::FunctionInput> functions;  // the definitions
  std::vector<Variable> variables;              // the definitions
  EntityId main = EntityId::none;               // the program's `main`, if this unit defines it
};

// What failed, at a location in an item when there is one. Every error here is something the α
// subset does not support yet: valid code that the front end accepted.
struct BackError {
  std::optional<ast::Loc> at;
  std::string message;
};

// `layout` (AST design Part D) for `lower`: sizes, alignments, subobjects and constant values on
// the target. String literal objects become entities of their own, numbered on first use.
class Layout {
 public:
  Layout(ast::Interners& in, const ast::Target& target, const Constants& consts)
      : in_(&in), target_(&target), consts_(&consts) {}

  ir::TypeLayout type(QualType q) const {
    using enum ir::TypeKind;
    QualType c = in_->types.canon(q);
    if (auto b = ast::builtin_of(*in_, c)) {
      if (*b == ast::Builtin::void_) return {void_, 0, 1, false, {}};
      ast::BuiltinLayout l = (*target_)[*b];
      if (*b == ast::Builtin::bool_) return {boolean, l.size, l.align, false, {}};
      if (ast::is_integral(*b)) return {integer, l.size, l.align, ast::is_signed(*target_, *b), {}};
      return {other, l.size, l.align, false, {}};
    }
    if (in_->types.is<ast::TPointer>(c))
      return {pointer, target_->pointer_size, target_->pointer_align, false, in_->canonical(ast::pointee_of(*in_, c))};
    if (in_->types.is<ast::TArray>(c)) {
      auto a = in_->types.get<ast::TArray>(c);
      ir::TypeLayout e = type(a.elem);
      std::uint64_t n = a.bound == ConstId::none ? 0 : in_->values.get<ast::VInt>(a.bound).bits.lo;
      return {array, e.size * n, e.align, false, {}};
    }
    if (in_->types.is<ast::TFunction>(c)) return {function, 0, 1, false, {}};
    return {};
  }

  ir::SubObject step(QualType q, ir::PathStep s) const {
    QualType c = in_->types.canon(q);
    if (s.step == ir::Step::elem && in_->types.is<ast::TArray>(c)) {
      QualType e = in_->canonical(in_->types.get<ast::TArray>(c).elem);
      return {s.index * type(e).size, e};
    }
    return {};  // classes come with #33's layout
  }

  ir::ConstValue constant(ConstId c) const {
    if (auto a = consts_->address(c)) return {string_entity(a->object), a->offset};
    if (in_->values.is<ast::VInt>(c)) return std::int64_t(in_->values.get<ast::VInt>(c).bits.lo);
    if (in_->values.is<ast::VNullptr>(c)) return 0;
    return 0;  // not a constant `lower` reads: `lower` reports what it cannot use
  }

  // The entity of a string literal object, created on first use.
  EntityId string_entity(ConstId object) const {
    for (const auto& [o, e] : strings_)
      if (o == object) return e;
    EntityId e = in_->entities.add<EntityId>(ast::mix(ast::seed("ycxx.pipeline.string"), in_->values.fp(object)));
    strings_.emplace_back(object, e);
    return e;
  }
  // The string literal objects used so far, in order of first use.
  std::span<const std::pair<ConstId, EntityId>> strings() const { return strings_; }

 private:
  ast::Interners* in_;
  const ast::Target* target_;
  const Constants* consts_;
  mutable std::vector<std::pair<ConstId, EntityId>> strings_;  // the layout query's own memo
};

// The objects of one translation unit, or what the α subset cannot compile yet.
std::expected<obj::object, std::vector<BackError>> compile(const TranslationUnit& tu, ast::Interners& in,
                                                           Constants& consts,
                                                           const ast::Target& target = ast::aarch64_darwin);

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

// `Facts` for `function_ir`: types and integer constants from the interners, string literals from
// `Constants`, declarations from the unit's entity list.
class UnitFacts : public irgen::TypeFacts {
 public:
  UnitFacts(ast::Interners& in, const ast::Target& target, Constants& consts, const TranslationUnit& tu)
      : TypeFacts(in, target), consts_(&consts), tu_(&tu) {}
  ConstId string_address(ConstId c) { return consts_->address_of(c); }
  bool is_constexpr(EntityId e) {
    return std::ranges::any_of(tu_->entities, [&](const Entity& x) { return x.id == e && x.is_constexpr; });
  }
  bool is_main(EntityId e) { return e != EntityId::none && e == tu_->main; }

 private:
  Constants* consts_;
  const TranslationUnit* tu_;
};

static_assert(irgen::Facts<UnitFacts>);
static_assert(ir::layout_source<Layout>);

// A variable's initial bytes, little-endian, with the pointers in them.
struct Bytes {
  std::vector<std::uint8_t> bytes;
  std::vector<aarch64::DataReloc> relocs;
};

std::expected<Bytes, std::string> initial_bytes(const ast::Interners& in, const Layout& layout, QualType type, ConstId init) {
  ir::TypeLayout l = layout.type(type);
  Bytes out;
  out.bytes.assign(l.size, 0);
  if (init == ConstId::none) return out;
  if (l.kind == ir::TypeKind::pointer) {
    ir::ConstValue v = layout.constant(init);
    if (v.base) out.relocs.push_back({0, *v.base, v.bits});
    else if (v.bits != 0) return std::unexpected("a pointer initialized with an integer other than zero");
    return out;
  }
  if (l.kind == ir::TypeKind::integer || l.kind == ir::TypeKind::boolean) {
    if (!in.values.is<ast::VInt>(init)) return std::unexpected("an integer variable's initializer is not an integer");
    std::uint64_t v = in.values.get<ast::VInt>(init).bits.lo;
    for (std::uint64_t i = 0; i < l.size; ++i) out.bytes[i] = std::uint8_t(v >> (8 * i));
    return out;
  }
  return std::unexpected("a variable of this type");
}

std::expected<obj::object, std::vector<BackError>> compile(const TranslationUnit& tu, ast::Interners& in,
                                                           Constants& consts, const ast::Target& target) {
  std::vector<BackError> errors;
  UnitFacts facts(in, target, consts, tu);
  Layout layout(in, target, consts);
  aarch64::Unit unit;
  unit.name = tu.name;

  // Functions: function_ir, then lower. An error points at the construct when it has a node.
  for (const irgen::FunctionInput& fn : tu.functions) {
    auto f = irgen::function_ir(in, fn, facts);
    if (!f) {
      errors.push_back({ast::Loc{fn.item, fn.body_offset}, f.error()});
      continue;
    }
    auto l2 = ir::lower(f->body, fn.entity, layout);
    if (!l2) {
      std::optional<ast::Loc> at;
      if (ir::raw(l2.error().node) < f->debug.loc.size()) at = f->debug.loc[ir::raw(l2.error().node)];
      errors.push_back({at, "lower: " + l2.error().message});
      continue;
    }
    unit.functions.push_back({fn.entity, std::move(*l2)});
  }

  // Variables: their initial bytes, by what they hold ([basic.start.static]).
  for (const Variable& v : tu.variables) {
    auto b = initial_bytes(in, layout, v.type, v.init);
    if (!b) {
      errors.push_back({v.at, std::format("{} is not supported yet", b.error())});
      continue;
    }
    bool is_const = (std::to_underlying(ast::cv_of(in.canonical(v.type))) & 1) != 0;
    bool zero = b->relocs.empty() && std::ranges::all_of(b->bytes, [](std::uint8_t x) { return x == 0; });
    aarch64::DataDef d{v.entity, aarch64::DataKind::data, std::move(b->bytes), 0, layout.type(v.type).align, std::move(b->relocs)};
    if (is_const) d.kind = d.relocs.empty() ? aarch64::DataKind::rodata : aarch64::DataKind::data_const;
    else if (zero) {
      d.kind = aarch64::DataKind::bss;
      d.size = d.bytes.size();
      d.bytes.clear();
    }
    unit.data.push_back(std::move(d));
  }

  // String literal objects, as `lower` and the variables met them ([lex.string]).
  std::uint32_t n = 0;
  for (auto [object, entity] : layout.strings()) {
    auto units = consts.string_object(object);
    if (!units) {
      errors.push_back({std::nullopt, "an address constant whose object is not a string literal"});
      continue;
    }
    unit.data.push_back({entity, aarch64::DataKind::cstring, std::move(*units), 0, 1, {}});
    unit.symbols.push_back({entity, std::format("l_.str.{}", n++), obj::binding::local});
  }

  // Symbols for the entities the object defines or refers to; a declaration nothing uses needs
  // no name.
  std::set<std::uint32_t> used;
  for (const aarch64::FunctionDef& f : unit.functions) {
    used.insert(ir::raw(f.entity));
    for (std::uint32_t n = 0; n < f.body.nodes.size(); ++n) {
      l2::Op op = f.body.nodes[n].op;
      if (op == l2::Op::symbol || op == l2::Op::call) used.insert(std::uint32_t(f.body.imms_of(ir::NodeId(n))[0]));
    }
  }
  for (const aarch64::DataDef& d : unit.data) {
    used.insert(ir::raw(d.entity));
    for (const aarch64::DataReloc& r : d.relocs) used.insert(ir::raw(r.target));
  }
  for (const Entity& e : tu.entities) {
    if (!used.contains(ir::raw(e.id))) continue;
    auto name = symbol_name(in, e.name, e.type, e.is_function, e.lang);
    if (!name) {
      errors.push_back({e.at, std::format("the name of '{}': {} is not supported yet", e.name, name.error())});
      continue;
    }
    unit.symbols.push_back({e.id, std::move(*name), e.linkage == ast::Linkage::internal ? obj::binding::local : obj::binding::global});
  }
  if (!errors.empty()) return std::unexpected(std::move(errors));

  auto o = aarch64::codegen(unit);
  if (!o) {
    std::optional<ast::Loc> at;
    for (const irgen::FunctionInput& fn : tu.functions)
      if (fn.entity == o.error().entity) at = ast::Loc{fn.item, fn.body_offset};
    return std::unexpected(std::vector<BackError>{{at, "codegen: " + o.error().message}});
  }
  return std::move(*o);
}

}  // namespace ycxx::pipeline
