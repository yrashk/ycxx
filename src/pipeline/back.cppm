// The back half of a translation unit's pipeline (#43, #63) as engine queries: every function
// definition through `function_ir` (ycxx.irgen), `lower` (ycxx.ir.lower), its optimization unit,
// `optimized` at level 0 and `codegen` (AArch64), then `tu(TU)`, whose value is the object
// (`ycxx.obj`) with every variable definition and string literal in it. An edit to one function's
// body re-executes that function's cells, `tu` and the link; every other function's cells stay
// green, because each reads its definition through a projection that backdates (§A1.8).
//
// The cells (query-engine design §A8, optimization design §10):
//
//   tu_unit(TU)              the unit's definitions: the front end's (TuFront) or one given
//   function_source(TU, F)   F's definition, projected out of the unit
//   function_gen(TU, F)      function_ir: L1 body and debug information; `function_ir(TU, F)` and
//                            `function_debug(TU, F)` project the two apart
//   lower(TU, F)             L1 → L2, through `layout`
//   unit(TU, F)              the optimization unit: F's L2 with an empty inline candidate set, and
//                            which of its references this TU defines; its value is the unit's
//                            fingerprint, the unit itself is in a content-addressed store
//   optimized(unit, target, level)  level 0: selection, slot allocation and emission
//   codegen(TU, F)           best_available(unit, B, S) over the opt_cache_snapshot input S
//   tu(TU)                   the object, in definition order (§A5.3), and the first-demand table
//   layout                   `layout_type(T)`, `layout_step(T, s)`, `layout_constant(c)` (cheap)
//
// What a query reads besides its key and fetches: the process's interners, sema's constants and
// the target, bound by whoever owns them (`Universe`, as pp's `session()`); handles in values are
// numbers in those interners, so a cell's value is meaningful only with them (#17 persists them).
//
// α subset (#39): the target is AArch64 Darwin; functions and variables of the global namespace;
// constants are integers, null pointers and addresses of string literal objects.
export module ycxx.pipeline:back;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.ir;
import ycxx.ir.l2;
import ycxx.ir.lower;
import ycxx.irgen;
import ycxx.obj;
import ycxx.sema;
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
// A constructor or destructor is named by its kind, not an identifier (Itanium §5.1.4.3).
enum class Special : std::uint8_t { none, constructor, destructor };
struct Entity {
  EntityId id = EntityId::none;
  std::string name;  // its identifier
  QualType type{};
  bool is_function = false;
  ast::LangLinkage lang = ast::LangLinkage::cxx;
  ast::Linkage linkage = ast::Linkage::external;
  bool is_constexpr = false;
  std::optional<ast::Loc> at;  // its first declarator, for diagnostics
  EntityId scope = EntityId::none;  // the class of a member
  Special special = Special::none;  // a constructor or destructor
  // A function template specialization: its template's function type and its template arguments,
  // which its name encodes (Itanium §5.1.5.7–8).
  std::optional<QualType> template_type{};
  std::vector<ast::TemplateArg> template_args{};
  std::vector<ast::TemplateParamSema> template_params{};  // its template's head
  // BOOTSTRAP: std::vector<bool>: GCC 16 writes a module unit exporting one that importers cannot read.
  std::vector<std::uint8_t> template_same_heads{};  // by argument: see `TemplateSpec`
  // A GNU asm label: the symbol exactly as written (docs/EXTENSIONS.md); empty if none.
  std::string asm_label{};
};

// A function definition: what `function_ir(F)` reads (irgen::FunctionInput, without a thunk's
// locals).
struct FunctionDef {
  EntityId entity = EntityId::none;
  ast::DeclSema decl{};
  ast::BodySema body{};
  ast::ItemId item{};
  std::uint32_t body_offset = 0;  // the declaration part's length in tokens
  std::optional<QualType> this_type{};  // a non-static member function's
};

// A variable definition at namespace scope with its constant initializer, or `ConstId::none` for
// zero-initialization ([basic.start.static]).
struct Variable {
  EntityId entity = EntityId::none;
  QualType type{};
  ConstId init = ConstId::none;
  std::optional<ast::Loc> at;  // its declarator, for diagnostics
};

// The classes and enumerations a unit defines, as `layout` reads them: a class's non-static data
// members' types in declaration order, an enumeration's underlying type. Each unit's own, from
// its front end (#115): entities are program-wide, but one session may see different definitions
// of one (several programs, or an edit), and the records follow the unit's source.
struct ClassShape {
  EntityId entity = EntityId::none;
  std::vector<QualType> fields;
  std::vector<EntityId> bases;  // its direct base classes, in declaration order
  bool union_ = false;  // every member at offset 0 ([class.union]/2)
  bool pod = true;      // POD for layout (`ast::record_layout`)
  // Trivially copyable for calls ([class.temporary]/3): no non-trivial copy constructor or
  // destructor, and the copy not deleted, so the calling convention may pass it in registers.
  bool trivial_calls = true;
};
struct EnumShape {
  EntityId entity = EntityId::none;
  QualType underlying{};
};
struct Shapes {
  std::vector<ClassShape> classes;  // in order of definition
  std::vector<EnumShape> enums;
};

// A translation unit's definitions and declarations, in the order of their items.
struct TranslationUnit {
  std::string name;  // the object's name, for diagnostics
  std::vector<Entity> entities;
  std::vector<FunctionDef> functions;  // the definitions
  std::vector<Variable> variables;     // the definitions
  EntityId main = EntityId::none;      // the program's `main`, if this unit defines it
  bool front_failed = false;           // the front end reported errors: nothing to compile
};

// What failed, at a location in an item when there is one. Every error here is something the α
// subset does not support yet: valid code that the front end accepted.
struct BackError {
  std::optional<ast::Loc> at;
  std::string message;
};

