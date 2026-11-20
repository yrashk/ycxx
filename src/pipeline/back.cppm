// The back half of a translation unit's pipeline (#43, #63) as engine queries: every function
// definition through `function_ir` (ycxx.irgen), `lower` (ycxx.ir.lower), its optimization unit,
// `optimized` at level 0 and `codegen`, then `tu(TU)`, whose value is the object
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
// the default target for pre-built units, bound by whoever owns them (`Universe`); handles in values are
// numbers in those interners, so a cell's value is meaningful only with them (#17 persists them).
//
// The Linux ABI subset admits scalar integer and pointer types in function signatures.
export module ycxx.pipeline:back;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.ir;
import ycxx.ir.l2;
import ycxx.ir.lower;
import ycxx.irgen;
import ycxx.items;
import ycxx.obj;
import ycxx.pp;
import ycxx.sema;
import ycxx.target.aarch64;
import ycxx.target.x86_64;
import :lines;
import :mangle;

export namespace ycxx::pipeline {

namespace ast = ycxx::ast;
namespace ir = ycxx::ir;

using ast::ConstId;
using ast::EntityId;
using ast::QualType;

// What the back end asks of constants beyond integers and null pointers (ast.md §5.5): string
// literal objects and address constants. The front end answers from its value interner; tests
// answer from tables.
class Constants {
 public:
  virtual ~Constants() = default;
  // The element type and code units of a string literal object, including the terminating null.
  struct StringObject {
    ast::Builtin element;
    std::vector<std::uint32_t> units;
  };
  virtual std::optional<StringObject> string_object(ConstId c) const = 0;
  // An address constant: its base (`VString` or `VEntity`) and byte offset.
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
  // A static local ([stmt.dcl]/3), or with `guard` its guard variable: the function it is local
  // to, whose <encoding> its <local-name> begins with, and its number among that function's
  // statics of its name (Itanium C++ ABI §5.1.6, §3.3.3).
  EntityId local_function = EntityId::none;
  std::uint32_t discriminator = 0;
  bool guard = false;
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
  // Its static locals' guard variables and the runtime functions their initialization calls
  // ([stmt.dcl]/3; irgen::FunctionInput).
  std::vector<irgen::FunctionInput::StaticGuard> guards{};
  EntityId guard_acquire = EntityId::none, guard_release = EntityId::none, atexit = EntityId::none, dso = EntityId::none;
  bool weak_elf = false;  // an ordinary external inline definition in the GNU ELF subset (#422)
};

// A variable definition at namespace scope with its constant initializer, or `ConstId::none` for
// zero-initialization ([basic.start.static]).
struct Variable {
  EntityId entity = EntityId::none;
  QualType type{};
  ConstId init = ConstId::none;
  std::optional<ast::Loc> at;  // its declarator, for diagnostics
  bool dynamic = false;  // also initialized by code ([basic.start.dynamic]): writable, if const
  bool weak_elf = false;  // eligible external inline namespace definition (#432)
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
  std::vector<std::uint8_t> bit_widths{};  // parallel to fields, empty for ordinary records
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
  // Its initialization functions ([basic.start.dynamic]), in order, each with the entity of its
  // startup array slot.
  std::vector<std::pair<EntityId, EntityId>> init_functions;
  std::optional<ast::Loc> namespace_destruction;  // first variable whose destructor runs at exit
  bool front_failed = false;           // the front end reported errors: nothing to compile
};

// What failed, at a location in an item when there is one. Every error here is something the α
// subset does not support yet: valid code that the front end accepted.
struct BackError {
  std::optional<ast::Loc> at;
  std::string message;
  std::optional<SourcePosition> source{};

  std::string detail() const {
    if (source && !source->where.empty()) return std::format("{}: {}", source->where, message);
    return message;
  }
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
  std::vector<ir::EntityId> local;  // directly addressable references, sorted by handle
  std::vector<ir::EntityId> got_symbols;  // addresses using the ELF GOT, sorted by handle
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

// The interners, constants, default target for given units, and sema worlds for source units.
// The owner (`Session` or `compile`) keeps them alive as long as its engine. Each worker's `Bind`
// makes them available to the queries it runs.
struct Universe {
  ast::Interners* in = nullptr;
  Constants* consts = nullptr;
  const ast::Target* target = &ast::aarch64_darwin;
  sema::World* world = nullptr;
  UnitStore* units = nullptr;
  sema::World* linux_world = nullptr;