// An L2 body as a value. Its origin map is a class, so the body is fingerprinted and serialized
// field by field here, the map node by node.
struct L2Body {
  l2::Function f;
};
void hash_into(Hasher& h, const L2Body& b) {
  const l2::Function& f = b.f;
  hash_append(h, f.blocks);
  hash_append(h, f.nodes);
  hash_append(h, f.values);
  hash_append(h, f.operands);
  hash_append(h, f.imms);
  hash_append(h, f.succs);
  hash_append(h, f.results);
  h.uint(f.origins.size());
  for (std::uint32_t n = 0; n < f.origins.size(); ++n) hash_append(h, f.origins.of(ir::NodeId(n)));
}
void serialize_into(engine::Sink& out, const L2Body& b) {
  const l2::Function& f = b.f;
  engine::serialize(out, f.blocks);
  engine::serialize(out, f.nodes);
  engine::serialize(out, f.values);
  engine::serialize(out, f.operands);
  engine::serialize(out, f.imms);
  engine::serialize(out, f.succs);
  engine::serialize(out, f.results);
  std::vector<std::vector<ir::Origin>> origins;
  for (std::uint32_t n = 0; n < f.origins.size(); ++n) origins.emplace_back(std::from_range, f.origins.of(ir::NodeId(n)));
  engine::serialize(out, origins);
}
L2Body deserialize_from(engine::Source& in, std::type_identity<L2Body>) {
  L2Body b;
  l2::Function& f = b.f;
  f.blocks = engine::deserialize<decltype(f.blocks)>(in);
  f.nodes = engine::deserialize<decltype(f.nodes)>(in);
  f.values = engine::deserialize<decltype(f.values)>(in);
  f.operands = engine::deserialize<decltype(f.operands)>(in);
  f.imms = engine::deserialize<decltype(f.imms)>(in);
  f.succs = engine::deserialize<decltype(f.succs)>(in);
  f.results = engine::deserialize<decltype(f.results)>(in);
  f.origins = ir::OriginMap(engine::deserialize<std::vector<std::vector<ir::Origin>>>(in));
  return b;
}

// L2 bodies of optimization units, by fingerprint: the interned large values of optimization.md
// §2.4, so that `optimized` is keyed by what it compiles, not by where it came from.
struct OptUnit {
  L2Body body;
  std::vector<ir::EntityId> local;  // the entities it refers to that this TU defines, sorted by handle
  // The inline candidates and callee summaries of §2.4 come with level 1.
};
class UnitStore {
 public:
  Fp128 add(OptUnit u) {
    Fp128 fp = fingerprint(u);
    std::scoped_lock lock(m_);
    units_.try_emplace(fp, std::move(u));
    return fp;
  }
  const OptUnit& at(Fp128 fp) const {
    std::scoped_lock lock(m_);
    return units_.at(fp);
  }

 private:
  mutable std::mutex m_;
  std::map<Fp128, OptUnit> units_;  // node-based: references stay valid as units are added
};

// What the queries read besides their keys: the interners, constants and target of the process,
// and sema's world when units come from source. Bound for the duration of a `Bind`; the owner
// (`Session`, or `compile`) keeps them alive as long as its engine.
struct Universe {
  ast::Interners* in = nullptr;
  Constants* consts = nullptr;
  const ast::Target* target = &ast::aarch64_darwin;
  sema::World* world = nullptr;
  UnitStore* units = nullptr;
};
Universe& universe();
// Binds `u` and its interners on this thread until it is bound again: a pool worker's (#137).
void bind_thread(Universe* u);
// Binds a universe on this thread for the queries it runs, and its interners for the
// fingerprints of the handles they store (#68).
class Bind {
 public:
  explicit Bind(Universe& u);
  ~Bind();
  Bind(const Bind&) = delete;

 private:
  Universe* old_;
  std::optional<ast::BindInterners> interners_;
};

// ---------------------------------------------------------------------------------------------
// Inputs.

// A translation unit, numbered by the driver in command-line order.
struct TuId {
  std::uint32_t n = 0;
  bool operator==(const TuId&) const = default;
};

// Where a unit comes from: a source file (the driver's `pp::FileText` number), or definitions
// given as they are (tests, and units built without a front end).
struct TuSource {
  std::string name;                   // the object's name
  std::optional<std::uint32_t> file;  // a `pp::FileId`
  TranslationUnit given;              // when there is no file
};
struct [[=engine::input{engine::Durability::low}]] TuSourceIn {
  using Key = TuId;
  using Value = TuSource;
};

// The cost model of a target (optimization design §8): a stub until #9 measures one; level 0
// reads none of it, but `optimized` depends on it as later levels will.
struct CostModel {
  std::string name = "generic";
  std::uint32_t schema = 1;
};
struct [[=engine::input{engine::Durability::high}]] CostModelIn {
  using Key = obj::target;
  using Value = CostModel;
};

// The optimization-cache snapshot S (§A8.3): which units the cache holds at which level. Set by
// the daemon at quiescence; empty until the anytime layer exists.
struct SnapshotEntry {
  Fp128 unit;
  std::uint8_t level = 0;
};
struct [[=engine::input{engine::Durability::high}]] OptCacheSnapshot {
  using Key = obj::target;
  using Value = std::vector<SnapshotEntry>;
};

// The requested level B (`-O0`: 0, the only one so far).
struct [[=engine::input{engine::Durability::high}]] OptLevel {
  using Key = obj::target;
  using Value = std::uint8_t;
};

// ---------------------------------------------------------------------------------------------
// `layout` (AST design Part D) for `lower`: sizes, alignments, subobjects and constant values on
// the target. A string literal object's address is an entity of its own, named by the object's
// fingerprint.

// A class's layout: its size and alignment, and its non-static data members' offsets and types.
struct ClassLayout {
  std::uint64_t size = 0;
  std::uint32_t align = 1;
  std::vector<ir::SubObject> fields;
  std::vector<ir::SubObject> bases;  // the direct bases' subobjects, in declaration order
  ast::Placed placed;                // what it says of the class as a component
};

struct ConstLayout {
  std::int64_t bits = 0;
  std::optional<EntityId> base;    // an address constant
  std::optional<ConstId> string;   // the string literal object `base` is the entity of
};
struct [[=engine::cheap]] LayoutConstant {
  using Key = ConstId;
  using Value = ConstLayout;
  static Value run(engine::SyncCtx& cx, ConstId c);
};

// A string literal object a function or variable uses, and its entity.
struct StringUse {
  ConstId object = ConstId::none;
  EntityId entity = EntityId::none;
};

// `layout` as `lower` reads it (ir::layout_source): types from the interners and the unit's
// shapes (its classes' members and enumerations' underlying types, #115), constants through the
// cell above. Records the string literal objects whose addresses it gave, in order of first use.
class QueryLayout {
 public:
  QueryLayout(engine::SyncCtx& cx, const Shapes& shapes) : cx_(&cx) {
    for (const ClassShape& c : shapes.classes) classes_.emplace(std::to_underlying(c.entity), &c);
    for (const EnumShape& e : shapes.enums) enums_.emplace(std::to_underlying(e.entity), e.underlying);
  }
  ir::TypeLayout type(QualType q) const;
  ir::SubObject step(QualType q, ir::PathStep s) const;
  ir::ConstValue constant(ConstId c) const {
    ConstLayout v = cx_->fetch<LayoutConstant>(c);
    if (!v.base) return v.bits;
    if (v.string && !std::ranges::contains(strings_, *v.string, &StringUse::object)) strings_.push_back({*v.string, *v.base});
    return {*v.base, v.bits};
  }
  std::span<const StringUse> strings() const { return strings_; }

 private:
  std::optional<ClassLayout> class_layout(QualType c) const;
  ast::Placed placed(QualType t) const;

  engine::SyncCtx* cx_;
  std::map<std::uint32_t, const ClassShape*> classes_;
  std::map<std::uint32_t, QualType> enums_;
  mutable std::vector<StringUse> strings_;
  // Each class's layout and type layout, once: a member's are needed by every class holding it.
  mutable std::map<std::uint32_t, std::optional<ClassLayout>> class_layouts_;
  mutable std::map<std::uint32_t, ir::TypeLayout> records_;
};

// ---------------------------------------------------------------------------------------------
// The cells of one function.

struct FnKey {
  TuId tu;
  EntityId fn = EntityId::none;
  bool operator==(const FnKey&) const = default;
};

// Names in engine traces (#245): a unit by its source file, a function by its qualified name
// (from the bound universe's sema records, else its handle).
void trace_describe(std::string& out, TuId tu);
void trace_describe(std::string& out, const FnKey& k);

// The unit's definitions.
struct TuUnit {
  using Key = TuId;
  using Value = TranslationUnit;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
};
// The unit's classes and enumerations alone: an edit elsewhere in the unit stops here.
struct UnitShapes {
  using Key = TuId;
  using Value = Shapes;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
};

// A function declared with an ellipsis ([dcl.fct]) and how many parameters precede it.
struct VariadicFunction {
  EntityId entity = EntityId::none;
  std::uint32_t parameters = 0;
};

// F's definition and the facts `function_ir` asks of the rest of the unit.
struct FunctionSource {
  FunctionDef def;
  bool is_main = false;
  std::vector<EntityId> constexpr_functions;        // the unit's, sorted by handle
  std::vector<VariadicFunction> variadic_functions;  // the unit's, sorted by handle
};
struct FunctionSourceQ {
  using Key = FnKey;
  using Value = FunctionSource;
  static engine::query_task<Value> run(engine::Ctx& cx, FnKey k);
};

// `function_ir` itself, both halves; read through the two projections below.
struct Generated {
  std::optional<irgen::FunctionIr> ir;
  std::optional<BackError> error;
};
struct FunctionGen {
  using Key = FnKey;
  using Value = Generated;
  static engine::query_task<Value> run(engine::Ctx& cx, FnKey k);
  static std::string errors(const Value& v) { return v.error ? v.error->message : std::string(); }  // for traces
};

// `function_ir(F)`: the L1 body, which holds no position (optimization design §2.4).
struct FunctionIrValue {
  std::optional<ir::Function> body;
  std::optional<BackError> error;
};
struct FunctionIr {
  using Key = FnKey;
  using Value = FunctionIrValue;
  static engine::query_task<Value> run(engine::Ctx& cx, FnKey k);
  static std::string show(const Value& v);  // the L1 text, for traces (#245)
  static std::string errors(const Value& v) { return v.error ? v.error->message : std::string(); }
};

// `function_debug(F)`: node id → `Loc`, and the variables.
struct FunctionDebug {
  using Key = FnKey;
  using Value = ir::FunctionDebug;
  static engine::query_task<Value> run(engine::Ctx& cx, FnKey k);
};

struct Lowered {
  std::optional<L2Body> body;
  std::vector<StringUse> strings;  // in order of first use
  std::optional<BackError> error;
};
struct Lower {
  using Key = FnKey;
  using Value = Lowered;
  static engine::query_task<Value> run(engine::Ctx& cx, FnKey k);
  static std::string show(const Value& v);  // the L2 text, for traces (#245)
  static std::string errors(const Value& v) { return v.error ? v.error->message : std::string(); }
};

// `unit(F, ctx, inline_opts)`: the fingerprint of F's optimization unit, which is in the
// universe's `UnitStore`.
struct UnitRef {
  std::optional<Fp128> unit;
  std::optional<BackError> error;  // F did not lower
};
struct UnitQ {
  using Key = FnKey;
  using Value = UnitRef;
  static engine::query_task<Value> run(engine::Ctx& cx, FnKey k);
  static std::string errors(const Value& v) { return v.error ? v.error->message : std::string(); }  // for traces
};