  sema::World* world_for(const ast::Target& t) const {
    return t.triple == ast::x86_64_linux.triple ? linux_world : world;
  }
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
  QueryLayout(engine::SyncCtx& cx, const Shapes& shapes, const ast::Target& target) : cx_(&cx), target_(&target) {
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
  const ast::Target* target_;
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
  static std::string errors(const Value& v) { return v.error ? v.error->detail() : std::string(); }  // for traces
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
  static std::string errors(const Value& v) { return v.error ? v.error->detail() : std::string(); }
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
  static std::string errors(const Value& v) { return v.error ? v.error->detail() : std::string(); }
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
  static std::string errors(const Value& v) { return v.error ? v.error->detail() : std::string(); }  // for traces
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
  std::optional<x86_64::MachineCode> linux_code{};
  bool ok() const { return code.has_value() || linux_code.has_value(); }
};
// `optimized(unit, target, options, optimizer_version, level)` (§A8.1). Options and the optimizer
// version have nothing to choose yet: the version is the binary's, and a daemon serves one binary.
struct Optimized {
  using Key = OptKey;
  using Value = Machine;
  static engine::query_task<Value> run(engine::Ctx& cx, OptKey k);
  static std::string show(const Value& v);  // the instruction words, for traces (#245)
  static std::string errors(const Value& v) { return v.ok() ? std::string() : v.error; }
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
    for (const BackError& e : v.errors) out += e.detail() + "\n";
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

namespace {

// A source unit uses the target in its configuration; a given unit uses its owner's target.
const ast::Target& unit_target(engine::SyncCtx& cx, TuId tu) {
  const TuSource& src = cx.fetch<TuSourceIn>(tu);
  if (src.file) return *cx.fetch<pp::SourceOf>(pp::FileId{*src.file}).config.target;
  return *universe().target;
}

obj::target object_target(const ast::Target& target) {
  return target.triple == ast::x86_64_linux.triple ? obj::target::x86_64_linux : obj::target::aarch64_macos;
}

// Source positions are dependencies only of queries that report an error.
engine::query_task<std::optional<SourceLocations>> error_locations(engine::Ctx& cx, TuId tu) {
  const TuSource& src = cx.fetch<TuSourceIn>(tu);
  if (!src.file) co_return std::nullopt;
  const pp::FileId file{*src.file};
  const pp::Source& source = cx.fetch<pp::SourceOf>(file);
  const pp::Region& region = co_await cx.fetch<pp::Pp>(file);
  const items::Items& items = co_await cx.fetch<items::RegionItems>(file);
  co_return SourceLocations(cx, source, file, region, items);
}

engine::query_task<BackError> located_error(engine::Ctx& cx, TuId tu, std::optional<ast::Loc> at, std::string message) {
  BackError error{at, std::move(message)};
  if (at)
    if (auto locations = co_await error_locations(cx, tu)) error.source = locations->at(*universe().in, *at);
  co_return error;
}

// A failed object build may have more than one error. Use one table per file for those that
// arose here, and keep the positions already resolved by the cells that reported the others.
engine::query_task<TuValue> locate_errors(engine::Ctx& cx, TuId tu, TuValue out) {
  const auto needs_position = [](const BackError& e) { return e.at && !e.source; };
  if (std::ranges::any_of(out.errors, needs_position))
    if (auto locations = co_await error_locations(cx, tu))
      for (BackError& e : out.errors)
        if (needs_position(e)) e.source = locations->at(*universe().in, *e.at);
  co_return out;
}

}  // namespace

ir::TypeLayout QueryLayout::type(QualType q) const {
  using enum ir::TypeKind;
  ast::Interners& in = *universe().in;
  const ast::Target& target = *target_;
  QualType c = in.types.canon(q);
  if (auto b = ast::builtin_of(in, c)) {
    if (*b == ast::Builtin::void_) return {void_, 0, 1, false, {}};
    ast::BuiltinLayout l = target[*b];
    if (*b == ast::Builtin::bool_) return {boolean, l.size, l.align, false, {}};
    // std::nullptr_t's object holds a null pointer ([basic.fundamental]/16).
    if (*b == ast::Builtin::nullptr_t) return {pointer, l.size, l.align, false, ast::builtin(in, ast::Builtin::void_)};
    if (ast::is_integral(*b)) return {integer, l.size, l.align, ast::is_signed(target, *b), {}};
    if (ast::is_floating(*b) && ast::computes(l.format)) return {floating, l.size, l.align, false, {}};
    return {other, l.size, l.align, false, {}};
  }
  if (in.types.is<ast::TPointer>(c))
    return {pointer, target.pointer_size, target.pointer_align, false, in.canonical(ast::pointee_of(in, c))};
  if (in.types.is<ast::TMemberPtr>(c)) {
    ast::BuiltinLayout l = ast::member_pointer_layout(target, ast::is_function(in, in.types.get<ast::TMemberPtr>(c).member));
    return {.kind = member_pointer, .size = l.size, .align = l.align, .contains_member_pointer = true};
  }
  if (in.types.is<ast::TArray>(c)) {
    auto a = in.types.get<ast::TArray>(c);
    ir::TypeLayout e = type(a.elem);
    std::uint64_t n = a.bound == ConstId::none ? 0 : in.values.get<ast::VInt>(a.bound).bits.lo;
    return {.kind = array, .size = e.size * n, .align = e.align, .contains_member_pointer = e.contains_member_pointer};
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
    l.empty = r->placed.empty;  // no data members, in it or its bases
    l.data = r->placed.nvsize;
    // A homogeneous floating-point aggregate (AAPCS64 §5.9.5): every member, through arrays and
    // member classes, of one floating type, 1 to 4 of them; a union's members overlap, so it has
    // as many as its largest.
    bool union_ = it != classes_.end() && it->second->union_;
    std::uint32_t count = 0;
    std::uint8_t bytes = 0;
    // Its non-empty bases' members count as its own (AAPCS64 §5.9.5, as Clang has it).
    std::vector<ir::SubObject> parts;
    for (const ir::SubObject& b : r->bases)
      if (!type(b.type).empty) parts.push_back(b);
    parts.insert(parts.end(), r->fields.begin(), r->fields.end());
    bool hfa = !parts.empty();
    for (const ir::SubObject& f : parts) {
      QualType ft = f.type;
      std::uint64_t n = 1;
      while (in.types.is<ast::TArray>(in.types.canon(ft))) {
        auto a = in.types.get<ast::TArray>(in.types.canon(ft));
        n *= a.bound == ConstId::none ? 0 : in.values.get<ast::VInt>(a.bound).bits.lo;
        ft = a.elem;
      }
      ir::TypeLayout m = type(ft);
      l.contains_member_pointer |= m.contains_member_pointer;
      std::uint8_t mb = m.kind == floating ? std::uint8_t(m.size) : m.hfa_bytes;
      std::uint64_t mn = (m.kind == floating ? 1 : m.hfa) * n;
      if (!mb || mn == 0 || (bytes && mb != bytes)) hfa = false;
      bytes = mb;
      count = union_ ? std::max<std::uint32_t>(count, std::uint32_t(std::min<std::uint64_t>(mn, 5)))
                     : std::uint32_t(std::min<std::uint64_t>(count + mn, 5));
    }
    if (hfa && count <= 4 && count * bytes == r->size) {
      l.hfa = std::uint8_t(count);
      l.hfa_bytes = bytes;
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
  for (std::size_t i = 0; i < it->second->fields.size(); ++i) {
    ast::Placed f = placed(in.canonical(it->second->fields[i]));
    if (!it->second->bit_widths.empty()) f.bit_width = it->second->bit_widths[i];
    places.push_back(std::move(f));
  }
  ast::RecordLayout r = ast::record_layout(bases, places, it->second->union_, it->second->pod, key);
  ClassLayout out{r.size, std::uint32_t(r.align), {}, {}, r.placed};
  for (std::size_t i = 0; i < r.offsets.size(); ++i)
    out.fields.push_back({r.offsets[i], in.canonical(it->second->fields[i]), r.bit_fields[i]});
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
    if (in.values.is<ast::VEntity>(a->object))
      return {a->offset, in.values.get<ast::VEntity>(a->object).entity, {}};
    EntityId e = in.entities.add<EntityId>(ast::mix(ast::seed("ycxx.pipeline.string"), in.values.fp(a->object)));
    return {a->offset, e, a->object};
  }
  if (in.values.is<ast::VInt>(c)) return {std::int64_t(in.values.get<ast::VInt>(c).bits.lo), {}, {}};
  if (in.values.is<ast::VFloat>(c)) return {std::int64_t(in.values.get<ast::VFloat>(c).bits.lo), {}, {}};
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
  if (universe().linux_world && (!w || !w->entities.find(e))) w = universe().linux_world;
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
  irgen::TypeFacts types(*universe().in, unit_target(cx, k.tu));
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
  UnitFacts(const FunctionSource& src, const Shapes& shapes, const ast::Target& target)
      : TypeFacts(*universe().in, target), src_(&src) {
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
    bit_field_width = [&shapes](EntityId c, std::uint32_t field) -> std::uint8_t {
      for (const ClassShape& shape : shapes.classes)
        if (shape.entity == c && field < shape.bit_widths.size()) return shape.bit_widths[field];
      return 0;
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
  UnitFacts facts(src, shapes, unit_target(cx, k.tu));
  const FunctionDef& d = src.def;
  irgen::FunctionInput input{d.entity, d.decl, d.body, d.item, d.body_offset};
  input.this_type = d.this_type;
  input.guards = d.guards;
  input.guard_acquire = d.guard_acquire;
  input.guard_release = d.guard_release;
  input.atexit = d.atexit;
  input.dso = d.dso;
  auto f = irgen::function_ir(*universe().in, input, facts);
  if (!f) {
    auto error = co_await located_error(cx, k.tu, ast::Loc{d.item, d.body_offset}, std::move(f.error()));
    co_return Generated{std::nullopt, std::move(error)};
  }
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
  if (v.error) return "error: " + v.error->detail();
  return v.body ? unnamed(ir::print(EntityId::none, *v.body)) : std::string();
}

std::string Lower::show(const Lowered& v) {
  if (v.error) return "error: " + v.error->detail();
  return v.body ? unnamed(l2::print(EntityId::none, v.body->f)) : std::string();
}

std::string Optimized::show(const Machine& m) {
  if (m.linux_code)
    return std::format("{} bytes, {} relocations\n", m.linux_code->bytes.size(), m.linux_code->relocs.size());
  if (m.code)
    return std::format("{} bytes, {} relocations\n", m.code->bytes.size(), m.code->relocs.size()) +
           aarch64::disassemble(*m.code, [](EntityId e) { return entity_name(e); });
  return m.error.empty() ? std::string() : "error: " + m.error;
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

namespace {

bool linux_ellipsis_scalar(ir::TypeLayout l) {
  return l.kind == ir::TypeKind::pointer || (l.kind == ir::TypeKind::integer && (l.size == 4 || l.size == 8));
}

bool linux_fixed_scalar(ir::TypeLayout l) {
  return l.kind == ir::TypeKind::boolean || linux_ellipsis_scalar(l) ||
         (l.kind == ir::TypeKind::integer && (l.size == 1 || l.size == 2));
}

// Lowering implements the Darwin aggregate calling convention. Check the original signature of
// each function and call before lowering can erase a class or ellipsis.
std::string_view linux_signature_error(const ast::Interners& in, const QueryLayout& layout, QualType type,
                                       bool allow_variadic = false) {
  QualType canonical = in.types.canon(type);
  if (!in.types.is<ast::TFunction>(canonical)) return "the function type is unavailable";
  auto f = in.types.get<ast::TFunction>(canonical);
  if (f.variadic && !allow_variadic) return "variadic functions are not supported";
  ir::TypeLayout result = layout.type(f.ret);
  if (result.kind != ir::TypeKind::void_ && !linux_fixed_scalar(result))
    return "only void, bool, 8-, 16-, 32- and 64-bit integer, pointer and reference results are supported";
  for (QualType p : in.seqs.elements(f.params))
    if (!linux_fixed_scalar(layout.type(p)))
      return "only bool, 8-, 16-, 32- and 64-bit integer, pointer and reference parameters are supported";
  return {};
}

std::string_view linux_direct_call_error(const QueryLayout& layout, const ir::Function& body, ir::NodeId call) {
  auto imms = body.imms_of(call);
  // The L1 by-value bit identifies a class argument represented by a pointer.
  if (imms[2]) return "aggregate arguments are not supported";
  // The tail values have already undergone the default argument promotions ([expr.call]).
  for (ir::ValueId v : body.operands_of(call).last(imms[1]))
    if (!linux_ellipsis_scalar(layout.type(body.value(v).type.type)))
      return "only 32- and 64-bit integer and pointer ellipsis arguments are supported";
  return {};
}

}  // namespace

engine::query_task<Lowered> Lower::run(engine::Ctx& cx, FnKey k) {
  const FunctionIrValue& f = co_await cx.fetch<FunctionIr>(k);
  const ast::Target& target = unit_target(cx, k.tu);
  const bool linux = object_target(target) == obj::target::x86_64_linux;
  if (!f.body && !linux) co_return Lowered{std::nullopt, {}, f.error};
  QueryLayout layout(cx, co_await cx.fetch<UnitShapes>(k.tu), target);
  ir::LowerConfig config;
  if (linux) {
    const ast::Interners& in = *universe().in;
    const FunctionDef& def = (co_await cx.fetch<FunctionSourceQ>(k)).def;
    QualType type = def.body.deduced ? def.body.deduced_type : def.decl.type;
    if (auto error = linux_signature_error(in, layout, type); !error.empty()) {
      auto message = std::format("x86-64 System V ABI: {}", error);
      auto located = co_await located_error(cx, k.tu, ast::Loc{def.item, def.body_offset}, std::move(message));
      co_return Lowered{std::nullopt, {}, std::move(located)};
    }
    if (!f.body) co_return Lowered{std::nullopt, {}, f.error};
    const TranslationUnit& tu = co_await cx.fetch<TuUnit>(k.tu);
    const ir::Function& body = *f.body;
    for (std::size_t i = 0; i < body.nodes.size(); ++i) {
      const auto n = ir::NodeId(static_cast<std::uint32_t>(i));
      const ir::Op op = body.node(n).op;
      const bool direct = op == ir::Op::call;
      std::optional<QualType> callee;
      EntityId callee_id = EntityId::none;
      if (direct) {
        callee_id = EntityId(static_cast<std::uint32_t>(body.imms_of(n)[0]));
        auto e = std::ranges::find(tu.entities, callee_id, &Entity::id);
        if (e != tu.entities.end()) callee = e->type;
      } else if (op == ir::Op::call_ind) {
        QualType ptr = body.value(body.operands_of(n)[1]).type.type;
        callee = ast::pointee_of(in, ptr);
      } else continue;
      std::string_view error = callee ? linux_signature_error(in, layout, *callee, direct) : "the function type is unavailable";
      if (direct && callee && !error.empty()) {
        // The declaration can still have a placeholder return type. Read the signature already
        // deduced in this unit without depending on the callee's lowering ([dcl.spec.auto]).
        const auto definition = std::ranges::find(tu.functions, callee_id, &FunctionDef::entity);
        if (definition != tu.functions.end() && definition->body.deduced)
          error = linux_signature_error(in, layout, definition->body.deduced_type, true);
      }
      if (direct && error.empty()) error = linux_direct_call_error(layout, body, n);
      if (error.empty()) continue;
      const ir::FunctionDebug& debug = co_await cx.fetch<FunctionDebug>(k);
      auto at = i < debug.loc.size() ? debug.loc[i] : ast::Loc{def.item, def.body_offset};
      auto located = co_await located_error(cx, k.tu, at, std::format("x86-64 System V ABI: call: {}", error));
      co_return Lowered{std::nullopt, {}, std::move(located)};
    }
    config.int_arg_registers = 6;
    // Extend outgoing narrow results for interoperability. Lowering truncates incoming results
    // to the declared width, as the System V AMD64 psABI §3.2.3 leaves excess bits unspecified.
    config.min_result_bits = 32;
  }
  auto l2 = ir::lower(*f.body, k.fn, layout, config);
  if (!l2) {
    // Only an error needs positions.
    const ir::FunctionDebug& debug = co_await cx.fetch<FunctionDebug>(k);
    std::optional<ast::Loc> at;
    if (ir::raw(l2.error().node) < debug.loc.size()) at = debug.loc[ir::raw(l2.error().node)];
    auto error = co_await located_error(cx, k.tu, at, "lower: " + l2.error().message);
    co_return Lowered{std::nullopt, {}, std::move(error)};
  }
  co_return Lowered{L2Body{std::move(*l2)}, {layout.strings().begin(), layout.strings().end()}, std::nullopt};
}

// The entities a function's code refers to (`symbol` and `call` operands).
struct SymbolReference {
  ir::EntityId entity;
  l2::Op op;
};

std::vector<SymbolReference> references(const l2::Function& body) {
  std::vector<SymbolReference> out;
  for (std::uint32_t n = 0; n < body.nodes.size(); ++n) {
    l2::Op op = body.nodes[n].op;
    if (op == l2::Op::symbol || op == l2::Op::call)
      out.push_back({ir::EntityId(std::uint32_t(body.imms_of(ir::NodeId(n))[0])), op});
  }
  return out;
}

struct SymbolAddresses {
  std::vector<EntityId> direct, got;
};

// ELF default-visible symbols can be preempted, so their addresses use the GOT.
// Internal definitions can use the symbol itself in a direct displacement.
struct TuSymbolAddresses {
  using Key = TuId;
  using Value = SymbolAddresses;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu) {
    const TranslationUnit& u = co_await cx.fetch<TuUnit>(tu);
    const bool linux = object_target(unit_target(cx, tu)) == obj::target::x86_64_linux;
    Value out;
    auto add = [&](EntityId id) {
      if (linux) {
        auto e = std::ranges::find(u.entities, id, &Entity::id);
        if (e == u.entities.end() || e->linkage != ast::Linkage::internal) return;
      }
      out.direct.push_back(id);
    };
    for (const FunctionDef& f : u.functions) add(f.entity);
    for (const Variable& v : u.variables) add(v.entity);
    if (linux) {
      for (const Entity& e : u.entities)
        if (e.linkage == ast::Linkage::external || e.linkage == ast::Linkage::module)
          out.got.push_back(e.id);
      std::ranges::sort(out.got);
    }
    std::ranges::sort(out.direct);
    co_return out;
  }
};

engine::query_task<UnitRef> UnitQ::run(engine::Ctx& cx, FnKey k) {
  auto [lowered, symbols] = co_await cx.par_fetch(engine::q<Lower>{k}, engine::q<TuSymbolAddresses>{k.tu});
  if (!lowered.body) co_return UnitRef{std::nullopt, lowered.error};
  // Which references can be addressed directly, including the function's string literals.
  OptUnit u{*lowered.body, {}, {}};
  for (auto [e, op] : references(u.body.f)) {
    if (std::ranges::binary_search(symbols.direct, e) || std::ranges::contains(lowered.strings, e, &StringUse::entity))
      u.local.push_back(e);
    else if (op == l2::Op::symbol && std::ranges::binary_search(symbols.got, e))
      u.got_symbols.push_back(e);
  }
  for (auto* ids : {&u.local, &u.got_symbols}) {
    std::ranges::sort(*ids);
    ids->erase(std::ranges::unique(*ids).begin(), ids->end());
  }
  co_return UnitRef{universe().units->add(std::move(u)), std::nullopt};
}

engine::query_task<Machine> Optimized::run(engine::Ctx& cx, OptKey k) {
  (void)cx.fetch<CostModelIn>(k.target);  // part of `target` (§O8); level 0 reads no cost
  const OptUnit& u = universe().units->at(k.unit);
  // Level 0: `lower` already ran (it made the unit); selection and stack slots (§O7).
  if (k.target == obj::target::x86_64_linux) {
    auto m = x86_64::compile_function(u.body.f, u.local, u.got_symbols);
    if (!m) co_return Machine{std::nullopt, m.error()};
    co_return Machine{std::nullopt, {}, std::move(*m)};
  }
  auto m = aarch64::compile_function(u.body.f, u.local);
  if (!m) co_return Machine{std::nullopt, m.error()};
  co_return Machine{std::move(*m), {}};
}

engine::query_task<Machine> Codegen::run(engine::Ctx& cx, FnKey k) {
  const obj::target target = object_target(unit_target(cx, k.tu));
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
  if (l.kind == ir::TypeKind::floating) {  // its encoding
    if (!in.values.is<ast::VFloat>(init)) return std::unexpected("a floating variable's initializer is not floating");
    std::uint64_t v = in.values.get<ast::VFloat>(init).bits.lo;
    for (std::uint64_t i = 0; i < l.size; ++i) out.bytes[offset + i] = std::uint8_t(v >> (8 * i));
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
      if (s.bit_field) {
        if (!in.values.is<ast::VInt>(elems[i])) return std::unexpected("a bit-field initializer is not an integer");
        const auto bits = s.bit_field;
        auto value = std::uint32_t(in.values.get<ast::VInt>(elems[i]).bits.lo) << bits.shift;
        for (std::uint32_t byte = 0; byte < bits.bytes(); ++byte) {
          auto mask = std::uint8_t(bits.mask() >> (8 * byte));
          auto& dst = out.bytes[offset + s.offset + byte];
          dst = std::uint8_t((dst & ~mask) | ((value >> (8 * byte)) & mask));
        }
        continue;
      }
      if (auto r = place_value(in, layout, s.type, elems[i], offset + s.offset, out); !r) return r;
    }
    return {};
  }
  return std::unexpected("a variable of this type");
}

std::expected<Bytes, std::string> initial_bytes(const ast::Interners& in, const QueryLayout& layout, QualType type,
                                                ConstId init) {
  ir::TypeLayout l = layout.type(type);
  if (l.contains_member_pointer) return std::unexpected("static storage containing pointers to members");
  Bytes out;
  out.bytes.assign(l.size, 0);
  if (init == ConstId::none) return out;
  if (auto r = place_value(in, layout, type, init, 0, out); !r) return std::unexpected(r.error());
  return out;
}

namespace {

struct LinuxFunction {
  EntityId entity;
  const x86_64::MachineCode* code;
  bool weak;
};

obj::reloc_kind linux_relocation(x86_64::RelocKind kind) {
  switch (kind) {
    case x86_64::RelocKind::pc32: return obj::reloc_kind::x86_64_pc32;
    case x86_64::RelocKind::plt32: return obj::reloc_kind::x86_64_plt32;
    case x86_64::RelocKind::rex_gotpcrelx: return obj::reloc_kind::x86_64_rex_gotpcrelx;
  }
  std::unreachable();
}

std::expected<obj::object, aarch64::CodegenError> assemble_linux(
    const std::string& name, std::span<const LinuxFunction> functions,
    std::span<const aarch64::DataDef> data, std::span<const aarch64::Symbol> symbols,
    const std::set<EntityId>& weak_variables) {
  obj::object o;
  o.name = name;
  o.arch = obj::target::x86_64_linux;
  std::map<EntityId, std::uint32_t> symbol_of;
  for (const auto& s : symbols) symbol_of.emplace(s.entity, o.add_symbol({s.name, s.bind, {}, 0, false}));
  auto missing = [](EntityId e) {
    return std::unexpected(aarch64::CodegenError{e, std::format("@{} has no symbol name", ir::raw(e))});
  };
  if (!functions.empty()) {
    const std::uint32_t text = o.add_section(obj::standard_section::text);
    obj::section& sec = o.sections[text];
    for (const LinuxFunction& f : functions) {
      auto sym = symbol_of.find(f.entity);
      if (sym == symbol_of.end()) return missing(f.entity);
      sec.align(4);
      const std::size_t base = sec.bytes.size();
      auto& symbol = o.symbols[sym->second];
      symbol.section = text;
      symbol.offset = base;
      symbol.weak = f.weak && symbol.bind == obj::binding::global;
      symbol.function_size = f.code->bytes.size();
      sec.emit(f.code->bytes);
      for (const auto& r : f.code->relocs) {
        auto target = symbol_of.find(r.target);
        if (target == symbol_of.end()) return missing(r.target);
        if (base + r.offset > std::numeric_limits<std::uint32_t>::max())
          return std::unexpected(aarch64::CodegenError{f.entity, "a text relocation exceeds the object offset limit"});
        sec.relocate(linux_relocation(r.kind), target->second, r.addend, static_cast<std::uint32_t>(base + r.offset));
      }
    }
  }
  using K = aarch64::DataKind;
  using S = obj::standard_section;
  for (auto [kind, section] : {std::pair{K::cstring, S::cstring}, {K::rodata, S::rodata}, {K::data_const, S::data_const},
                               {K::data, S::data}, {K::bss, S::bss}, {K::init_func, S::init_array}}) {
    if (!std::ranges::contains(data, kind, &aarch64::DataDef::kind)) continue;
    const std::uint32_t index = o.add_section(section);
    obj::section& sec = o.sections[index];
    for (const auto& d : data) {
      if (d.kind != kind) continue;
      auto sym = symbol_of.find(d.entity);
      if (sym == symbol_of.end()) return missing(d.entity);
      sec.align(std::countr_zero(std::bit_ceil(d.align)));
      const std::uint64_t base = sec.size();
      auto& symbol = o.symbols[sym->second];
      symbol.section = index;
      symbol.offset = base;
      symbol.weak = symbol.bind == obj::binding::global && weak_variables.contains(d.entity);
      if (kind == K::bss) sec.reserve_zerofill(d.size);
      else sec.emit(d.bytes);
      for (const auto& r : d.relocs) {
        auto target = symbol_of.find(r.target);
        if (target == symbol_of.end()) return missing(r.target);
        if (base + r.offset > std::numeric_limits<std::uint32_t>::max())
          return std::unexpected(aarch64::CodegenError{d.entity, "a data relocation exceeds the object offset limit"});
        sec.relocate(obj::reloc_kind::abs64, target->second, r.addend, static_cast<std::uint32_t>(base + r.offset));
      }
    }
  }
  return o;
}

}  // namespace

// Functions, variables, string literals and symbols in definition order (§A5.3): functions and
// variables as the unit lists them, string literal objects by first use in that order, and
// undefined symbols by first reference.
engine::query_task<TuValue> Tu::run(engine::Ctx& cx, TuId id) {
  const TranslationUnit& tu = co_await cx.fetch<TuUnit>(id);
  ast::Interners& in = *universe().in;
  const ast::Target& target = unit_target(cx, id);
  const bool linux = object_target(target) == obj::target::x86_64_linux;
  TuValue out;
  std::vector<BackError>& errors = out.errors;
  if (tu.front_failed) {
    // Its diagnostics are the front end's (`TuFront`); this only keeps an object out of a link.
    errors.push_back({std::nullopt, "the translation unit has errors"});
    co_return out;
  }
  if (linux && tu.namespace_destruction) {
    errors.push_back({tu.namespace_destruction, "namespace-scope destruction is not supported on x86-64 Linux"});
    co_return co_await locate_errors(cx, id, std::move(out));
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
  std::set<EntityId> weak_variables;
  const Shapes& shapes = co_await cx.fetch<UnitShapes>(id);
  for (const Variable& v : tu.variables) {
    QueryLayout layout(cx, shapes, target);
    auto b = initial_bytes(in, layout, v.type, v.init);
    use(layout.strings());
    if (!b) {
      errors.push_back({v.at, std::format("{} is not supported yet", b.error())});
      continue;
    }
    bool is_const = (std::to_underlying(ast::cv_of(in.canonical(v.type))) & 1) != 0;
    bool zero = b->relocs.empty() && std::ranges::all_of(b->bytes, [](std::uint8_t x) { return x == 0; });
    aarch64::DataDef d{v.entity, aarch64::DataKind::data, std::move(b->bytes), 0, layout.type(v.type).align, std::move(b->relocs)};
    if (v.weak_elf && !v.dynamic && d.relocs.empty()) weak_variables.insert(v.entity);
    if (is_const && !v.dynamic) d.kind = d.relocs.empty() ? aarch64::DataKind::rodata : aarch64::DataKind::data_const;
    else if (zero) {
      d.kind = aarch64::DataKind::bss;
      d.size = d.bytes.size();
      d.bytes.clear();
    }
    data.push_back(std::move(d));
  }

  std::vector<aarch64::Symbol> symbols;
  // The pointers to the initialization functions that startup calls before `main`, in order.
  for (std::size_t i = 0; i < tu.init_functions.size(); ++i) {
    auto [fn, slot] = tu.init_functions[i];
    data.push_back({slot, aarch64::DataKind::init_func, std::vector<std::uint8_t>(8, 0), 0, 8, {{0, fn, 0}}});
    symbols.push_back({slot, std::format("l_.init_func.{}", i), obj::binding::local});
  }

  // String literal objects, as `lower` and the variables met them ([lex.string]).
  std::uint32_t n = 0;
  for (auto [object, entity] : strings) {
    auto str = universe().consts->string_object(object);
    if (!str) {
      errors.push_back({std::nullopt, "an address constant whose object is not a string literal"});
      continue;
    }
    const ast::BuiltinLayout layout = target[str->element];
    std::vector<std::uint8_t> bytes;
    bytes.reserve(str->units.size() * layout.size);
    for (std::uint32_t unit : str->units) {
      // Both targets store the least-significant byte first.
      for (std::uint8_t i = 0; i < layout.size; ++i, unit >>= 8)
        bytes.push_back(std::uint8_t(unit));
    }
    // The cstring section is reserved for strings with one-byte code units.
    auto kind = layout.size == 1 ? aarch64::DataKind::cstring : aarch64::DataKind::rodata;
    data.push_back({entity, kind, std::move(bytes), 0, layout.align, {}});
    symbols.push_back({entity, std::format("{}.str.{}", linux ? ".L" : "l_", n++), obj::binding::local});
  }

  // Symbols for the entities the object defines or refers to; a declaration nothing uses needs
  // no name.
  std::set<std::uint32_t> used;
  {
    const auto ls = co_await cx.par_map<Lower>(lowered);  // all hits
    for (const Lowered& l : ls) {
      for (const auto& ref : references(l.body->f)) used.insert(ir::raw(ref.entity));
    }
  }
  for (const FnKey& f : lowered) used.insert(ir::raw(f.fn));
  for (const aarch64::DataDef& d : data) {
    used.insert(ir::raw(d.entity));
    for (const aarch64::DataReloc& r : d.relocs) used.insert(ir::raw(r.target));
  }
  // The functions closure types are local to, by entity (docs/design/closures.md).
  std::map<std::uint32_t, const Entity*> by_id;
  for (const Entity& e : tu.entities) by_id.emplace(ir::raw(e.id), &e);
  auto spec_of = [](const Entity& e) {
    return TemplateSpec{e.template_type.value_or(e.type), e.template_args, e.template_params, e.template_same_heads};
  };
  ClassNames names = [&](EntityId c) {
    const sema::World* w = universe().world_for(target);
    if (!w) return ClassName{};
    const sema::EntityRec& r = w->entities.rec(c);
    ClassName n{std::string(w->ins.names.spelling(r.name)), r.scope, r.templ, r.args,
                r.kind == sema::EntityKind::class_ || r.kind == sema::EntityKind::class_template};
    n.unnamed_type_ordinal = r.unnamed_type_ordinal;
    if (r.kind == sema::EntityKind::class_ && n.name.empty() && r.scope != EntityId::none && !n.unnamed_type_ordinal) {
      auto f = by_id.find(ir::raw(r.scope));
      n.closure = true;
      n.closure_type = r.type;
      n.closure_number = r.ordinal;
      n.closure_head = r.closure_head;
      if (f != by_id.end()) {
        const Entity& fe = *f->second;
        std::string fname = fe.special == Special::constructor ? "C1" : fe.special == Special::destructor ? "D1" : fe.name;
        n.local = std::make_shared<const LocalFunction>(LocalFunction{
            fname, fe.type, fe.scope, fe.special != Special::none,
            fe.template_type ? std::optional(spec_of(fe)) : std::nullopt});
      }
    }
    return n;
  };
  std::vector<const Entity*> locals;  // named after their functions, below
  std::map<std::uint32_t, std::string> mangled;  // by function entity
  for (const Entity& e : tu.entities) {
    if (!used.contains(ir::raw(e.id))) continue;
    if (e.local_function != EntityId::none) {
      locals.push_back(&e);
      continue;
    }
    // The complete-object constructor and destructor (C1, D1): with no virtual bases they are the
    // base-object ones too (#136).
    std::string name_ = e.special == Special::constructor ? "C1" : e.special == Special::destructor ? "D1" : e.name;
    TemplateSpec spec{e.template_type.value_or(e.type), e.template_args, e.template_params, e.template_same_heads};
    auto name = symbol_name(in, name_, e.type, e.is_function, e.lang, names, e.scope, e.special != Special::none,
                            e.template_type ? &spec : nullptr, !linux);
    if (!name) {
      errors.push_back({e.at, std::format("the name of '{}': {} is not supported yet", e.name, name.error())});
      continue;
    }
    mangled.emplace(ir::raw(e.id), *name);  // before an asm label: what its statics' names begin with
    if (!e.asm_label.empty()) name = e.asm_label;
    symbols.push_back({e.id, std::move(*name), e.linkage == ast::Linkage::internal ? obj::binding::local : obj::binding::global});
  }
  // A static local and its guard: `_ZZ <encoding> E <name> [_<n>]` and `_ZGV` before it, the
  // function's own <encoding> taken from its symbol (Itanium C++ ABI §5.1.6, §5.1.4.6).
  const std::string_view object_prefix = linux ? "" : "_";
  for (const Entity* e : locals) {
    auto f = mangled.find(ir::raw(e->local_function));
    if (f == mangled.end()) {
      errors.push_back({e->at, std::format("the name of the static local '{}': its function has no symbol", e->name)});
      continue;
    }
    std::string_view function_symbol = f->second;
    function_symbol.remove_prefix(object_prefix.size());
    std::string encoding = function_symbol.starts_with("_Z") ? std::string(function_symbol.substr(2))
                                                              : std::format("{}{}", function_symbol.size(), function_symbol);
    std::uint32_t k = e->discriminator;
    std::string disc = k == 0 ? "" : k <= 10 ? std::format("_{}", k - 1) : std::format("__{}_", k - 1);
    symbols.push_back({e->id, std::format("{}{}{}E{}{}{}", object_prefix, e->guard ? "_ZGVZ" : "_ZZ",
                                         encoding, e->name.size(), e->name, disc),
                       obj::binding::local});
  }
  if (!errors.empty()) co_return co_await locate_errors(cx, id, std::move(out));

  // Code: codegen(F) for every function, in definition order.
  auto ms = co_await cx.par_map<Codegen>(fns);
  for (std::size_t i = 0; i < fns.size(); ++i) {
    if (ms[i].get().ok()) continue;
    errors.push_back({ast::Loc{tu.functions[i].item, tu.functions[i].body_offset}, "codegen: " + ms[i].get().error});
    co_return co_await locate_errors(cx, id, std::move(out));
  }
  auto o = [&] {
    if (linux) {
      std::vector<LinuxFunction> code;
      for (std::size_t i = 0; i < fns.size(); ++i)
        code.push_back({fns[i].fn, &*ms[i].get().linux_code, tu.functions[i].weak_elf});
      return assemble_linux(tu.name, code, data, symbols, weak_variables);
    }
    std::vector<aarch64::CompiledFunction> code;
    for (std::size_t i = 0; i < fns.size(); ++i) code.push_back({fns[i].fn, *ms[i].get().code});
    return aarch64::assemble(tu.name, code, data, symbols);
  }();
  if (!o) {
    std::optional<ast::Loc> at;
    for (const FunctionDef& fn : tu.functions)
      if (fn.entity == o.error().entity) at = ast::Loc{fn.item, fn.body_offset};
    errors.push_back({at, "codegen: " + o.error().message});
    co_return co_await locate_errors(cx, id, std::move(out));
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
  const obj::target arch = object_target(target);
  db.set<CostModelIn>(arch, {});
  db.set<OptCacheSnapshot>(arch, {});
  db.set<OptLevel>(arch, 0);
  db.set<TuSourceIn>(TuId{0}, {tu.name, std::nullopt, tu});
  const TuValue& v = db.get<Tu>(TuId{0});
  if (!v.object) return std::unexpected(v.errors);
  return *v.object;
}

}  // namespace ycxx::pipeline