struct OptKey {
  Fp128 unit;
  obj::target target = obj::target::aarch64_macos;
  std::uint8_t level = 0;
  bool operator==(const OptKey&) const = default;
};
struct Machine {
  std::optional<aarch64::MachineCode> code;
  std::string error;
};
// `optimized(unit, target, options, optimizer_version, level)` (§A8.1). Options and the optimizer
// version have nothing to choose yet: the version is the binary's, and a daemon serves one binary.
struct Optimized {
  using Key = OptKey;
  using Value = Machine;
  static engine::query_task<Value> run(engine::Ctx& cx, OptKey k);
  static std::string show(const Value& v);  // the instruction words, for traces (#245)
  static std::string errors(const Value& v) { return v.code ? std::string() : v.error; }
};

// `codegen(F)`: `best_available(unit, B, S)` (§A8.3; optimization design §7: S is consulted only
// for B ≥ 1, so -O0 is level 0 exactly).
struct Codegen {
  using Key = FnKey;
  using Value = Machine;
  static engine::query_task<Value> run(engine::Ctx& cx, FnKey k);
  static std::string show(const Value& v) { return Optimized::show(v); }
  static std::string errors(const Value& v) { return Optimized::errors(v); }
};

// ---------------------------------------------------------------------------------------------
// `tu(TU)`.

// The first request of each demanded cell, for "required from here" chains (§A5.2, §B6.2).
// Nothing in the back half is demanded yet; the table fills with contextual queries (#16).
struct FirstDemand {
  Fp128 cell;
  engine::Loc at;
};
struct TuValue {
  std::optional<obj::object> object;  // none when there are errors
  std::vector<BackError> errors;
  std::vector<FirstDemand> first_demands;
};
struct Tu {
  using Key = TuId;
  using Value = TuValue;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
  static std::string errors(const Value& v) {  // for traces (#245)
    std::string out;
    for (const BackError& e : v.errors) out += e.message + "\n";
    return out;
  }
};

// The objects of one translation unit, or what the α subset cannot compile yet: `tu(TU)` in an
// engine of its own, over the given interners.
std::expected<obj::object, std::vector<BackError>> compile(const TranslationUnit& tu, ast::Interners& in,
                                                           Constants& consts,
                                                           const ast::Target& target = ast::aarch64_darwin);

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

Universe*& bound() {
  static thread_local Universe* u = nullptr;
  return u;
}
Universe& universe() {
  contract_assert(bound() != nullptr);  // queries run inside a `Bind`
  return *bound();
}
void bind_thread(Universe* u) {
  bound() = u;
  ast::BindInterners::on_this_thread(u ? u->in : nullptr);
}
Bind::Bind(Universe& u) : old_(std::exchange(bound(), &u)) {
  if (u.in) interners_.emplace(*u.in);
}
Bind::~Bind() { bound() = old_; }

// ------------------------------------------------------------------------------------- layout

ir::TypeLayout QueryLayout::type(QualType q) const {
  using enum ir::TypeKind;
  ast::Interners& in = *universe().in;
  const ast::Target& target = *universe().target;
  QualType c = in.types.canon(q);
  if (auto b = ast::builtin_of(in, c)) {
    if (*b == ast::Builtin::void_) return {void_, 0, 1, false, {}};
    ast::BuiltinLayout l = target[*b];
    if (*b == ast::Builtin::bool_) return {boolean, l.size, l.align, false, {}};
    // std::nullptr_t's object holds a null pointer ([basic.fundamental]/16).
    if (*b == ast::Builtin::nullptr_t) return {pointer, l.size, l.align, false, ast::builtin(in, ast::Builtin::void_)};
    if (ast::is_integral(*b)) return {integer, l.size, l.align, ast::is_signed(target, *b), {}};
    return {other, l.size, l.align, false, {}};
  }
  if (in.types.is<ast::TPointer>(c))
    return {pointer, target.pointer_size, target.pointer_align, false, in.canonical(ast::pointee_of(in, c))};
  if (in.types.is<ast::TArray>(c)) {
    auto a = in.types.get<ast::TArray>(c);
    ir::TypeLayout e = type(a.elem);
    std::uint64_t n = a.bound == ConstId::none ? 0 : in.values.get<ast::VInt>(a.bound).bits.lo;
    return {array, e.size * n, e.align, false, {}};
  }
  if (in.types.is<ast::TFunction>(c)) return {function, 0, 1, false, {}};
  // A reference member is held as the address of its object ([dcl.ref]).
  if (in.types.is<ast::TLRef>(c) || in.types.is<ast::TRRef>(c))
    return {pointer, target.pointer_size, target.pointer_align, false,
            in.canonical(in.types.is<ast::TLRef>(c) ? in.types.get<ast::TLRef>(c).referee : in.types.get<ast::TRRef>(c).referee)};
  if (in.types.is<ast::TClass>(c))
    if (auto done = records_.find(std::to_underlying(in.types.get<ast::TClass>(c).entity)); done != records_.end()) return done->second;
  if (auto r = class_layout(c)) {
    auto it = classes_.find(std::to_underlying(in.types.get<ast::TClass>(c).entity));
    ir::TypeLayout l{record, r->size, r->align, false, {}, it == classes_.end() || it->second->trivial_calls};
    l.empty = r->fields.empty();
    // A floating member makes the class a candidate for the vector registers (AAPCS64 HFAs).
    for (const ir::SubObject& f : r->fields) {
      QualType ft = f.type;
      while (in.types.is<ast::TArray>(in.types.canon(ft))) ft = in.types.get<ast::TArray>(in.types.canon(ft)).elem;
      auto b = ast::builtin_of(in, ft);
      l.floating = l.floating || (b && ast::is_floating(*b)) || type(ft).floating;
    }
    records_[std::to_underlying(in.types.get<ast::TClass>(c).entity)] = l;
    return l;
  }
  // An enumeration is laid out as its underlying type ([dcl.enum]/8).
  if (in.types.is<ast::TEnum>(c))
    if (auto it = enums_.find(std::to_underlying(in.types.get<ast::TEnum>(c).entity)); it != enums_.end()) return type(it->second);
  return {};
}

// A class's layout (`ast::record_layout`, the rule sema's `sizeof` reads too). Its members are the
// unit's shape of it, when units come from source; none otherwise.
std::optional<ClassLayout> QueryLayout::class_layout(QualType c) const {
  ast::Interners& in = *universe().in;
  if (!in.types.is<ast::TClass>(c)) return std::nullopt;
  auto key = std::to_underlying(in.types.get<ast::TClass>(c).entity);
  if (auto done = class_layouts_.find(key); done != class_layouts_.end()) return done->second;
  auto it = classes_.find(key);
  if (it == classes_.end()) return std::nullopt;
  std::vector<ast::Placed> bases, places;
  for (EntityId b : it->second->bases) bases.push_back(placed(in.intern_type(ast::TClass{b})));
  for (QualType f : it->second->fields) places.push_back(placed(in.canonical(f)));
  ast::RecordLayout r = ast::record_layout(bases, places, it->second->union_, it->second->pod, key);
  ClassLayout out{r.size, std::uint32_t(r.align), {}, {}, r.placed};
  for (std::size_t i = 0; i < r.offsets.size(); ++i) out.fields.push_back({r.offsets[i], in.canonical(it->second->fields[i])});
  for (std::size_t i = 0; i < r.base_offsets.size(); ++i) out.bases.push_back({r.base_offsets[i], in.intern_type(ast::TClass{it->second->bases[i]})});
  class_layouts_[key] = out;
  return out;
}

// What a type's layout says of it as a component of a class.
ast::Placed QueryLayout::placed(QualType t) const {
  ast::Interners& in = *universe().in;
  QualType c = in.types.canon(t);
  if (in.types.is<ast::TArray>(c)) {
    auto a = in.types.get<ast::TArray>(c);
    std::uint64_t n = a.bound == ConstId::none ? 0 : in.values.get<ast::VInt>(a.bound).bits.lo;
    return ast::array_placed(placed(a.elem), n);
  }
  if (auto r = class_layout(c)) return r->placed;
  ir::TypeLayout m = type(c);
  return ast::scalar_placed(m.size, m.align);
}

ir::SubObject QueryLayout::step(QualType q, ir::PathStep s) const {
  ast::Interners& in = *universe().in;
  QualType c = in.types.canon(q);
  if (s.step == ir::Step::elem && in.types.is<ast::TArray>(c)) {
    QualType e = in.canonical(in.types.get<ast::TArray>(c).elem);
    return {s.index * type(e).size, e};
  }
  if (s.step == ir::Step::field)
    if (auto r = class_layout(c); r && s.index < r->fields.size()) return r->fields[s.index];
  // A direct base's subobject, by the base class's entity (direct bases are distinct).
  if (s.step == ir::Step::base)
    if (auto r = class_layout(c))
      for (const ir::SubObject& b : r->bases)
        if (std::to_underlying(in.types.get<ast::TClass>(in.types.canon(b.type)).entity) == s.index) return b;
  return {};
}

ConstLayout LayoutConstant::run(engine::SyncCtx&, ConstId c) {
  ast::Interners& in = *universe().in;
  if (auto a = universe().consts->address(c)) {
    EntityId e = in.entities.add<EntityId>(ast::mix(ast::seed("ycxx.pipeline.string"), in.values.fp(a->object)));
    return {a->offset, e, a->object};
  }
  if (in.values.is<ast::VInt>(c)) return {std::int64_t(in.values.get<ast::VInt>(c).bits.lo), {}, {}};
  return {};  // a null pointer, or not a constant `lower` reads: `lower` reports what it cannot use
}

static_assert(ir::layout_source<QueryLayout>);

// ------------------------------------------------------------------------------------- functions

// Every function's source re-runs after any edit to the unit and scans it: quadratic in the
// unit's functions per edit, cheap at α sizes (#121 indexes it).
namespace {
// An entity's qualified name from the bound universe's sema records, else its handle.
std::string entity_name(EntityId e) {
  std::string out;
  const sema::World* w = universe().world;
  const auto name = [&](this const auto& self, EntityId x, int depth) -> void {
    const sema::EntityRec* r = w ? w->entities.find(x) : nullptr;
    if (!r) {
      std::format_to(std::back_inserter(out), "entity {}", std::to_underlying(x));
      return;
    }
    if (r->scope != EntityId::none && depth < 8) {
      self(r->scope, depth + 1);
      out += "::";
    }
    out += w->ins.names.spelling(r->name);
    // A specialization: its template arguments, so `Describe<int>` and `Describe<int*>` differ.
    if (r->templ != EntityId::none) {
      const auto arg = [&](this const auto& one, const ast::TemplateArg& a) -> std::string {
        switch (a.kind) {
          case ast::TemplateArgKind::type: return w->types.spell(a.type);
          case ast::TemplateArgKind::value: {
            const sema::ValueRec v = w->values.rec(a.value);
            if (v.kind == sema::ValueKind::integer) return std::to_string(static_cast<std::int64_t>(v.bits));
            if (v.kind == sema::ValueKind::null_pointer) return "nullptr";
            return "…";
          }
          case ast::TemplateArgKind::pack: {
            std::string p;
            for (const ast::TemplateArg& x : w->ins.seqs.elements(a.elems)) p += (p.empty() ? "" : ", ") + one(x);
            return p;
          }
          case ast::TemplateArgKind::template_:
            return a.templ != EntityId::none ? std::string(w->ins.names.spelling(w->entities.rec(a.templ).name)) : w->types.spell(a.type);
        }
        return "?";
      };
      out += '<';
      bool first = true;
      for (const ast::TemplateArg& a : r->args) {
        const std::string text = arg(a);
        if (text.empty()) continue;  // an empty pack
        out += (first ? "" : ", ") + text;
        first = false;
      }
      out += '>';
    }
  };
  name(e, 0);
  return out;
}
}  // namespace

void trace_describe(std::string& out, const FnKey& k) { out += entity_name(k.fn); }


engine::query_task<FunctionSource> FunctionSourceQ::run(engine::Ctx& cx, FnKey k) {
  const TranslationUnit& tu = co_await cx.fetch<TuUnit>(k.tu);
  FunctionSource out;
  for (const FunctionDef& f : tu.functions)
    if (f.entity == k.fn) out.def = f;
  out.is_main = k.fn == tu.main;
  irgen::TypeFacts types(*universe().in, *universe().target);
  for (const Entity& e : tu.entities) {
    if (!e.is_function) continue;
    if (e.is_constexpr) out.constexpr_functions.push_back(e.id);
    if (auto n = types.parameters_before_ellipsis(e.type)) out.variadic_functions.push_back({e.id, *n});
  }
  std::ranges::sort(out.constexpr_functions);
  std::ranges::sort(out.variadic_functions, {}, &VariadicFunction::entity);
  co_return out;
}

// `Facts` for `function_ir`: types and integer constants from the interners, string literals from
// `Constants`, declarations from the function's source.
class UnitFacts : public irgen::TypeFacts {
 public:
  UnitFacts(const FunctionSource& src, const Shapes& shapes) : TypeFacts(*universe().in, *universe().target), src_(&src) {
    enum_underlying = [&shapes](EntityId e) {
      for (const EnumShape& s : shapes.enums)
        if (s.entity == e) return s.underlying;
      return ast::builtin(*universe().in, ast::Builtin::int_);
    };
    bases_of = [&shapes](EntityId c) {
      for (const ClassShape& s : shapes.classes)
        if (s.entity == c) return s.bases;
      return std::vector<EntityId>{};
    };
  }
  ConstId string_address(ConstId c) { return universe().consts->address_of(c); }
  bool is_constexpr(EntityId e) { return std::ranges::binary_search(src_->constexpr_functions, e); }
  bool is_main(EntityId e) { return e != EntityId::none && e == src_->def.entity && src_->is_main; }
  std::optional<std::uint32_t> ellipsis_after(EntityId e) {
    auto it = std::ranges::lower_bound(src_->variadic_functions, e, {}, &VariadicFunction::entity);
    if (it == src_->variadic_functions.end() || it->entity != e) return std::nullopt;
    return it->parameters;
  }

 private:
  const FunctionSource* src_;
};
static_assert(irgen::Facts<UnitFacts>);

engine::query_task<Generated> FunctionGen::run(engine::Ctx& cx, FnKey k) {
  const FunctionSource& src = co_await cx.fetch<FunctionSourceQ>(k);
  const Shapes& shapes = co_await cx.fetch<UnitShapes>(k.tu);
  UnitFacts facts(src, shapes);
  const FunctionDef& d = src.def;
  irgen::FunctionInput input{d.entity, d.decl, d.body, d.item, d.body_offset};
  input.this_type = d.this_type;
  auto f = irgen::function_ir(*universe().in, input, facts);
  if (!f) co_return Generated{std::nullopt, BackError{ast::Loc{d.item, d.body_offset}, f.error()}};
  co_return Generated{std::move(*f), std::nullopt};
}

namespace {
// A body's text without its `func @N` line's handle, which means nothing to a reader.
std::string unnamed(std::string text) {
  if (const std::size_t brace = text.find(" {"); text.starts_with("func @") && brace != std::string::npos) text.replace(0, brace, "func");
  return text;
}
}  // namespace

std::string FunctionIr::show(const FunctionIrValue& v) {
  if (v.error) return "error: " + v.error->message;
  return v.body ? unnamed(ir::print(EntityId::none, *v.body)) : std::string();
}

std::string Lower::show(const Lowered& v) {
  if (v.error) return "error: " + v.error->message;
  return v.body ? unnamed(l2::print(EntityId::none, v.body->f)) : std::string();
}

std::string Optimized::show(const Machine& m) {
  if (!m.code) return m.error.empty() ? std::string() : "error: " + m.error;
  return std::format("{} bytes, {} relocations\n", m.code->bytes.size(), m.code->relocs.size()) +
         aarch64::disassemble(*m.code, [](EntityId e) { return entity_name(e); });
}

engine::query_task<FunctionIrValue> FunctionIr::run(engine::Ctx& cx, FnKey k) {
  const Generated& g = co_await cx.fetch<FunctionGen>(k);
  if (!g.ir) co_return FunctionIrValue{std::nullopt, g.error};
  co_return FunctionIrValue{g.ir->body, std::nullopt};
}

engine::query_task<ir::FunctionDebug> FunctionDebug::run(engine::Ctx& cx, FnKey k) {
  const Generated& g = co_await cx.fetch<FunctionGen>(k);
  co_return g.ir ? g.ir->debug : ir::FunctionDebug{};
}

engine::query_task<Lowered> Lower::run(engine::Ctx& cx, FnKey k) {
  const FunctionIrValue& f = co_await cx.fetch<FunctionIr>(k);
  if (!f.body) co_return Lowered{std::nullopt, {}, f.error};
  QueryLayout layout(cx, co_await cx.fetch<UnitShapes>(k.tu));
  auto l2 = ir::lower(*f.body, k.fn, layout);
  if (!l2) {
    // Only an error needs positions.
    const ir::FunctionDebug& debug = co_await cx.fetch<FunctionDebug>(k);
    std::optional<ast::Loc> at;
    if (ir::raw(l2.error().node) < debug.loc.size()) at = debug.loc[ir::raw(l2.error().node)];
    co_return Lowered{std::nullopt, {}, BackError{at, "lower: " + l2.error().message}};
  }
  co_return Lowered{L2Body{std::move(*l2)}, {layout.strings().begin(), layout.strings().end()}, std::nullopt};
}

// The entities a function's code refers to (`symbol` and `call` operands).
std::vector<ir::EntityId> references(const l2::Function& body) {
  std::vector<ir::EntityId> out;
  for (std::uint32_t n = 0; n < body.nodes.size(); ++n) {
    l2::Op op = body.nodes[n].op;
    if (op == l2::Op::symbol || op == l2::Op::call) out.push_back(ir::EntityId(std::uint32_t(body.imms_of(ir::NodeId(n))[0])));
  }
  return out;
}

// The entities a unit defines: its functions and variables.
struct TuDefined {
  using Key = TuId;
  using Value = std::vector<EntityId>;  // sorted by handle
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu) {
    const TranslationUnit& u = co_await cx.fetch<TuUnit>(tu);
    std::vector<EntityId> out;
    for (const FunctionDef& f : u.functions) out.push_back(f.entity);
    for (const Variable& v : u.variables) out.push_back(v.entity);
    std::ranges::sort(out);
    co_return out;
  }
};

engine::query_task<UnitRef> UnitQ::run(engine::Ctx& cx, FnKey k) {
  auto [lowered, defined] = co_await cx.par_fetch(engine::q<Lower>{k}, engine::q<TuDefined>{k.tu});
  if (!lowered.body) co_return UnitRef{std::nullopt, lowered.error};
  // Which of its references the object defines: the unit's definitions and the string literal
  // objects the function itself uses (the object defines every one its functions use).
  OptUnit u{*lowered.body, {}};
  for (ir::EntityId e : references(u.body.f))
    if (std::ranges::binary_search(defined, e) || std::ranges::contains(lowered.strings, e, &StringUse::entity))
      u.local.push_back(e);
  std::ranges::sort(u.local, {}, [](ir::EntityId e) { return ir::raw(e); });
  u.local.erase(std::ranges::unique(u.local).begin(), u.local.end());
  co_return UnitRef{universe().units->add(std::move(u)), std::nullopt};
}

engine::query_task<Machine> Optimized::run(engine::Ctx& cx, OptKey k) {
  (void)cx.fetch<CostModelIn>(k.target);  // part of `target` (§O8); level 0 reads no cost
  const OptUnit& u = universe().units->at(k.unit);
  // Level 0: `lower` already ran (it made the unit); tree selection and stack slots (§O7).
  auto m = aarch64::compile_function(u.body.f, u.local);
  if (!m) co_return Machine{std::nullopt, m.error()};
  co_return Machine{std::move(*m), {}};
}

engine::query_task<Machine> Codegen::run(engine::Ctx& cx, FnKey k) {
  constexpr obj::target target = obj::target::aarch64_macos;
  const UnitRef& unit = co_await cx.fetch<UnitQ>(k);
  if (!unit.unit) co_return Machine{std::nullopt, unit.error ? unit.error->message : "no unit"};
  // best_available(unit, B, S): the highest level S holds, never one below B.
  std::uint8_t level = cx.fetch<OptLevel>(target);
  if (level >= 1) {
    for (const SnapshotEntry& e : cx.fetch<OptCacheSnapshot>(target))
      if (e.unit == *unit.unit) level = std::max(level, e.level);
  }
  co_return co_await cx.fetch<Optimized>({*unit.unit, target, level});
}

// ------------------------------------------------------------------------------------- tu(TU)

// A variable's initial bytes, little-endian, with the pointers in them.
struct Bytes {
  std::vector<std::uint8_t> bytes;
  std::vector<aarch64::DataReloc> relocs;
};

// Writes the value `init` of an object of `type` at `offset` in `out`.
std::expected<void, std::string> place_value(const ast::Interners& in, const QueryLayout& layout, QualType type,
                                             ConstId init, std::uint64_t offset, Bytes& out) {
  ir::TypeLayout l = layout.type(type);
  if (l.kind == ir::TypeKind::pointer) {
    ir::ConstValue v = layout.constant(init);
    if (v.base) out.relocs.push_back({std::uint32_t(offset), *v.base, v.bits});
    else if (v.bits != 0) return std::unexpected("a pointer initialized with an integer other than zero");
    return {};
  }
  if (l.kind == ir::TypeKind::integer || l.kind == ir::TypeKind::boolean) {
    if (!in.values.is<ast::VInt>(init)) return std::unexpected("an integer variable's initializer is not an integer");
    std::uint64_t v = in.values.get<ast::VInt>(init).bits.lo;
    for (std::uint64_t i = 0; i < l.size; ++i) out.bytes[offset + i] = std::uint8_t(v >> (8 * i));
    return {};
  }
  if ((l.kind == ir::TypeKind::array || l.kind == ir::TypeKind::record) && in.values.is<ast::VAggregate>(init)) {
    // [dcl.init.aggr]: each element at its offset.
    auto elems = in.seqs.elements(in.values.get<ast::VAggregate>(init).elems);
    ir::Step step = l.kind == ir::TypeKind::array ? ir::Step::elem : ir::Step::field;
    for (std::uint32_t i = 0; i < elems.size(); ++i) {
      ir::SubObject s = layout.step(type, {step, i});
      if (auto r = place_value(in, layout, s.type, elems[i], offset + s.offset, out); !r) return r;
    }
    return {};
  }
  return std::unexpected("a variable of this type");
}

std::expected<Bytes, std::string> initial_bytes(const ast::Interners& in, const QueryLayout& layout, QualType type,
                                                ConstId init) {
  Bytes out;
  out.bytes.assign(layout.type(type).size, 0);
  if (init == ConstId::none) return out;
  if (auto r = place_value(in, layout, type, init, 0, out); !r) return std::unexpected(r.error());
  return out;
}

// Functions, variables, string literals and symbols in definition order (§A5.3): functions and
// variables as the unit lists them, string literal objects by first use in that order, and
// undefined symbols by first reference.
engine::query_task<TuValue> Tu::run(engine::Ctx& cx, TuId id) {
  const TranslationUnit& tu = co_await cx.fetch<TuUnit>(id);
  ast::Interners& in = *universe().in;
  TuValue out;
  std::vector<BackError>& errors = out.errors;
  if (tu.front_failed) {
    // Its diagnostics are the front end's (`TuFront`); this only keeps an object out of a link.
    errors.push_back({std::nullopt, "the translation unit has errors"});
    co_return out;
  }
  std::vector<FnKey> fns;
  for (const FunctionDef& f : tu.functions) fns.push_back({id, f.entity});

  // Functions: function_ir, then lower. An error points at the construct when it has a node.
  std::vector<StringUse> strings;
  auto use = [&](std::span<const StringUse> s) {
    for (const StringUse& u : s)
      if (!std::ranges::contains(strings, u.object, &StringUse::object)) strings.push_back(u);
  };
  std::vector<FnKey> lowered;
  {
    auto ls = co_await cx.par_map<Lower>(fns);
    for (std::size_t i = 0; i < fns.size(); ++i) {
      const Lowered& l = ls[i];
      if (l.error) errors.push_back(*l.error);
      if (!l.body) continue;
      use(l.strings);
      lowered.push_back(fns[i]);
    }
  }

  // Variables: their initial bytes, by what they hold ([basic.start.static]).
  std::vector<aarch64::DataDef> data;
  const Shapes& shapes = co_await cx.fetch<UnitShapes>(id);
  for (const Variable& v : tu.variables) {
    QueryLayout layout(cx, shapes);
    auto b = initial_bytes(in, layout, v.type, v.init);
    use(layout.strings());
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
    data.push_back(std::move(d));
  }

  // String literal objects, as `lower` and the variables met them ([lex.string]).
  std::vector<aarch64::Symbol> symbols;
  std::uint32_t n = 0;
  for (auto [object, entity] : strings) {
    auto units = universe().consts->string_object(object);
    if (!units) {
      errors.push_back({std::nullopt, "an address constant whose object is not a string literal"});
      continue;
    }
    data.push_back({entity, aarch64::DataKind::cstring, std::move(*units), 0, 1, {}});
    symbols.push_back({entity, std::format("l_.str.{}", n++), obj::binding::local});
  }

  // Symbols for the entities the object defines or refers to; a declaration nothing uses needs
  // no name.
  std::set<std::uint32_t> used;
  {
    const auto ls = co_await cx.par_map<Lower>(lowered);  // all hits
    for (const Lowered& l : ls) {
      for (ir::EntityId e : references(l.body->f)) used.insert(ir::raw(e));
    }
  }
  for (const FnKey& f : lowered) used.insert(ir::raw(f.fn));
  for (const aarch64::DataDef& d : data) {
    used.insert(ir::raw(d.entity));
    for (const aarch64::DataReloc& r : d.relocs) used.insert(ir::raw(r.target));
  }
  for (const Entity& e : tu.entities) {
    if (!used.contains(ir::raw(e.id))) continue;
    // The complete-object constructor and destructor (C1, D1): with no virtual bases they are the
    // base-object ones too (#136).
    std::string name_ = e.special == Special::constructor ? "C1" : e.special == Special::destructor ? "D1" : e.name;
    TemplateSpec spec{e.template_type.value_or(e.type), e.template_args, e.template_params, e.template_same_heads};
    auto name = symbol_name(
        in, name_, e.type, e.is_function, e.lang,
        [](EntityId c) {
          const sema::World* w = universe().world;
          if (!w) return ClassName{};
          const sema::EntityRec& r = w->entities.rec(c);
          return ClassName{std::string(w->ins.names.spelling(r.name)), r.scope, r.templ, r.args,
                           r.kind == sema::EntityKind::class_ || r.kind == sema::EntityKind::class_template};
        },
        e.scope, e.special != Special::none, e.template_type ? &spec : nullptr);
    if (!name) {
      errors.push_back({e.at, std::format("the name of '{}': {} is not supported yet", e.name, name.error())});
      continue;
    }
    if (!e.asm_label.empty()) name = e.asm_label;
    symbols.push_back({e.id, std::move(*name), e.linkage == ast::Linkage::internal ? obj::binding::local : obj::binding::global});
  }
  if (!errors.empty()) co_return out;

  // Code: codegen(F) for every function, in definition order.
  std::vector<aarch64::CompiledFunction> code;
  {
    auto ms = co_await cx.par_map<Codegen>(fns);
    for (std::size_t i = 0; i < fns.size(); ++i) {
      const Machine& m = ms[i];
      if (!m.code) {
        errors.push_back({ast::Loc{tu.functions[i].item, tu.functions[i].body_offset}, "codegen: " + m.error});
        co_return out;
      }
      code.push_back({fns[i].fn, *m.code});
    }
  }
  auto o = aarch64::assemble(tu.name, code, data, symbols);
  if (!o) {
    std::optional<ast::Loc> at;
    for (const FunctionDef& fn : tu.functions)
      if (fn.entity == o.error().entity) at = ast::Loc{fn.item, fn.body_offset};
    errors.push_back({at, "codegen: " + o.error().message});
    co_return out;
  }
  out.object = std::move(*o);
  co_return out;
}

std::expected<obj::object, std::vector<BackError>> compile(const TranslationUnit& tu, ast::Interners& in,
                                                           Constants& consts, const ast::Target& target) {
  UnitStore units;
  Universe u{&in, &consts, &target, nullptr, &units};
  Bind bind(u);
  engine::Db db;
  db.set<CostModelIn>(obj::target::aarch64_macos, {});
  db.set<OptCacheSnapshot>(obj::target::aarch64_macos, {});
  db.set<OptLevel>(obj::target::aarch64_macos, 0);
  db.set<TuSourceIn>(TuId{0}, {tu.name, std::nullopt, tu});
  const TuValue& v = db.get<Tu>(TuId{0});
  if (!v.object) return std::unexpected(v.errors);
  return *v.object;
}

}  // namespace ycxx::pipeline
