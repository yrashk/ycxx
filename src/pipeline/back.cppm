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
//   codegen(TU, F, mode)     best_available(unit, B, S) over the opt_cache_snapshot input S
//   tu(TU)                   the object, in definition order (§A5.3), and the first-demand table
//   layout                   `layout_type(T)`, `layout_step(T, s)`, `layout_constant(c)` (cheap)
//
// Per-unit cells give the per-function ones what they read of the whole unit, so that an edit
// re-runs them cheaply or not at all (#938): `UnitIndexQ` (where each definition and entity is),
// `UnitFunctionFactsQ` (the constexpr and variadic functions irgen asks about), `UnitShapeIndex`
// (`layout`'s lookup of classes and enumerations) and `SymbolAccessQ(TU, E)` (how the unit's
// code addresses one entity, read by `unit`).
//
// What a query reads besides its key and fetches: the process's interners, sema's constants and
// the default target for pre-built units, bound by whoever owns them (`Universe`); handles in values are
// numbers in those interners, so a cell's value is meaningful only with them (#17 persists them).
//
// The Linux ABI subset admits scalar integer and pointer types in function signatures.
export module ycxx.pipeline:back;

import std;
import ycxx.ast;
import ycxx.cm;
import ycxx.engine;
import ycxx.ir;
import ycxx.ir.l2;
import ycxx.ir.legalize;
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
// These symbols use special Itanium encodings (§5.1.4.3, §5.1.4.5).
enum class Special : std::uint8_t {
  none, constructor, destructor, virtual_table, deleting_destructor, type_info, base_object_destructor, template_parameter_object
};
struct Entity {
  EntityId id = EntityId::none;
  std::string name;  // its identifier
  QualType type{};
  bool is_function = false;
  ast::LangLinkage lang = ast::LangLinkage::cxx;
  ast::Linkage linkage = ast::Linkage::external;
  bool is_constexpr = false;
  std::optional<ast::Loc> at;  // its first declarator, for diagnostics
  EntityId scope = EntityId::none;  // the enclosing namespace or class
  Special special = Special::none;
  // A function template specialization: its template's function type and its template arguments,
  // which its name encodes (Itanium §5.1.5.7–8).
  std::optional<QualType> template_type{};
  std::vector<ast::TemplateArg> template_args{};
  std::vector<ast::TemplateParamSema> template_params{};  // its template's head
  // BOOTSTRAP: std::vector<bool>: GCC 16 writes a module unit exporting one that importers cannot read.
  std::vector<std::uint8_t> template_same_heads{};  // by argument: see `TemplateSpec`
  ast::WrittenConstraint template_head_constraint{}, template_trailing_constraint{};
  // A GNU asm label: the symbol exactly as written (docs/EXTENSIONS.md); empty if none.
  std::string asm_label{};
  std::optional<ast::Loc> asm_label_at{};
  // A static local ([stmt.dcl]/3), or with `guard` its guard variable: the function it is local
  // to, whose <encoding> its <local-name> begins with, and its number among that function's
  // statics of its name (Itanium C++ ABI §5.1.6, §3.3.3).
  EntityId local_function = EntityId::none;
  std::uint32_t discriminator = 0;
  bool guard = false;
  // The mangled parameter types tie this ordinary C++ function to the unit (#582).
  bool local_parameter_binding = false;
  bool hidden = false;  // GNU hidden visibility: ELF STV_HIDDEN, Mach-O private extern (#602, #124)
  bool nonthrowing = false;  // including implicit exception specifications on destructors
  bool weak_reference = false;  // explicit GNU weak binding for an undefined ELF object
  bool unsupported_class_parameter_signature = false;

  bool has_local_binding() const { return linkage == ast::Linkage::internal || local_parameter_binding; }
};

// Certified on the typed definition, before IR drops lifetime information.
enum class UnwindEligibility : std::uint8_t { none, cleanup_free, cleanup_free_with_throw, hosted_catch, hosted_cleanup };

struct HostedCatch {
  EntityId callee;  // checked against the complete unit
  std::optional<std::uint32_t> int_binding{};  // empty for a catch-all
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
  // An ordinary external inline definition that every unit using it may emit: a weak definition,
  // coalesced by the linker (ELF #422, Mach-O #124).
  bool coalesced = false;
  bool explicit_weak = false;  // an ordinary non-inline definition (#937)
  UnwindEligibility unwind = UnwindEligibility::none;
  std::optional<HostedCatch> hosted_catch{};
  ast::NodeId cleanup_call = ast::NodeId::none;
  EntityId constructor_cleanup = EntityId::none;
  EntityId constructor_initializer_callee = EntityId::none;
  EntityId unwind_through_constructor = EntityId::none;
  EntityId destructor_table = EntityId::none;  // the primary table installed by D1 and D2
  // The primary table a dynamic class's constructor installs after its base subobject (#309).
  EntityId constructor_table = EntityId::none;
  bool emit_if_referenced = false;  // an eligible immediate definition (#1097)
  // A named namespace-scope immediate function template instance without local statics or closures.
  bool plain_immediate_template = false;
  bool private_constexpr = false;  // a candidate for demand-driven inline emission (#1174)
  bool unsupported_class_parameter_abi = false;
};

// A variable definition at namespace scope with its constant initializer, or `ConstId::none` for
// zero-initialization ([basic.start.static]).
struct Variable {
  EntityId entity = EntityId::none;
  QualType type{};
  ConstId init = ConstId::none;
  std::optional<ast::Loc> at;  // its declarator, for diagnostics
  bool dynamic = false;  // also initialized by code ([basic.start.dynamic]): writable, if const
  bool coalesced = false;  // an eligible external inline data definition, weak (#432, #903)
  bool inline_definition = false;
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
  std::uint8_t explicit_alignment = 0;
  // Trivially copyable for calls ([class.temporary]/3): no non-trivial copy constructor or
  // destructor, and the copy not deleted, so the calling convention may pass it in registers.
  bool trivial_calls = true;
  bool gcc_stack_calls = false;  // GCC's System V MEMORY class for an implicit deletion
  std::vector<std::uint8_t> bit_widths{};  // parallel to fields, empty for ordinary records
  std::vector<std::uint32_t> overlapping_fields{};  // [[no_unique_address]] ([intro.object])
  bool layout_uncertain = false;  // source packing or alignment that native layout does not apply
  bool copy_abi_unknown = false;   // no ABI classification for this defaulted copy
  // `trivial_calls` false that the destructor and copy and move constructors, taken one by one,
  // do not confirm: an unknown one, or one explicitly defaulted and trivial that the call rule
  // still counts as user-declared (#641).
  bool calls_abi_uncertain = false;
  bool layout_requires_virtual_support = false;
  bool has_mutable_field = false;
  std::uint8_t clang_weak_alignment = 0;
  sema::PeerVirtualLayout peer_virtual_layout = sema::PeerVirtualLayout::none;
  EntityId peer_automatic_primary_base = EntityId::none;
  bool dynamic_root = false;  // a dynamic class whose own vptr is at offset zero (#309)
  std::vector<ast::NameId> field_names{};
  bool plain_ownership_record = false;
  EntityId ownership_destructor = EntityId::none;
  EntityId nonthrowing_destructor = EntityId::none;
  std::vector<std::uint8_t> field_alignments{};  // parallel to fields when a member strengthens alignment
};
struct EnumShape {
  EntityId entity = EntityId::none;
  QualType underlying{};
};
struct Shapes {
  std::vector<ClassShape> classes;  // in order of definition
  std::vector<EnumShape> enums;
};

// The table this unit owns, as established by its complete declaration set. This does
// not depend on the functions' bodies or whether this unit ever uses the table.
struct OwnedVirtualTable {
  EntityId entity;
  std::vector<EntityId> slots;  // in slot order, starting with the key function in the admitted profiles
  EntityId primary_base = EntityId::none;
  bool peer_base_rtti = false;  // a peer's key owns the primary base descriptor
};

// A dynamic class's primary table and RTTI this unit defines (Itanium C++ ABI §2.5.2, §2.9.5,
// §5.2.3): strong in the unit defining the key function, else weak in every unit that defines the
// class. `slots` holds the function entries (D1 and D0 for a virtual destructor, the pure virtual
// handler for a pure one).
struct DynamicTable {
  EntityId entity;  // the virtual table
  EntityId cls;
  std::vector<EntityId> slots;
  EntityId base = EntityId::none;  // the single public primary base, whose RTTI the descriptor names
  bool weak = false;
  bool rtti_only = false;  // a keyless base's descriptor for a derived one, its table not used here
};

struct AsmHiddenSymbol {
  std::string name;
  ast::Loc at;
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
  bool virtual_emission_blocked = false;
  std::optional<ast::Loc> virtual_declaration;
  std::optional<ast::Loc> unsupported_typed_dynamic_cast;
  std::optional<ir::ThrowRuntime> throw_runtime;
  std::optional<ir::CatchAllRuntime> catch_runtime;
  EntityId cleanup_resume = EntityId::none;
  std::optional<OwnedVirtualTable> owned_virtual_table;
  std::vector<DynamicTable> dynamic_tables;  // in class definition order: a base before its derived classes
  std::set<EntityId> keyless_type_info;  // owned root descriptors for typed casts and exception objects
  // The written complete-object definitions whose ELF base-object entries share their code.
  std::set<EntityId> base_object_variants;
  std::vector<AsmHiddenSymbol> asm_hidden_symbols;
  bool has_raw_linkage_names = false;  // written asm labels or attributes with raw names
  // The declared heads of class templates whose own arguments may appear in symbols.
  std::map<EntityId, ast::SeqId<ast::TemplateParamSema>> class_template_parameters{};
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
  h.uint(f.indirect_result);  // where the class result's address arrives (x8, or RDI and back in RAX)
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
  engine::serialize(out, f.indirect_result);
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
  f.indirect_result = engine::deserialize<decltype(f.indirect_result)>(in);
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
  std::int64_t high = 0;           // a 128-bit integer's high half
  std::optional<EntityId> base;    // an address constant
  std::optional<ConstId> object;  // a string literal or compiler-created constant object
};
struct [[=engine::cheap]] LayoutConstant {
  struct Key {
    ConstId value;
    bool linux_target;
    bool operator==(const Key&) const = default;
  };
  using Value = ConstLayout;
  static Value run(engine::SyncCtx& cx, Key key);
};

// A constant object a function or variable uses, and its entity.
struct ConstantObjectUse {
  ConstId object = ConstId::none;
  EntityId entity = EntityId::none;
};

// A compiler runtime function (compiler-rt, libgcc) a function's code calls, and its entity.
struct RuntimeUse {
  ir::Runtime function{};
  EntityId entity = EntityId::none;
};

// The unit's shapes by entity handle (#938): each function's `layout` looks its classes up here,
// where it built a map of every class of the unit, which made lowering a unit quadratic.
struct ShapeIndex {
  std::vector<std::pair<EntityId, std::uint32_t>> classes;  // into `Shapes::classes`
  std::vector<std::pair<EntityId, std::uint32_t>> enums;    // into `Shapes::enums`
};
engine::SourceSpan tu_span(const engine::Db& db, TuId tu);  // defined with the function cells' spans
struct UnitShapeIndex {
  using Key = TuId;
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return tu_span(db, k); }
  using Value = ShapeIndex;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
};
// An entity's first class or enumeration record in the unit's shapes, or null.
inline const ClassShape* class_shape(const Shapes& shapes, const ShapeIndex& index, EntityId e) {
  auto it = std::ranges::lower_bound(index.classes, e, {}, &std::pair<EntityId, std::uint32_t>::first);
  return it != index.classes.end() && it->first == e ? &shapes.classes[it->second] : nullptr;
}
inline const EnumShape* enum_shape(const Shapes& shapes, const ShapeIndex& index, EntityId e) {
  auto it = std::ranges::lower_bound(index.enums, e, {}, &std::pair<EntityId, std::uint32_t>::first);
  return it != index.enums.end() && it->first == e ? &shapes.enums[it->second] : nullptr;
}

// `layout` as `lower` reads it (ir::layout_source): types from the interners and the unit's
// shapes (its classes' members and enumerations' underlying types, #115), constants through the
// cell above. Records constant objects whose addresses it gave, in order of first use.
class QueryLayout {
 public:
  QueryLayout(engine::SyncCtx& cx, const Shapes& shapes, const ShapeIndex& index, const ast::Target& target)
      : cx_(&cx), target_(&target), shapes_(&shapes), index_(&index) {}
  ir::TypeLayout type(QualType q) const;
  std::optional<ir::TypeLayout> peer_root_allocation(QualType q) const;
  ir::SubObject step(QualType q, ir::PathStep s) const;
  bool is_class(EntityId e) const { return shape_of(e) != nullptr; }
  bool contains_mutable_field(QualType type) const;
  ir::ConstValue constant(ConstId c) const {
    ConstLayout v = cx_->fetch<LayoutConstant>({c, target_->triple == ast::x86_64_linux.triple});
    if (!v.base) return {v.bits, v.high};
    if (v.object && !std::ranges::contains(objects_, *v.object, &ConstantObjectUse::object))
      objects_.push_back({*v.object, *v.base});
    return {*v.base, v.bits};
  }
  // An asm template is literal data, without a native string object's address.
  std::optional<ast::BreakpointInstruction> breakpoint_statement_asm(ConstId c) const {
    const ast::Interners& in = *universe().in;
    if (!in.values.is<ast::VString>(c)) return std::nullopt;
    const auto text = in.values.get<ast::VString>(c);
    const auto type = in.types.canon(text.type);
    if (!in.types.is<ast::TArray>(type) ||
        ast::builtin_of(in, in.types.get<ast::TArray>(type).elem) != ast::Builtin::char_) return std::nullopt;
    return target_->breakpoint_statement_asm(in.seqs.elements(text.units));
  }
  std::span<const ConstantObjectUse> objects() const { return objects_; }

 private:
  std::optional<ClassLayout> class_layout(QualType c) const;
  std::uint8_t sysv_integer_eightbytes(const ClassShape& shape, const ClassLayout& layout) const;
  ast::Placed placed(QualType t) const;

  const ClassShape* shape_of(EntityId e) const { return class_shape(*shapes_, *index_, e); }
  const EnumShape* enum_of(EntityId e) const { return enum_shape(*shapes_, *index_, e); }

  engine::SyncCtx* cx_;
  const ast::Target* target_;
  const Shapes* shapes_;
  const ShapeIndex* index_;
  mutable std::vector<ConstantObjectUse> objects_;
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
// Where in the source (#638): a unit's cells are its file; a function's are its definition (an
// instantiation's, its pattern's), read from values the engine already holds.
engine::SourceSpan function_span(const engine::Db& db, const FnKey& k);
engine::SourceSpan tu_span(const engine::Db& db, TuId tu);

// The unit's definitions.
struct TuUnit {
  using Key = TuId;
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return tu_span(db, k); }
  using Value = TranslationUnit;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
};
// The unit's classes and enumerations alone: an edit elsewhere in the unit stops here.
struct UnitShapes {
  using Key = TuId;
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return tu_span(db, k); }
  using Value = Shapes;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
};

// Effective exception specifications needed by native class initializer cleanup.
struct UnitNonthrowingFunctions {
  using Key = TuId;
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return tu_span(db, k); }
  using Value = std::vector<EntityId>;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
};

// A function declared with an ellipsis ([dcl.fct]) and how many parameters precede it.
struct VariadicFunction {
  EntityId entity = EntityId::none;
  std::uint32_t parameters = 0;
};

// Where each function definition and entity is in the unit's `TranslationUnit`, by handle: a
// function's projection finds its own records in O(log n) instead of scanning the unit (#938).
struct UnitIndex {
  std::vector<std::pair<EntityId, std::uint32_t>> functions;  // into `TranslationUnit::functions`
  std::vector<std::pair<EntityId, std::uint32_t>> entities;   // into `TranslationUnit::entities`
};
struct UnitIndexQ {
  using Key = TuId;
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return tu_span(db, k); }
  using Value = UnitIndex;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
};

// The facts `function_ir` asks of the rest of the unit, computed once per unit (#938).
struct UnitFunctionFacts {
  std::vector<EntityId> constexpr_functions;        // the unit's, sorted by handle
  std::vector<VariadicFunction> variadic_functions;  // the unit's, sorted by handle
};
struct UnitFunctionFactsQ {
  using Key = TuId;
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return tu_span(db, k); }
  using Value = UnitFunctionFacts;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
};

// F's definition, and what lowering on x86-64 asks of its own entity record.
struct FunctionSource {
  FunctionDef def;
  bool is_main = false;
  bool declared = false;        // the unit has an entity record for F
  bool specialization = false;  // F is a function template specialization
  EntityId scope = EntityId::none;
  EntityId cleanup_resume = EntityId::none;  // the unit's, for a hosted cleanup only
};
struct FunctionSourceQ {
  using Key = FnKey;
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return function_span(db, k); }
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
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return function_span(db, k); }
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
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return function_span(db, k); }
  using Value = FunctionIrValue;
  static engine::query_task<Value> run(engine::Ctx& cx, FnKey k);
  static std::string show(const Value& v);  // the L1 text, for traces (#245)
  static std::string errors(const Value& v) { return v.error ? v.error->detail() : std::string(); }
};

// `function_debug(F)`: node id → `Loc`, and the variables.
struct FunctionDebug {
  using Key = FnKey;
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return function_span(db, k); }
  using Value = ir::FunctionDebug;
  static engine::query_task<Value> run(engine::Ctx& cx, FnKey k);
};

struct Lowered {
  std::optional<L2Body> body;
  std::vector<ConstantObjectUse> objects;  // in order of first use
  std::optional<BackError> error;
  bool unreachable_immediate = false;
  std::vector<RuntimeUse> runtime{};  // in order of first use
};
struct Lower {
  using Key = FnKey;
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return function_span(db, k); }
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
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return function_span(db, k); }
  using Value = UnitRef;
  static engine::query_task<Value> run(engine::Ctx& cx, FnKey k);
  static std::string errors(const Value& v) { return v.error ? v.error->detail() : std::string(); }  // for traces
};

enum class OptimizationMode : std::uint8_t { none, heuristic, solver };

struct OptKey {
  Fp128 unit;
  obj::target target = obj::target::aarch64_macos;
  std::uint8_t level = 0;
  OptimizationMode mode = OptimizationMode::none;
  bool operator==(const OptKey&) const = default;
};
enum class RewriteOutcome : std::uint8_t { none, applied, no_candidates, rejected, unsupported_target };
enum class FusionOutcome : std::uint8_t { none, no_candidates, invalid_model, budget_exhausted, rejected };
struct Machine {
  std::optional<aarch64::MachineCode> code;
  std::string error;
  std::optional<x86_64::MachineCode> linux_code{};
  RewriteOutcome rewrites = RewriteOutcome::none;
  std::uint32_t candidates = 0;
  std::uint32_t fused_pairs = 0;
  std::uint32_t saved_bytes = 0;
  FusionOutcome fusion = FusionOutcome::none;
  bool ok() const { return code.has_value() || linux_code.has_value(); }
};
// `optimized(unit, target, options, optimizer_version, level)` (§A8.1). The version is the
// binary's, and a daemon serves one binary.
struct Optimized {
  using Key = OptKey;
  using Value = Machine;
  static engine::query_task<Value> run(engine::Ctx& cx, OptKey k);
  static std::string describe(const Key& k);
  static std::string show(const Value& v);  // the instruction words, for traces (#245)
  static std::string errors(const Value& v) { return v.ok() ? std::string() : v.error; }
};

// `codegen(F)`: `best_available(unit, B, S)` (§A8.3; optimization design §7: S is consulted only
// for B ≥ 1, so -O0 is level 0 exactly).
struct Codegen {
  struct Key {
    FnKey function;
    OptimizationMode mode = OptimizationMode::none;
    bool operator==(const Key&) const = default;

    friend void hash_into(Hasher& h, const Key& k) noexcept {
      hash_append(h, k.function);
      if (k.mode != OptimizationMode::none) h.uint(std::to_underlying(k.mode));
    }
    friend void trace_describe(std::string& out, const Key& k) { trace_describe(out, k.function); }
  };
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return function_span(db, k.function); }
  using Value = Machine;
  static engine::query_task<Value> run(engine::Ctx& cx, Key k);
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
  std::vector<aarch64::Symbol> symbols;
  std::set<EntityId> omitted_immediate_functions;
};
struct Tu {
  struct Key {
    TuId tu;
    bool check_policies = false;
    OptimizationMode optimization = OptimizationMode::none;
    bool operator==(const Key&) const = default;

    friend void hash_into(Hasher& h, const Key& k) noexcept {
      hash_append(h, k.tu);
      if (k.check_policies || k.optimization != OptimizationMode::none)
        h.uint(std::uint8_t(k.check_policies));
      if (k.optimization != OptimizationMode::none) h.uint(std::to_underlying(k.optimization));
    }
    friend void trace_describe(std::string& out, const Key& k) { trace_describe(out, k.tu); }
  };
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return tu_span(db, k.tu); }
  using Value = TuValue;
  static engine::query_task<Value> run(engine::Ctx& cx, Key k);
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

const pp::Config* unit_config(engine::SyncCtx& cx, TuId tu) {
  const TuSource& src = cx.fetch<TuSourceIn>(tu);
  return src.file ? &cx.fetch<pp::SourceOf>(pp::FileId{*src.file}).config : nullptr;
}

// A source unit uses the target in its configuration; a given unit uses its owner's target.
const ast::Target& unit_target(engine::SyncCtx& cx, TuId tu) {
  const auto* config = unit_config(cx, tu);
  return config ? *config->target : *universe().target;
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
    return {.kind = array, .size = e.size * n, .align = e.align, .contains_member_pointer = e.contains_member_pointer,
            .layout_uncertain = e.layout_uncertain, .contains_explicit_alignment = e.contains_explicit_alignment,
            .unusable_class_alignment = e.unusable_class_alignment};
  }
  if (in.types.is<ast::TFunction>(c)) return {function, 0, 1, false, {}};
  // A reference member is held as the address of its object ([dcl.ref]).
  if (in.types.is<ast::TLRef>(c) || in.types.is<ast::TRRef>(c))
    return {pointer, target.pointer_size, target.pointer_align, false,
            in.canonical(in.types.is<ast::TLRef>(c) ? in.types.get<ast::TLRef>(c).referee : in.types.get<ast::TRRef>(c).referee)};
  if (in.types.is<ast::TClass>(c)) {
    const auto key = std::to_underlying(in.types.get<ast::TClass>(c).entity);
    // A peer object's field offsets are available through step(), with no native complete-object type.
    if (const ClassShape* shape = shape_of(EntityId(key))) {
      if (shape->clang_weak_alignment)
        return {.kind = record, .size = shape->clang_weak_alignment, .align = shape->clang_weak_alignment,
                .unusable_class_alignment = true};
      if (shape->layout_requires_virtual_support) return {};
    }
    if (auto done = records_.find(key); done != records_.end()) return done->second;
  }
  if (auto r = class_layout(c)) {
    const ClassShape* it = shape_of(in.types.get<ast::TClass>(c).entity);
    ir::TypeLayout l{record, r->size, r->align, false, {}, it->trivial_calls};
    l.empty = r->placed.empty;  // no data members, in it or its bases
    l.data = r->placed.nvsize;
    l.copy_abi_unknown = it->copy_abi_unknown;
    l.calls_abi_uncertain = it->calls_abi_uncertain;
    l.layout_uncertain = it->layout_uncertain;
    l.contains_explicit_alignment = it->explicit_alignment != 0 || !it->field_alignments.empty();
    if (target.triple == ast::x86_64_linux.triple) {
      l.sysv_stack_record = it->gcc_stack_calls && !l.layout_uncertain && !l.empty && l.align <= 16;
      if (!l.sysv_stack_record) l.sysv_integer_eightbytes = sysv_integer_eightbytes(*it, *r);
      l.sysv_empty_record = l.empty && l.size == 1 && l.align == 1 && l.trivial_calls &&
                            !l.copy_abi_unknown && !l.calls_abi_uncertain && !l.layout_uncertain &&
                            !l.contains_explicit_alignment && !it->union_ && it->bases.empty() && it->fields.empty();
    }
    // A homogeneous floating-point aggregate (AAPCS64 §5.9.5): every member, through arrays and
    // member classes, of one floating type, 1 to 4 of them; a union's members overlap, so it has
    // as many as its largest.
    bool union_ = it->union_;
    std::uint32_t count = 0;
    std::uint8_t bytes = 0;
    // Its non-empty bases' members count as its own (AAPCS64 §5.9.5, as Clang has it).
    std::vector<ir::SubObject> parts;
    for (const ir::SubObject& b : r->bases) {
      const auto base = type(b.type);
      l.layout_uncertain |= base.layout_uncertain;
      l.unusable_class_alignment |= base.unusable_class_alignment;
      l.contains_explicit_alignment |= base.contains_explicit_alignment;
      if (!base.empty) parts.push_back(b);
    }
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
      l.layout_uncertain |= m.layout_uncertain;
      l.unusable_class_alignment |= m.unusable_class_alignment;
      l.contains_explicit_alignment |= m.contains_explicit_alignment;
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
    if (l.unusable_class_alignment) {
      l.size = 0;
      l.align = 1;
    }
    records_[std::to_underlying(in.types.get<ast::TClass>(c).entity)] = l;
    return l;
  }
  // An enumeration is laid out as its underlying type ([dcl.enum]/8).
  if (in.types.is<ast::TEnum>(c))
    if (const EnumShape* it = enum_of(in.types.get<ast::TEnum>(c).entity)) return type(it->underlying);
  return {};
}

// Only the certified automatic-object operation can request this virtual object's storage.
// Passing or returning the class by value still asks type(), which guards virtual layouts.
std::optional<ir::TypeLayout> QueryLayout::peer_root_allocation(QualType q) const {
  auto& in = *universe().in;
  const auto c = in.types.canon(q);
  if (!in.types.is<ast::TClass>(c)) return std::nullopt;
  const ClassShape* shape = shape_of(in.types.get<ast::TClass>(c).entity);
  if (!shape || shape->layout_uncertain || shape->fields.size() != 1 ||
      (shape->peer_virtual_layout != sema::PeerVirtualLayout::root &&
       shape->peer_automatic_primary_base == EntityId::none)) return std::nullopt;
  if (const auto layout = class_layout(c))
    return ir::TypeLayout{.kind = ir::TypeKind::record, .size = layout->size, .align = layout->align};
  return std::nullopt;
}

// A const object still permits writes to its mutable subobjects ([dcl.type.cv]).
bool QueryLayout::contains_mutable_field(QualType type) const {
  ast::Interners& in = *universe().in;
  std::set<EntityId> visited;
  auto contains = [&](this auto&& visit, QualType t) -> bool {
    auto kind = in.types.canon(t);
    while (in.types.is<ast::TArray>(kind)) kind = in.types.canon(in.types.get<ast::TArray>(kind).elem);
    if (!in.types.is<ast::TClass>(kind)) return false;
    EntityId entity = in.types.get<ast::TClass>(kind).entity;
    if (!visited.insert(entity).second) return false;
    const ClassShape* found = shape_of(entity);
    if (!found) return false;
    const ClassShape& shape = *found;
    return shape.has_mutable_field || std::ranges::any_of(shape.fields, [&](QualType field) { return visit(field); }) ||
           std::ranges::any_of(shape.bases, [&](EntityId base) { return visit(in.intern_type(ast::TClass{base})); });
  };
  return contains(type);
}

// System V AMD64 psABI §3.2.3. Flat records of integral, object-pointer or object lvalue-reference members.
// Padding leaves the corresponding eightbyte class unchanged (initially NO_CLASS).
std::uint8_t QueryLayout::sysv_integer_eightbytes(const ClassShape& shape, const ClassLayout& l) const {
  // Only a 128-bit integer member raises the alignment above 8: an alignment-specifier is refused.
  if (!shape.trivial_calls || shape.layout_uncertain || shape.union_ || !shape.bases.empty() || !shape.bit_widths.empty() ||
      l.fields.empty() || l.size > 16 || (l.align > 8 && (shape.explicit_alignment || !shape.field_alignments.empty()))) return 0;
  const ast::Interners& in = *universe().in;
  std::uint64_t end = 0;
  std::uint8_t mask = 0;
  for (const ir::SubObject& f : l.fields) {
    ast::BuiltinLayout field;
    if (auto b = ast::builtin_of(in, f.type); b && ast::is_integral(*b))
      field = (*target_)[*b];
    else if ((ast::is_pointer(in, f.type) && !ast::is_function(in, ast::pointee_of(in, f.type))) ||
             (in.types.is<ast::TLRef>(f.type) && !ast::is_function(in, in.types.get<ast::TLRef>(f.type).referee)))
      field = {target_->pointer_size, target_->pointer_align};
    else return 0;
    if (!field.size || field.size > 16 || field.align > l.align || f.offset % field.align ||
        f.offset < end || f.offset + field.size > l.size) return 0;
    end = f.offset + field.size;
    mask |= std::uint8_t((1u << (f.offset / 8)) | (1u << ((end - 1) / 8)));
  }
  return mask;
}

// A class's layout (`ast::record_layout`, the rule sema's `sizeof` reads too). Its members are the
// unit's shape of it, when units come from source; none otherwise.
std::optional<ClassLayout> QueryLayout::class_layout(QualType c) const {
  ast::Interners& in = *universe().in;
  if (!in.types.is<ast::TClass>(c)) return std::nullopt;
  auto key = std::to_underlying(in.types.get<ast::TClass>(c).entity);
  if (auto done = class_layouts_.find(key); done != class_layouts_.end()) return done->second;
  const ClassShape* it = shape_of(EntityId(key));
  if (it && it->clang_weak_alignment) return std::nullopt;
  if (!it || (it->layout_requires_virtual_support &&
                               it->peer_virtual_layout == sema::PeerVirtualLayout::none &&
                               it->peer_automatic_primary_base == EntityId::none))
    return std::nullopt;
  std::vector<ast::Placed> bases, places;
  for (EntityId b : it->bases) {
    if (b == it->peer_automatic_primary_base)
      bases.push_back(ast::scalar_placed(target_->pointer_size, target_->pointer_align));
    else
      bases.push_back(placed(in.intern_type(ast::TClass{b})));
  }
  for (std::size_t i = 0; i < it->fields.size(); ++i) {
    ast::Placed f = placed(in.canonical(it->fields[i]));
    if (!it->bit_widths.empty()) f.bit_width = it->bit_widths[i];
    if (!it->field_alignments.empty()) f.align = std::max(f.align, std::uint64_t(it->field_alignments[i]));
    places.push_back(std::move(f));
  }
  const ast::Target* root = it->peer_virtual_layout == sema::PeerVirtualLayout::root || it->dynamic_root ? target_ : nullptr;
  ast::RecordLayout r = ast::record_layout(bases, places, it->union_, it->pod, key, root,
                                          it->explicit_alignment);
  ClassLayout out{r.size, std::uint32_t(r.align), {}, {}, r.placed};
  for (std::size_t i = 0; i < r.offsets.size(); ++i)
    out.fields.push_back({r.offsets[i], in.canonical(it->fields[i]), r.bit_fields[i]});
  for (std::size_t i = 0; i < r.base_offsets.size(); ++i) out.bases.push_back({r.base_offsets[i], in.intern_type(ast::TClass{it->bases[i]})});
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

ConstLayout LayoutConstant::run(engine::SyncCtx&, Key key) {
  ast::Interners& in = *universe().in;
  const ConstId c = key.value;
  if (auto a = universe().consts->address(c)) {
    if (in.values.is<ast::VEntity>(a->object)) {
      const EntityId entity = in.values.get<ast::VEntity>(a->object).entity;
      // Descriptor records are created in the target's semantic world. The same interned
      // address can be used on both targets before both worlds contain the corresponding record.
      if (const auto* world = universe().world_for(key.linux_target ? ast::x86_64_linux : ast::aarch64_darwin)) {
        if (const auto* record = world->entities.find(entity); record && record->constant_object != ConstId::none)
          return {a->offset, 0, entity, record->constant_object};
      }
      return {a->offset, 0, entity, {}};
    }
    EntityId e = in.entities.add<EntityId>(ast::mix(ast::seed("ycxx.pipeline.string"), in.values.fp(a->object)));
    return {a->offset, 0, e, a->object};
  }
  if (in.values.is<ast::VInt>(c)) {
    const ast::Bits128 v = in.values.get<ast::VInt>(c).bits;
    return {std::int64_t(v.lo), std::int64_t(v.hi), {}, {}};
  }
  if (in.values.is<ast::VFloat>(c)) return {std::int64_t(in.values.get<ast::VFloat>(c).bits.lo), 0, {}, {}};
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

engine::SourceSpan tu_span(const engine::Db& db, TuId tu) {
  const TuSource* src = db.peek<TuSourceIn>(tu);
  return src && src->file ? pp::file_span(db, pp::FileId{*src->file}) : engine::SourceSpan{};
}

engine::SourceSpan function_span(const engine::Db& db, const FnKey& k) {
  const TuSource* src = db.peek<TuSourceIn>(k.tu);
  const TranslationUnit* tu = db.peek<TuUnit>(k.tu);
  if (!src || !src->file || !tu) return {};
  for (const FunctionDef& f : tu->functions)
    if (f.entity == k.fn) return item_span(db, *universe().in, pp::FileId{*src->file}, f.item);
  return {};
}


engine::query_task<UnitNonthrowingFunctions::Value> UnitNonthrowingFunctions::run(engine::Ctx& cx, TuId id) {
  const TranslationUnit& tu = co_await cx.fetch<TuUnit>(id);
  std::vector<EntityId> functions;
  for (const Entity& entity : tu.entities)
    if (entity.nonthrowing) functions.push_back(entity.id);
  std::ranges::sort(functions);
  co_return functions;
}

namespace {
template <class Pair>
std::optional<std::uint32_t> indexed(const std::vector<Pair>& by_handle, EntityId e) {
  auto it = std::ranges::lower_bound(by_handle, e, {}, &Pair::first);
  if (it == by_handle.end() || it->first != e) return std::nullopt;
  return it->second;
}
}  // namespace

engine::query_task<UnitIndex> UnitIndexQ::run(engine::Ctx& cx, TuId tu) {
  const TranslationUnit& u = co_await cx.fetch<TuUnit>(tu);
  UnitIndex out;
  for (std::uint32_t i = 0; i < u.functions.size(); ++i) out.functions.emplace_back(u.functions[i].entity, i);
  for (std::uint32_t i = 0; i < u.entities.size(); ++i) out.entities.emplace_back(u.entities[i].id, i);
  // Stable: equal handles keep the unit's order, so a lookup finds what a scan of it would.
  for (auto* v : {&out.functions, &out.entities}) std::ranges::stable_sort(*v, {}, &std::pair<EntityId, std::uint32_t>::first);
  co_return out;
}

engine::query_task<ShapeIndex> UnitShapeIndex::run(engine::Ctx& cx, TuId tu) {
  const Shapes& shapes = co_await cx.fetch<UnitShapes>(tu);
  ShapeIndex out;
  for (std::uint32_t i = 0; i < shapes.classes.size(); ++i) out.classes.emplace_back(shapes.classes[i].entity, i);
  for (std::uint32_t i = 0; i < shapes.enums.size(); ++i) out.enums.emplace_back(shapes.enums[i].entity, i);
  // Stable: a lookup finds the first record of an entity, as the maps it replaces kept it.
  for (auto* v : {&out.classes, &out.enums}) std::ranges::stable_sort(*v, {}, &std::pair<EntityId, std::uint32_t>::first);
  co_return out;
}

engine::query_task<UnitFunctionFacts> UnitFunctionFactsQ::run(engine::Ctx& cx, TuId tu) {
  const TranslationUnit& u = co_await cx.fetch<TuUnit>(tu);
  UnitFunctionFacts out;
  irgen::TypeFacts types(*universe().in, unit_target(cx, tu));
  for (const Entity& e : u.entities) {
    if (!e.is_function) continue;
    if (e.is_constexpr) out.constexpr_functions.push_back(e.id);
    if (auto n = types.parameters_before_ellipsis(e.type)) out.variadic_functions.push_back({e.id, *n});
  }
  std::ranges::sort(out.constexpr_functions);
  std::ranges::sort(out.variadic_functions, {}, &VariadicFunction::entity);
  co_return out;
}

engine::query_task<FunctionSource> FunctionSourceQ::run(engine::Ctx& cx, FnKey k) {
  auto [tu, index] = co_await cx.par_fetch(engine::q<TuUnit>{k.tu}, engine::q<UnitIndexQ>{k.tu});
  FunctionSource out;
  // The last definition, as a scan of the definitions in order would keep.
  for (auto it = std::ranges::lower_bound(index.functions, k.fn, {}, &std::pair<EntityId, std::uint32_t>::first);
       it != index.functions.end() && it->first == k.fn; ++it)
    out.def = tu.functions[it->second];
  out.is_main = k.fn == tu.main;
  if (auto e = indexed(index.entities, k.fn)) {
    const Entity& entity = tu.entities[*e];
    out.declared = true;
    out.specialization = entity.template_type.has_value();
    out.scope = entity.scope;
  }
  if (out.def.unwind == UnwindEligibility::hosted_cleanup) out.cleanup_resume = tu.cleanup_resume;
  co_return out;
}

// `Facts` for `function_ir`: types and integer constants from the interners, string literals from
// `Constants`, declarations from the function's source.
class UnitFacts : public irgen::TypeFacts {
 public:
  UnitFacts(const FunctionSource& src, const UnitFunctionFacts& unit, const Shapes& shapes, const ShapeIndex& index,
            const ast::Target& target)
      : TypeFacts(*universe().in, target), src_(&src), unit_(&unit) {
    // Looked up through the unit's index, not by a scan of its classes per question (#938).
    enum_underlying = [&shapes, &index](EntityId e) {
      if (const EnumShape* s = enum_shape(shapes, index, e)) return s->underlying;
      return ast::builtin(*universe().in, ast::Builtin::int_);
    };
    bases_of = [&shapes, &index](EntityId c) {
      if (const ClassShape* s = class_shape(shapes, index, c)) return s->bases;
      return std::vector<EntityId>{};
    };
    bit_field_width = [&shapes, &index](EntityId c, std::uint32_t field) -> std::uint8_t {
      const ClassShape* s = class_shape(shapes, index, c);
      return s && field < s->bit_widths.size() ? s->bit_widths[field] : 0;
    };
    field_type = [&shapes, &index](EntityId c, std::uint32_t field) -> std::optional<QualType> {
      const ClassShape* s = class_shape(shapes, index, c);
      if (s && field < s->fields.size()) return s->fields[field];
      return std::nullopt;
    };
    potentially_overlapping_field = [&shapes, &index](EntityId c, std::uint32_t field) {
      const ClassShape* s = class_shape(shapes, index, c);
      return s && std::ranges::binary_search(s->overlapping_fields, field);
    };
  }
  ConstId string_address(ConstId c) { return universe().consts->address_of(c); }
  bool is_constexpr(EntityId e) { return std::ranges::binary_search(unit_->constexpr_functions, e); }
  bool is_main(EntityId e) { return e != EntityId::none && e == src_->def.entity && src_->is_main; }
  std::optional<std::uint32_t> ellipsis_after(EntityId e) {
    auto it = std::ranges::lower_bound(unit_->variadic_functions, e, {}, &VariadicFunction::entity);
    if (it == unit_->variadic_functions.end() || it->entity != e) return std::nullopt;
    return it->parameters;
  }

 private:
  const FunctionSource* src_;
  const UnitFunctionFacts* unit_;
};
static_assert(irgen::Facts<UnitFacts>);

engine::query_task<Generated> FunctionGen::run(engine::Ctx& cx, FnKey k) {
  auto [src, unit, shapes, shape_index] =
      co_await cx.par_fetch(engine::q<FunctionSourceQ>{k}, engine::q<UnitFunctionFactsQ>{k.tu}, engine::q<UnitShapes>{k.tu},
                            engine::q<UnitShapeIndex>{k.tu});
  const auto* config = unit_config(cx, k.tu);
  UnitFacts facts(src, unit, shapes, shape_index, config ? *config->target : *universe().target);
  const FunctionDef& d = src.def;
  irgen::FunctionInput input{d.entity, d.decl, d.body, d.item, d.body_offset};
  input.exceptions_enabled = !config || config->exceptions_enabled;
  input.this_type = d.this_type;
  input.destructor_table = d.destructor_table;
  input.constructor_table = d.constructor_table;
  const auto& seqs = universe().in->seqs;
  const auto locals = d.body.locals;
  bool needs_nonthrowing = facts.classify(facts.return_type(d.decl.type)) == irgen::TypeClass::record;
  for (std::uint32_t index = 0, count = seqs.size(locals); !needs_nonthrowing && index < count; ++index) {
    QualType type = seqs.at(locals, index).type;
    while (facts.classify(type) == irgen::TypeClass::array) type = facts.element(type);
    needs_nonthrowing = facts.classify(type) == irgen::TypeClass::record;
  }
  if (needs_nonthrowing)
    input.nonthrowing_functions = co_await cx.fetch<UnitNonthrowingFunctions>(k.tu);
  input.guards = d.guards;
  input.guard_acquire = d.guard_acquire;
  input.guard_release = d.guard_release;
  input.atexit = d.atexit;
  input.dso = d.dso;
  if (d.unwind == UnwindEligibility::hosted_catch) {
    input.hosted_catch = true;
    input.catch_int_binding = d.hosted_catch->int_binding;
  }
  const bool target_is_linux = facts.target().triple == ast::x86_64_linux.triple;
  if (input.exceptions_enabled) {
    if (target_is_linux) input.unwind_cleanup_checks = irgen::UnwindCleanupChecks::all_calls;
    else if (facts.target().triple == ast::aarch64_darwin.triple)
      input.unwind_cleanup_checks = irgen::UnwindCleanupChecks::aggregate_defaults;
  }
  if (d.constructor_cleanup != EntityId::none && d.unwind == UnwindEligibility::none) {
    auto error = co_await located_error(cx, k.tu, ast::Loc{d.item, d.body_offset},
                                       "unsupported unwind from a union constructor with a nontrivial active member");
    co_return Generated{std::nullopt, std::move(error)};
  }
  if (d.unwind == UnwindEligibility::hosted_cleanup) {
    input.cleanup_call = d.cleanup_call;
    input.constructor_cleanup = d.constructor_cleanup;
  }
  input.native_array_delete = target_is_linux;
  input.native_array_new = target_is_linux && (d.unwind == UnwindEligibility::cleanup_free ||
                                     d.unwind == UnwindEligibility::cleanup_free_with_throw);
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

std::string Optimized::describe(const Key& k) {
  std::string out = "{";
  engine::describe_into(out, k.unit);
  out += ", ";
  engine::describe_into(out, k.target);
  out += ", ";
  engine::describe_into(out, k.level);
  if (k.mode != OptimizationMode::none) {
    out += ", ";
    engine::describe_into(out, k.mode);
  }
  return out + '}';
}

std::string Optimized::show(const Machine& m) {
  std::string decision;
  switch (m.rewrites) {
    case RewriteOutcome::none: break;
    case RewriteOutcome::applied:
      decision = std::format("{0}=heuristic candidates={1} applied={1} validation=passed\n",
                             m.fused_pairs ? "prepass" : "path", m.candidates);
      break;
    case RewriteOutcome::no_candidates:
      if (!m.fused_pairs && m.fusion == FusionOutcome::none) decision = "path=baseline fallback=no_candidates\n";
      break;
    case RewriteOutcome::rejected:
      decision = std::format("path=baseline candidates={} applied=0 fallback=validation_or_emission_failed\n",
                             m.candidates);
      break;
    case RewriteOutcome::unsupported_target: decision = "path=baseline fallback=unsupported_target\n"; break;
  }
  if (m.fused_pairs != 0)
    decision = std::format("path=solver fused_pairs={} saved_bytes={} validation=passed\n", m.fused_pairs, m.saved_bytes) + decision;
  std::string_view fusion_fallback;
  switch (m.fusion) {
    case FusionOutcome::none: break;
    case FusionOutcome::no_candidates: fusion_fallback = "no_candidates"; break;
    case FusionOutcome::invalid_model: fusion_fallback = "invalid_model"; break;
    case FusionOutcome::budget_exhausted: fusion_fallback = "budget_exhausted"; break;
    case FusionOutcome::rejected: fusion_fallback = "emitted_size_or_codegen_validation_failed"; break;
  }
  if (!fusion_fallback.empty()) {
    if (decision.empty()) decision = "path=baseline ";
    decision += std::format("fusion_fallback={}\n", fusion_fallback);
  }
  if (m.linux_code)
    return decision + std::format("{} bytes, {} relocations\n", m.linux_code->bytes.size(), m.linux_code->relocs.size());
  if (m.code)
    return decision + std::format("{} bytes, {} relocations\n", m.code->bytes.size(), m.code->relocs.size()) +
           aarch64::disassemble(*m.code, [](EntityId e) { return entity_name(e); });
  return m.error.empty() ? decision : decision + "error: " + m.error;
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

bool linux_integer_scalar(ir::TypeLayout l) {
  return l.kind == ir::TypeKind::boolean || linux_ellipsis_scalar(l) ||
         (l.kind == ir::TypeKind::integer && (l.size == 1 || l.size == 2 || l.size == 16));
}

enum class LinuxSignatureUse { definition, direct_call, indirect_call };

// Check the original signature of each function and call before lowering erases the types.
std::string_view linux_signature_error(const ast::Interners& in, const QueryLayout& layout, QualType type,
                                       LinuxSignatureUse use, bool ordinary) {
  using enum LinuxSignatureUse;
  QualType canonical = in.types.canon(type);
  if (!in.types.is<ast::TFunction>(canonical)) return "the function type is unavailable";
  auto f = in.types.get<ast::TFunction>(canonical);
  if (f.variadic && use == definition && !ordinary) return "only ordinary free variadic definitions are supported";
  const bool fixed_direct = !f.variadic && use != indirect_call;
  // Member functions, closure call operators, template specializations and calls through
  // pointers place INTEGER records as ordinary functions do; `this` is the first INTEGER
  // argument (#640).
  const bool allow_aggregates = !f.variadic;
  auto error_for = [&](QualType value, bool result) -> std::string_view {
    auto builtin = ast::builtin_of(in, value);
    if (builtin == ast::Builtin::long_double) return "x87 long double parameters and results are unsupported";
    if (builtin == ast::Builtin::float_ || builtin == ast::Builtin::double_) return {};
    auto l = layout.type(value);
    if (l.unusable_class_alignment) return "a class whose requested alignment has no usable object layout";
    if (l.copy_abi_unknown)
      return result ? "defaulted copy ABI classification is unavailable for the result"
                    : "defaulted copy ABI classification is unavailable for a parameter";
    if (f.variadic && l.kind == ir::TypeKind::integer && l.size == 16)  // #1172
      return "128-bit integers in variadic functions are unsupported";
    if ((result && l.kind == ir::TypeKind::void_) || linux_integer_scalar(l) ||
        (allow_aggregates && (l.sysv_integer_eightbytes || l.sysv_stack_record))) return {};
    if (fixed_direct && l.sysv_empty_record) return {};
    // A class not trivial for calls passes by address and returns through the hidden result
    // pointer; so does any result of more than two eightbytes (MEMORY) (#641).
    if (allow_aggregates && l.kind == ir::TypeKind::record && !l.empty && !l.layout_uncertain &&
        ((!l.trivial_calls && !l.calls_abi_uncertain) || (l.trivial_calls && result && l.size > 16)))
      return {};
    if (allow_aggregates && l.kind == ir::TypeKind::record)
      return result ? "unsupported aggregate result layout" : "unsupported aggregate parameter layout";
    if (fixed_direct)
      return result ? "unsupported fixed-prototype result type" : "unsupported fixed-prototype parameter type";
    if (use == direct_call)
      return result ? "unsupported direct variadic result type" : "unsupported direct variadic parameter type";
    return result ? "only void, bool, integer, pointer and reference results are supported"
                  : "only bool, integer, pointer and reference parameters are supported";
  };
  if (auto error = error_for(f.ret, true); !error.empty()) return error;
  for (QualType p : in.seqs.elements(f.params))
    if (auto error = error_for(p, false); !error.empty()) return error;
  return {};
}

std::string_view linux_ellipsis_error(const ast::Interners& in, const QueryLayout& layout,
                                      const ir::Function& body, ir::NodeId call) {
  const bool indirect = body.node(call).op == ir::Op::call_ind;
  auto imms = body.imms_of(call);
  auto ellipsis = imms[indirect ? 0 : 1], byval = imms[indirect ? 1 : 2];
  // The tail values have already undergone the default argument promotions ([expr.call]).
  // The by-value bits distinguish class objects from genuine pointer arguments.
  auto args = body.operands_of(call).subspan(indirect ? 2 : 1);
  for (std::size_t i = args.size() - ellipsis; i < args.size(); ++i) {
    QualType type = body.value(args[i]).type.type;
    auto builtin = ast::builtin_of(in, type);
    if (builtin == ast::Builtin::long_double) return "x87 long double ellipsis arguments are unsupported";
    auto l = layout.type(type);
    if (!((byval >> i) & 1) && (linux_ellipsis_scalar(l) || builtin == ast::Builtin::double_))
      continue;
    return "only double, 32- and 64-bit integer, and pointer ellipsis arguments are supported";
  }
  return {};
}

}  // namespace

engine::query_task<Lowered> Lower::run(engine::Ctx& cx, FnKey k) {
  const FunctionIrValue& f = co_await cx.fetch<FunctionIr>(k);
  const ast::Target& target = unit_target(cx, k.tu);
  const bool target_is_linux = object_target(target) == obj::target::x86_64_linux;
  if (!f.body && !target_is_linux) co_return Lowered{std::nullopt, {}, f.error};
  auto [shapes, shape_index] = co_await cx.par_fetch(engine::q<UnitShapes>{k.tu}, engine::q<UnitShapeIndex>{k.tu});
  QueryLayout layout(cx, shapes, shape_index, target);
  ir::LowerConfig config;
  if (f.body) {
    const auto checked = std::ranges::find_if(f.body->nodes, [](ir::Op op) {
      return op == ir::Op::sadd_overflow || op == ir::Op::smul_overflow;
    }, &ir::Node::op);
    if (checked != f.body->nodes.end()) {
      const FunctionSource& src = co_await cx.fetch<FunctionSourceQ>(k);
      // A retained immediate caller can reference this definition. Keep its ABI and symbol
      // while constant evaluation continues to execute the original L1.
      if (src.def.plain_immediate_template)
        config.unreachable_body = ir::NodeId(std::uint32_t(checked - f.body->nodes.begin()));
    }
  }
  if (target_is_linux) {
    const ast::Interners& in = *universe().in;
    const FunctionSource& src = co_await cx.fetch<FunctionSourceQ>(k);
    const FunctionDef& def = src.def;
    // The callees' records below still read the whole unit (#943).
    const TranslationUnit& tu = co_await cx.fetch<TuUnit>(k.tu);
    auto ordinary_free_function = [&](const Entity& e) {
      return !e.template_type && !layout.is_class(e.scope);
    };
    bool ordinary = !def.this_type && src.declared && !src.specialization && !layout.is_class(src.scope);
    QualType type = def.body.deduced ? def.body.deduced_type : def.decl.type;
    if (auto error = linux_signature_error(in, layout, type, LinuxSignatureUse::definition, ordinary); !error.empty()) {
      auto message = std::format("x86-64 System V ABI: {}", error);
      auto located = co_await located_error(cx, k.tu, ast::Loc{def.item, def.body_offset}, std::move(message));
      co_return Lowered{std::nullopt, {}, std::move(located)};
    }
    if (!f.body) co_return Lowered{std::nullopt, {}, f.error};
    const ir::Function& body = *f.body;
    const std::size_t node_count = config.unreachable_body ? 0 : body.nodes.size();
    for (std::size_t i = 0; i < node_count; ++i) {
      const auto n = ir::NodeId(static_cast<std::uint32_t>(i));
      const ir::Op op = body.node(n).op;
      const bool direct = op == ir::Op::call || op == ir::Op::invoke;
      std::optional<QualType> callee;
      EntityId callee_id = EntityId::none;
      bool ordinary_callee = false;
      if (direct) {
        callee_id = EntityId(static_cast<std::uint32_t>(body.imms_of(n)[0]));
        auto e = std::ranges::find(tu.entities, callee_id, &Entity::id);
        if (e != tu.entities.end()) {
          callee = e->type;
          ordinary_callee = ordinary_free_function(*e);
          if (e->special == Special::constructor && def.unwind_through_constructor != callee_id) {
            const auto constructor = std::ranges::find(tu.functions, callee_id, &FunctionDef::entity);
            if (constructor != tu.functions.end() && constructor->constructor_cleanup != EntityId::none) {
              auto located = co_await located_error(cx, k.tu, ast::Loc{def.item, def.body_offset},
                                                   "unsupported caller of a potentially throwing union constructor");
              co_return Lowered{std::nullopt, {}, std::move(located)};
            }
          }
        }
      } else if (op == ir::Op::call_ind) {
        QualType ptr = body.value(body.operands_of(n)[1]).type.type;
        callee = ast::pointee_of(in, ptr);
      } else continue;
      const auto use = direct ? LinuxSignatureUse::direct_call : LinuxSignatureUse::indirect_call;
      std::string_view error = callee ? linux_signature_error(in, layout, *callee, use, ordinary_callee)
                                      : "the function type is unavailable";
      if (direct && callee && !error.empty()) {
        // The declaration can still have a placeholder return type. Read the signature already
        // deduced in this unit without depending on the callee's lowering ([dcl.spec.auto]).
        const auto definition = std::ranges::find(tu.functions, callee_id, &FunctionDef::entity);
        if (definition != tu.functions.end() && definition->body.deduced)
          error = linux_signature_error(in, layout, definition->body.deduced_type, use, ordinary_callee);
      }
      if (error.empty() && op != ir::Op::invoke) error = linux_ellipsis_error(in, layout, body, n);
      if (error.empty()) continue;
      const ir::FunctionDebug& debug = co_await cx.fetch<FunctionDebug>(k);
      auto at = i < debug.loc.size() ? debug.loc[i] : ast::Loc{def.item, def.body_offset};
      auto located = co_await located_error(cx, k.tu, at, std::format("x86-64 System V ABI: call: {}", error));
      co_return Lowered{std::nullopt, {}, std::move(located)};
    }
    config.sysv_amd64 = true;
    if (def.unwind == UnwindEligibility::cleanup_free_with_throw && tu.throw_runtime)
      config.throw_runtime = &*tu.throw_runtime;
    if (def.unwind == UnwindEligibility::hosted_catch) config.catch_all = tu.catch_runtime;
    if (def.unwind == UnwindEligibility::hosted_cleanup) config.cleanup_resume = tu.cleanup_resume;
    // Extend outgoing narrow results for interoperability. Lowering truncates incoming results
    // to the declared width, as the System V AMD64 psABI §3.2.3 leaves excess bits unspecified.
    config.min_result_bits = 32;
  } else if (!config.unreachable_body && std::ranges::contains(f.body->nodes, ir::Op::invoke, &ir::Node::op)) {
    const FunctionSource& src = co_await cx.fetch<FunctionSourceQ>(k);
    if (src.def.unwind == UnwindEligibility::hosted_cleanup) config.cleanup_resume = src.cleanup_resume;
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
  // Both targets' general registers hold 64 bits: an `i128` becomes two of them (#1172).
  std::vector<RuntimeUse> runtime;
  auto legal = ir::legalize(std::move(*l2), ir::Int128Support{}, [&](ir::Runtime r) {
    auto used = std::ranges::find(runtime, r, &RuntimeUse::function);
    if (used != runtime.end()) return used->entity;
    const EntityId e = universe().in->entities.add<EntityId>(
        ast::mix(ast::seed("ycxx.pipeline.runtime"), ast::seed(ir::name_of(r))));
    runtime.push_back({r, e});
    return e;
  });
  if (!legal) {
    // The L2 node came from an L1 node, whose position it takes.
    const ir::FunctionDebug& debug = co_await cx.fetch<FunctionDebug>(k);
    std::optional<ast::Loc> at;
    if (legal.error().at && ir::raw(*legal.error().at) < debug.loc.size()) at = debug.loc[ir::raw(*legal.error().at)];
    auto error = co_await located_error(cx, k.tu, at, "legalize: " + legal.error().message);
    co_return Lowered{std::nullopt, {}, std::move(error)};
  }
  co_return Lowered{L2Body{std::move(*legal)}, {layout.objects().begin(), layout.objects().end()}, std::nullopt,
                    config.unreachable_body.has_value(), std::move(runtime)};
}

// The entities a function's code refers to (address and direct call operands).
struct SymbolReference {
  ir::EntityId entity;
  l2::Op op;
};

std::vector<SymbolReference> references(const l2::Function& body) {
  std::vector<SymbolReference> out;
  for (std::uint32_t n = 0; n < body.nodes.size(); ++n) {
    l2::Op op = body.nodes[n].op;
    if (op == l2::Op::symbol || op == l2::Op::call || op == l2::Op::invoke || op == l2::Op::invoke_cleanup)
      out.push_back({ir::EntityId(std::uint32_t(body.imms_of(ir::NodeId(n))[0])), op});
  }
  return out;
}

struct SymbolAddresses {
  std::vector<EntityId> direct, got;
};

// ELF default-visible symbols can be preempted, so their addresses use the GOT.
// Local definitions can use the symbol itself in a direct displacement.
struct TuSymbolAddresses {
  using Key = TuId;
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return tu_span(db, k); }
  using Value = SymbolAddresses;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu) {
    auto [u, index] = co_await cx.par_fetch(engine::q<TuUnit>{tu}, engine::q<UnitIndexQ>{tu});
    const bool target_is_linux = object_target(unit_target(cx, tu)) == obj::target::x86_64_linux;
    Value out;
    auto add = [&](EntityId id) {
      if (target_is_linux) {
        auto e = indexed(index.entities, id);
        if (!e || !u.entities[*e].has_local_binding()) return;
      }
      out.direct.push_back(id);
    };
    for (const FunctionDef& f : u.functions) add(f.entity);
    for (const Variable& v : u.variables) add(v.entity);
    if (target_is_linux) {
      for (const Entity& e : u.entities)
        if (!e.has_local_binding() && (e.linkage == ast::Linkage::external || e.linkage == ast::Linkage::module))
          out.got.push_back(e.id);
      std::ranges::sort(out.got);
    }
    std::ranges::sort(out.direct);
    co_return out;
  }
};

// How a function's code addresses one entity it refers to: a cell per entity of the unit, so an
// edit that changes the unit's symbol list (a new definition, a variable's initializer, which is
// part of its identity) re-checks these answers instead of every function's unit (#938).
enum class SymbolAccess : std::uint8_t { other, direct, got };
struct SymbolKey {
  TuId tu;
  EntityId entity = EntityId::none;
  bool operator==(const SymbolKey&) const = default;
};
void trace_describe(std::string& out, const SymbolKey& k) {  // for traces (#245)
  trace_describe(out, k.tu);
  out += ", ";
  trace_describe(out, FnKey{k.tu, k.entity});
}
struct SymbolAccessQ {
  using Key = SymbolKey;
  static engine::SourceSpan trace_span(const engine::Db& db, const Key& k) { return tu_span(db, k.tu); }
  using Value = SymbolAccess;
  static engine::query_task<Value> run(engine::Ctx& cx, SymbolKey k) {
    const SymbolAddresses& symbols = co_await cx.fetch<TuSymbolAddresses>(k.tu);
    if (std::ranges::binary_search(symbols.direct, k.entity)) co_return SymbolAccess::direct;
    if (std::ranges::binary_search(symbols.got, k.entity)) co_return SymbolAccess::got;
    co_return SymbolAccess::other;
  }
};

engine::query_task<UnitRef> UnitQ::run(engine::Ctx& cx, FnKey k) {
  const Lowered& lowered = co_await cx.fetch<Lower>(k);
  if (!lowered.body) co_return UnitRef{std::nullopt, lowered.error};
  // Which references can be addressed directly, including the function's constant objects. The
  // others are asked one entity at a time, in the order the body first refers to them.
  OptUnit u{*lowered.body, {}, {}};
  const std::vector<SymbolReference> refs = references(u.body.f);
  std::vector<SymbolKey> asked;
  for (const SymbolReference& r : refs)
    if (!std::ranges::contains(lowered.objects, r.entity, &ConstantObjectUse::entity) &&
        !std::ranges::contains(asked, r.entity, &SymbolKey::entity))
      asked.push_back({k.tu, r.entity});
  const auto access = co_await cx.par_map<SymbolAccessQ>(asked);
  for (auto [e, op] : refs) {
    auto at = std::ranges::find(asked, e, &SymbolKey::entity);
    const SymbolAccess a = at == asked.end() ? SymbolAccess::direct : access[at - asked.begin()].get();
    if (a == SymbolAccess::direct)
      u.local.push_back(e);
    else if (op == l2::Op::symbol && a == SymbolAccess::got)
      u.got_symbols.push_back(e);
  }
  for (auto* ids : {&u.local, &u.got_symbols}) {
    std::ranges::sort(*ids);
    ids->erase(std::ranges::unique(*ids).begin(), ids->end());
  }
  co_return UnitRef{universe().units->add(std::move(u)), std::nullopt};
}

struct DivisionRewrite {
  std::uint32_t node;
  std::uint32_t immediate;
  l2::Op op;
  std::uint64_t operand;
  bool operator==(const DivisionRewrite&) const = default;
};

namespace {

bool unsigned_division(const l2::Node& node) {
  return node.flags == 0 && (node.op == l2::Op::udiv || node.op == l2::Op::urem);
}

std::vector<DivisionRewrite> power_of_two_divisions(const l2::Function& body) {
  std::vector<DivisionRewrite> rewrites;
  if (std::ranges::none_of(body.nodes, unsigned_division)) return rewrites;

  // Successor arguments live in operands too. Changing the constant is safe only when this
  // division or remainder is its sole use.
  std::vector<std::uint8_t> uses(body.values.size());
  for (const auto operand : body.operands) {
    auto& count = uses[ir::raw(operand)];
    if (count < 2) ++count;
  }
  for (std::uint32_t i = 0; i < body.nodes.size(); ++i) {
    const auto& node = body.nodes[i];
    if (!unsigned_division(node)) continue;
    const auto divisor = body.operands_of(ir::NodeId(i))[1];
    const auto& value = body.value(divisor);
    if (value.param || uses[ir::raw(divisor)] != 1 ||
        (value.type != l2::Type::i32 && value.type != l2::Type::i64))
      continue;
    const auto& constant = body.nodes[value.def];
    if (constant.op != l2::Op::const_) continue;
    auto bits = body.imms[constant.imm0];
    if (value.type == l2::Type::i32) bits = std::uint32_t(bits);
    if (!std::has_single_bit(bits)) continue;

    // In both widths, division by 2^k drops the low k bits. The remainder retains exactly
    // those bits. The nonzero divisor and in-range shift preserve defined behavior.
    if (node.op == l2::Op::udiv)
      rewrites.push_back({i, constant.imm0, l2::Op::lshr, std::uint64_t(std::countr_zero(bits))});
    else
      rewrites.push_back({i, constant.imm0, l2::Op::and_, bits - 1});
  }
  return rewrites;
}

std::optional<l2::Function> rewrite_divisions(const l2::Function& body,
                                              std::span<const DivisionRewrite> rewrites) {
  if (rewrites.empty()) return {};
  auto transformed = body;
  for (const auto& rewrite : rewrites) {
    transformed.nodes[rewrite.node].op = rewrite.op;
    transformed.imms[rewrite.immediate] = rewrite.operand;
  }
  return transformed;
}

}  // namespace

struct DivisionModel {
  using Key = Fp128;
  using Value = std::vector<DivisionRewrite>;
  static engine::query_task<Value> run(engine::Ctx& cx, Key unit) {
    auto rewrites = power_of_two_divisions(universe().units->at(unit).body.f);
    co_await cx.stop_point();
    co_return rewrites;
  }
  static std::string show(const Value& v) {
    return std::format("model=unsigned_power_of_two_division candidates={}", v.size());
  }
};

using DivisionGroups = std::vector<std::vector<x86_64::DivisionSite>>;

void division_prefix_choices(std::span<const x86_64::DivisionSite> group,
                             std::vector<cm::PrefixChoice>& choices) {
  choices.clear();
  std::array<std::uint32_t, 2> counts{};
  for (const auto& site : group) {
    ++counts[site.quotient];
    choices.push_back({std::min(counts[0], counts[1]), site.saved_bytes});
  }
}

struct FusionModel {
  DivisionGroups groups;
  std::string error;
};
struct DivisionFusionModel {
  using Key = Fp128;
  using Value = FusionModel;
  static engine::query_task<Value> run(engine::Ctx& cx, Key unit) {
    co_await cx.stop_point();
    auto groups = x86_64::group_divisions(universe().units->at(unit).body.f, cx.stop_token());
    co_await cx.stop_point();
    if (!groups) co_return Value{{}, groups.error()};
    co_return Value{std::move(*groups), {}};
  }
  static std::string show(const Value& v) {
    if (!v.error.empty()) return "model=unsigned_divrem_fusion fallback=" + v.error;
    auto out = std::format("model=unsigned_divrem_fusion equivalence_groups={}", v.groups.size());
    if (v.groups.size() == 1 && v.groups.front().size() <= 8) {
      std::vector<cm::PrefixChoice> choices;
      division_prefix_choices(v.groups.front(), choices);
      out += "\nprefix_capacity:byte_benefit";
      for (const auto choice : choices) out += std::format(" {}:{}", choice.capacity, choice.benefit);
    }
    return out;
  }
};

struct FusionResult {
  std::vector<x86_64::DivisionPair> pairs;
  std::uint32_t saved_bytes = 0;
  std::size_t visits = 0;
  std::optional<cm::PrefixFailure> failure;
};
struct DivisionFusionSolve {
  static constexpr std::size_t visit_budget = 4'096;
  using Key = Fp128;
  using Value = FusionResult;
  static std::string describe(const Key& unit) {
    std::string key;
    engine::describe_into(key, unit);
    return key + std::format(", model=unsigned_divrem_fusion backend=exact_prefix budget={}", visit_budget);
  }
  static engine::query_task<Value> run(engine::Ctx& cx, Key unit) {
    // Optimized builds the model first. Fetch the completed value to record the dependency,
    // then do the solve synchronously on this worker.
    const auto& model = co_await cx.fetch<DivisionFusionModel>(unit);
    co_await cx.stop_point();
    Value result;
    std::vector<cm::PrefixChoice> choices;
    for (const auto& group : model.groups) {
      co_await cx.stop_point();
      if (group.size() > visit_budget - result.visits)
        co_return Value{{}, 0, result.visits, cm::PrefixFailure::budget_exhausted};

      division_prefix_choices(group, choices);
      const auto selected = cm::solve_prefix_choices(choices, visit_budget - result.visits, cx.stop_token());
      co_await cx.stop_point();
      if (!selected) co_return Value{{}, 0, result.visits, selected.error()};
      result.visits += group.size();

      // An accepted later site must have a free earlier site of the opposite operation.
      // Prefix feasibility ensures this in both directions, so reconstruct in source order.
      std::array<std::vector<ir::NodeId>, 2> free;
      auto next = selected->begin();
      for (std::size_t i = 0; i < group.size(); ++i) {
        const auto& site = group[i];
        if (next == selected->end() || *next != i) {
          free[site.quotient].push_back(site.node);
          continue;
        }
        auto& opposite = free[!site.quotient];
        if (opposite.empty()) co_return Value{{}, 0, result.visits, cm::PrefixFailure::unsupported};
        result.pairs.push_back({opposite.back(), site.node});
        opposite.pop_back();
        result.saved_bytes += site.saved_bytes;
        ++next;
      }
    }
    co_await cx.stop_point();
    co_return result;
  }
  static std::string show(const Value& v) {
    if (v.failure)
      return std::format("model=unsigned_divrem_fusion backend=exact_prefix budget={} visits={} fallback={}",
                         visit_budget, v.visits,
                         *v.failure == cm::PrefixFailure::budget_exhausted ? "budget_exhausted" : "invalid_model");
    return std::format("model=unsigned_divrem_fusion backend=exact_prefix budget={} visits={} outcome=optimal pairs={} predicted_saved_bytes={}",
                       visit_budget, v.visits, v.pairs.size(), v.saved_bytes);
  }
};

struct DivisionFusionValidate {
  using Key = Fp128;
  using Value = Machine;
  static engine::query_task<Value> run(engine::Ctx& cx, Key key) {
    const auto& u = universe().units->at(key);
    const auto& fusion = co_await cx.fetch<DivisionFusionSolve>(key);
    const auto& rewrites = co_await cx.fetch<DivisionModel>(key);
    co_await cx.stop_point();

    auto rewritten = rewrite_divisions(u.body.f, rewrites);
    const auto& body = rewritten ? *rewritten : u.body.f;
    auto heuristic = x86_64::compile_function(body, u.local, u.got_symbols);
    co_await cx.stop_point();
    const auto candidates = std::uint32_t(rewrites.size());
    if (!heuristic) {
      if (rewritten) heuristic = x86_64::compile_function(u.body.f, u.local, u.got_symbols);
      co_await cx.stop_point();
      if (!heuristic)
        co_return Machine{std::nullopt, heuristic.error(), {}, RewriteOutcome::rejected,
                          candidates, 0, 0, FusionOutcome::rejected};
      co_return Machine{std::nullopt, {}, std::move(*heuristic), RewriteOutcome::rejected,
                        candidates, 0, 0, FusionOutcome::rejected};
    }
    const auto outcome = rewritten ? RewriteOutcome::applied : RewriteOutcome::no_candidates;
    auto paired = x86_64::compile_function(body, u.local, u.got_symbols, fusion.pairs);
    co_await cx.stop_point();
    if (paired && paired->bytes.size() + fusion.saved_bytes == heuristic->bytes.size())
      co_return Machine{std::nullopt, {}, std::move(*paired), outcome, candidates,
                        std::uint32_t(fusion.pairs.size()), fusion.saved_bytes};
    co_return Machine{std::nullopt, {}, std::move(*heuristic), outcome, candidates, 0, 0, FusionOutcome::rejected};
  }
  static std::string show(const Value& v) { return Optimized::show(v); }
  static std::string errors(const Value& v) { return Optimized::errors(v); }
};

engine::query_task<Machine> Optimized::run(engine::Ctx& cx, OptKey k) {
  (void)cx.fetch<CostModelIn>(k.target);  // part of `target` (§O8); level 0 reads no cost
  const OptUnit& u = universe().units->at(k.unit);
  // Level 0: `lower` already ran (it made the unit); selection and stack slots (§O7).
  if (k.target == obj::target::x86_64_linux) {
    RewriteOutcome outcome = RewriteOutcome::none;
    std::uint32_t candidates = 0;
    const FusionResult* fusion = nullptr;
    FusionOutcome fusion_outcome = FusionOutcome::none;
    if (k.mode == OptimizationMode::solver) {
      const auto& model = co_await cx.fetch<DivisionFusionModel>(k.unit);
      if (!model.error.empty()) fusion_outcome = FusionOutcome::invalid_model;
      else if (model.groups.empty()) fusion_outcome = FusionOutcome::no_candidates;
      else {
        fusion = &co_await cx.fetch<DivisionFusionSolve>(k.unit);
        if (fusion->failure)
          fusion_outcome = *fusion->failure == cm::PrefixFailure::budget_exhausted
            ? FusionOutcome::budget_exhausted : FusionOutcome::invalid_model;
      }
    }
    if (k.mode != OptimizationMode::none) {
      const auto& rewrites = co_await cx.fetch<DivisionModel>(k.unit);
      candidates = std::uint32_t(rewrites.size());
      outcome = RewriteOutcome::no_candidates;
      if (fusion && !fusion->pairs.empty()) co_return co_await cx.fetch<DivisionFusionValidate>(k.unit);
      if (auto rewritten = rewrite_divisions(u.body.f, rewrites)) {
        auto optimized = x86_64::compile_function(*rewritten, u.local, u.got_symbols);
        co_await cx.stop_point();
        if (optimized)
          co_return Machine{std::nullopt, {}, std::move(*optimized), RewriteOutcome::applied,
                            candidates, 0, 0, fusion_outcome};
        outcome = RewriteOutcome::rejected;
      }
    }
    auto m = x86_64::compile_function(u.body.f, u.local, u.got_symbols);
    if (!m) co_return Machine{std::nullopt, m.error(), {}, outcome, candidates, 0, 0, fusion_outcome};
    co_return Machine{std::nullopt, {}, std::move(*m), outcome, candidates, 0, 0, fusion_outcome};
  }
  const auto outcome = k.mode == OptimizationMode::none ? RewriteOutcome::none : RewriteOutcome::unsupported_target;
  auto m = aarch64::compile_function(u.body.f, u.local);
  if (!m) co_return Machine{std::nullopt, m.error(), {}, outcome};
  co_return Machine{std::move(*m), {}, {}, outcome};
}

engine::query_task<Machine> Codegen::run(engine::Ctx& cx, Key key) {
  const FnKey k = key.function;
  const obj::target target = object_target(unit_target(cx, k.tu));
  const UnitRef& unit = co_await cx.fetch<UnitQ>(k);
  if (!unit.unit) co_return Machine{std::nullopt, unit.error ? unit.error->message : "no unit"};
  // best_available(unit, B, S): the highest level S holds, never one below B.
  std::uint8_t level = cx.fetch<OptLevel>(target);
  if (level >= 1) {
    for (const SnapshotEntry& e : cx.fetch<OptCacheSnapshot>(target))
      if (e.unit == *unit.unit) level = std::max(level, e.level);
  }
  co_return co_await cx.fetch<Optimized>({*unit.unit, target, level, key.mode});
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
    // Little-endian, the high half of a 128-bit integer after the low one.
    ast::Bits128 v = in.values.get<ast::VInt>(init).bits;
    for (std::uint64_t i = 0; i < l.size; ++i) out.bytes[offset + i] = std::uint8_t((i < 8 ? v.lo : v.hi) >> (8 * (i % 8)));
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
  if (l.unusable_class_alignment) return std::unexpected("an object whose class alignment has no usable layout");
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
  bool cleanup_free_unwind;
  std::string_view base_object_name;
  bool catch_int;
};

struct VirtualTable {
  EntityId entity;
  std::string type_encoding;
  std::span<const EntityId> slots;
  std::optional<std::string> base_encoding;
  bool peer_base_rtti = false;
  bool emit_type_info;
};

struct KeylessTypeInfo {
  EntityId entity;
  std::string type_encoding;
};

obj::reloc_kind linux_relocation(x86_64::RelocKind kind) {
  switch (kind) {
    case x86_64::RelocKind::pc32: return obj::reloc_kind::x86_64_pc32;
    case x86_64::RelocKind::plt32: return obj::reloc_kind::x86_64_plt32;
    case x86_64::RelocKind::rex_gotpcrelx: return obj::reloc_kind::x86_64_rex_gotpcrelx;
  }
  std::unreachable();
}

constexpr auto rtti_pointer_size = 8;
constexpr auto rtti_address_point = 2 * rtti_pointer_size;
constexpr std::string_view class_type_info = "_ZTVN10__cxxabiv117__class_type_infoE";
constexpr std::string_view si_class_type_info = "_ZTVN10__cxxabiv120__si_class_type_infoE";

void emit_rtti_pointer(obj::object& o, std::uint32_t index, std::uint32_t symbol, std::int64_t addend = 0) {
  auto& section = o.sections[index];
  section.emit64(0);
  section.relocate(obj::reloc_kind::abs64, symbol, addend);
}

// The name keeps its own vague linkage even when the table and descriptor have a unique
// owner (Itanium C++ ABI §5.2.4).
std::uint32_t emit_type_info_name(obj::object& o, std::uint32_t names, const std::string& encoding, obj::binding bind) {
  const bool elf = o.arch == obj::target::x86_64_linux;
  const std::string name = (elf ? "_ZTS" : "__ZTS") + encoding;
  const auto symbol = o.add_symbol({.name = name, .bind = bind, .section = names, .offset = o.sections[names].size(),
                                    .weak = true, .object_size = encoding.size() + 1});
  if (elf) o.comdat_groups.push_back({name, {names}});
  o.sections[names].emit(encoding);
  o.sections[names].bytes.push_back(0);
  return symbol;
}

void emit_keyless_type_info(obj::object& o, std::uint32_t pointers, std::uint32_t names,
                           std::uint32_t symbol, const std::string& encoding) {
  const bool elf = o.arch == obj::target::x86_64_linux;
  const auto bind = elf ? obj::binding::global : obj::binding::hidden;
  auto& descriptor = o.symbols[symbol];
  descriptor.bind = bind;
  descriptor.section = pointers;
  descriptor.offset = o.sections[pointers].size();
  descriptor.weak = true;
  descriptor.object_size = 2 * rtti_pointer_size;
  if (elf) o.comdat_groups.push_back({descriptor.name, {pointers}});
  const auto name = emit_type_info_name(o, names, encoding, bind);
  const auto runtime_table = elf ? o.reference(class_type_info) : o.reference("_" + std::string(class_type_info));
  emit_rtti_pointer(o, pointers, runtime_table, rtti_address_point);
  emit_rtti_pointer(o, pointers, name, elf ? 0 : std::numeric_limits<std::int64_t>::min());
}

// Itanium C++ ABI §2.5.2 and §2.9.5: a primary table has zero offset-to-top, its RTTI pointer
// and the function slots. A single-inheritance descriptor adds the base RTTI pointer to the
// runtime address point and type name fields. `table_symbol` (already named by the code that
// uses it) and `slots` index `o.symbols`. On ELF the vague-linkage objects are COMDAT groups. On
// Mach-O the names carry the C prefix `_`, the data joins the object's `__DATA,__const` and
// `__TEXT,__const`, and the vague-linkage objects are weak definitions. There the base class's
// descriptor and name, which every user emits, follow Apple's arm64 convention for non-unique
// RTTI: private external, with bit 63 set in the descriptor's name pointer so that `type_info`
// compares those names by content across images.
void emit_virtual_table(obj::object& o, const VirtualTable& table, std::uint32_t table_symbol,
                        std::span<const std::uint32_t> slots, std::optional<std::uint32_t> emitted_base = std::nullopt) {
  using S = obj::standard_section;
  const bool elf = o.arch == obj::target::x86_64_linux;
  const std::string prefix = elf ? "" : "_";
  // ELF gives each object its own section; Mach-O keeps one section of each kind, which the
  // linker splits at symbols (MH_SUBSECTIONS_VIA_SYMBOLS, set for the weak definitions).
  auto section = [&](S kind) {
    const obj::section standard = obj::section::standard(kind);
    if (!elf)
      for (std::uint32_t i = 0; i < o.sections.size(); ++i)
        if (o.sections[i].segment == standard.segment && o.sections[i].name == standard.name) return i;
    return o.add_section(standard);
  };
  const std::uint64_t table_size = rtti_address_point + rtti_pointer_size * slots.size();
  const auto pointers = section(S::data_const);
  o.sections[pointers].align(3);
  const std::uint64_t table_offset = o.sections[pointers].size();
  // Code relocations already name this symbol by index. Define the existing entry.
  o.symbols[table_symbol].section = pointers;
  o.symbols[table_symbol].offset = table_offset;
  o.symbols[table_symbol].object_size = table_size;
  std::optional<std::uint32_t> typeinfo;
  if (table.emit_type_info) {
    typeinfo = o.add_symbol({.name = prefix + "_ZTI" + table.type_encoding, .section = pointers,
                             .offset = table_offset + table_size,
                             .object_size = table.base_encoding ? 3 * rtti_pointer_size : 2 * rtti_pointer_size});
  }
  o.sections[pointers].emit64(0);  // offset-to-top
  if (typeinfo) emit_rtti_pointer(o, pointers, *typeinfo);
  else o.sections[pointers].emit64(0);
  for (std::uint32_t slot : slots) emit_rtti_pointer(o, pointers, slot);
  if (!typeinfo) return;
  const auto name_object = emit_type_info_name(o, section(S::rodata), table.type_encoding, obj::binding::global);
  const auto runtime_type = table.base_encoding ? si_class_type_info : class_type_info;
  emit_rtti_pointer(o, pointers, o.reference(prefix + std::string(runtime_type)), rtti_address_point);
  emit_rtti_pointer(o, pointers, name_object);
  if (const auto& base = table.base_encoding) {
    if (table.peer_base_rtti) {
      emit_rtti_pointer(o, pointers, o.reference(prefix + "_ZTI" + *base));
      return;
    }
    if (emitted_base) {
      emit_rtti_pointer(o, pointers, *emitted_base);
      return;
    }
    const auto base_pointers = elf ? o.add_section(S::data_const) : pointers;
    const auto base_typeinfo = o.add_symbol({.name = prefix + "_ZTI" + *base, .section = std::nullopt});
    emit_rtti_pointer(o, pointers, base_typeinfo);
    emit_keyless_type_info(o, base_pointers, section(S::rodata), base_typeinfo, *base);
  }
}

// A dynamic class's table and RTTI as the object emits them (`DynamicTable`).
struct DynamicTableOut {
  EntityId entity;
  std::string type_encoding;
  std::vector<EntityId> slots;
  std::optional<std::string> base_encoding;
  bool weak = false;
  bool emit_type_info = true;
  bool rtti_only = false;
};

// Itanium C++ ABI §2.5.2, §2.9.5 and §5.2.3–4 for the general dynamic classes (#309): the table
// (offset-to-top zero, RTTI, slots), then a `__class_type_info` descriptor for a root or a
// `__si_class_type_info` one naming the primary base's. A weak table and its descriptor have
// ELF COMDAT groups of their own, keyed by their names; on Mach-O they are weak definitions, and a
// weak descriptor and its name follow Apple's arm64 non-unique RTTI convention (hidden, bit 63
// set in the name pointer) as `emit_keyless_type_info` does. `descriptors` maps each type
// encoding to its descriptor's symbol, for derived classes emitted later in the same object.
void emit_dynamic_descriptor(obj::object& o, const DynamicTableOut& table, std::uint32_t typeinfo_symbol,
                             std::uint32_t shared_pointers, std::map<std::string, std::uint32_t>& descriptors);
void emit_dynamic_table(obj::object& o, const DynamicTableOut& table, std::uint32_t table_symbol,
                        std::span<const std::uint32_t> slots, std::map<std::string, std::uint32_t>& descriptors) {
  using S = obj::standard_section;
  const bool elf = o.arch == obj::target::x86_64_linux;
  const std::string prefix = elf ? "" : "_";
  auto section = [&](S kind) {
    const obj::section standard = obj::section::standard(kind);
    if (!elf)
      for (std::uint32_t i = 0; i < o.sections.size(); ++i)
        if (o.sections[i].segment == standard.segment && o.sections[i].name == standard.name) return i;
    return o.add_section(standard);
  };
  // A symbol by name: the one code already refers to, or a new undefined one.
  auto named = [&](const std::string& name) {
    for (std::uint32_t i = 0; i < o.symbols.size(); ++i)
      if (o.symbols[i].name_view() == name) return i;
    return o.add_symbol({name, obj::binding::global, std::nullopt, 0, false});
  };
  // An RTTI-only entry's symbol is the descriptor itself.
  if (table.rtti_only) {
    if (!table.emit_type_info) return;
    if (!o.symbols[table_symbol].defined())
      emit_dynamic_descriptor(o, table, table_symbol, elf ? 0 : section(S::data_const), descriptors);
    descriptors.emplace(table.type_encoding, table_symbol);
    return;
  }
  const std::uint64_t table_size = rtti_address_point + rtti_pointer_size * slots.size();
  const auto pointers = section(S::data_const);
  o.sections[pointers].align(3);
  auto& symbol = o.symbols[table_symbol];
  symbol.section = pointers;
  symbol.offset = o.sections[pointers].size();
  symbol.object_size = table_size;
  symbol.weak = table.weak;
  if (elf && table.weak) o.comdat_groups.push_back({std::string(symbol.name_view()), {pointers}});
  std::optional<std::uint32_t> typeinfo;
  if (table.emit_type_info) typeinfo = named(prefix + "_ZTI" + table.type_encoding);
  o.sections[pointers].emit64(0);  // offset-to-top
  if (typeinfo) emit_rtti_pointer(o, pointers, *typeinfo);
  else o.sections[pointers].emit64(0);
  for (std::uint32_t slot : slots) emit_rtti_pointer(o, pointers, slot);
  if (!typeinfo) return;
  // A descriptor this object already defines (a peer profile's keyless base) is reused.
  if (!o.symbols[*typeinfo].defined()) emit_dynamic_descriptor(o, table, *typeinfo, pointers, descriptors);
  descriptors.emplace(table.type_encoding, *typeinfo);
}

void emit_dynamic_descriptor(obj::object& o, const DynamicTableOut& table, std::uint32_t typeinfo_symbol,
                             std::uint32_t shared_pointers, std::map<std::string, std::uint32_t>& descriptors) {
  using S = obj::standard_section;
  const bool elf = o.arch == obj::target::x86_64_linux;
  const std::string prefix = elf ? "" : "_";
  const std::optional<std::uint32_t> typeinfo = typeinfo_symbol;
  auto section = [&](S kind) {
    const obj::section standard = obj::section::standard(kind);
    if (!elf)
      for (std::uint32_t i = 0; i < o.sections.size(); ++i)
        if (o.sections[i].segment == standard.segment && o.sections[i].name == standard.name) return i;
    return o.add_section(standard);
  };
  auto named = [&](const std::string& name) {
    for (std::uint32_t i = 0; i < o.symbols.size(); ++i)
      if (o.symbols[i].name_view() == name) return i;
    return o.add_symbol({name, obj::binding::global, std::nullopt, 0, false});
  };
  // A weak descriptor has its own ELF COMDAT group; a strong one follows its table.
  const auto descriptor_section = elf && table.weak ? o.add_section(S::data_const) : shared_pointers;
  o.sections[descriptor_section].align(3);
  const auto bind = !elf && table.weak ? obj::binding::hidden : obj::binding::global;
  auto& descriptor = o.symbols[*typeinfo];
  descriptor.bind = bind;
  descriptor.section = descriptor_section;
  descriptor.offset = o.sections[descriptor_section].size();
  descriptor.weak = table.weak;
  descriptor.object_size = table.base_encoding ? 3 * rtti_pointer_size : 2 * rtti_pointer_size;
  if (elf && table.weak) o.comdat_groups.push_back({prefix + "_ZTI" + table.type_encoding, {descriptor_section}});
  const auto name_object = emit_type_info_name(o, section(S::rodata), table.type_encoding, bind);
  const auto runtime_type = table.base_encoding ? si_class_type_info : class_type_info;
  emit_rtti_pointer(o, descriptor_section, o.reference(prefix + std::string(runtime_type)), rtti_address_point);
  emit_rtti_pointer(o, descriptor_section, name_object, !elf && table.weak ? std::numeric_limits<std::int64_t>::min() : 0);
  if (const auto& base = table.base_encoding) {
    const auto found = descriptors.find(*base);
    emit_rtti_pointer(o, descriptor_section, found != descriptors.end() ? found->second : named(prefix + "_ZTI" + *base));
  }
}

std::expected<obj::object, aarch64::CodegenError> assemble_linux(
    const std::string& name, std::span<const LinuxFunction> functions,
    std::span<const aarch64::DataDef> data, std::span<const aarch64::Symbol> symbols,
    const std::optional<VirtualTable>& table, std::span<const KeylessTypeInfo> keyless_type_info,
    std::span<const EntityId> asm_hidden, std::span<const DynamicTableOut> dynamic_tables,
    const std::set<EntityId>& template_parameter_objects) {
  obj::object o;
  o.name = name;
  o.arch = obj::target::x86_64_linux;
  std::map<EntityId, std::uint32_t> symbol_of;
  // Each definition has its own symbol, even beside another of the same spelling (a weak inline
  // body and a strong asm-labelled one, #694). An entity this object does not define shares a
  // symbol by name, as the linker resolves it: a builtin's implicit C declaration and a written one of
  // another type are two entities with one C symbol (#1095). It names this object's strong
  // definition of the name if there is one, else its weak one, else one undefined symbol.
  std::map<EntityId, bool> definitions;  // entity → weak
  std::vector<EntityId> defined;
  for (const auto& f : functions) defined.push_back(f.entity);
  for (const auto& d : data) defined.push_back(d.entity);
  if (auto error = aarch64::repeated_definition(std::move(defined))) return std::unexpected(std::move(*error));
  for (const auto& f : functions) definitions.emplace(f.entity, f.weak);
  for (const auto& d : data) definitions.emplace(d.entity, d.weak);
  for (const auto& t : keyless_type_info) definitions.emplace(t.entity, false);
  if (table) definitions.emplace(table->entity, false);
  for (const auto& t : dynamic_tables) definitions.emplace(t.entity, t.weak);
  std::map<std::string_view, std::pair<EntityId, bool>> defined_name;  // name → definition, weak
  for (const auto& s : symbols) {
    const auto d = definitions.find(s.entity);
    if (s.bind == obj::binding::local || d == definitions.end()) continue;
    auto [at, added] = defined_name.try_emplace(s.name, s.entity, d->second);
    if (!added && at->second.second && !d->second) at->second = {s.entity, false};
  }
  std::map<std::string_view, std::uint32_t> undefined_name;
  std::vector<const aarch64::Symbol*> defined_by_another;  // whose name another entity defines here
  auto share = [&](const aarch64::Symbol& s, std::uint32_t index) {
    auto& bind = o.symbols[index].bind;
    if (s.bind == obj::binding::hidden) bind = obj::binding::hidden;  // the stricter visibility
    symbol_of.emplace(s.entity, index);
  };
  for (const auto& s : symbols) {
    const bool undefined_nonlocal = s.bind != obj::binding::local && !definitions.contains(s.entity);
    if (undefined_nonlocal) {
      if (defined_name.contains(s.name)) {
        defined_by_another.push_back(&s);
        continue;
      }
      if (auto shared = undefined_name.find(s.name); shared != undefined_name.end()) {
        // Both declarations refer to this one undefined ELF symbol.
        o.symbols[shared->second].weak |= s.weak_reference;
        share(s, shared->second);
        continue;
      }
      undefined_name.emplace(s.name, static_cast<std::uint32_t>(o.symbols.size()));
    }
    symbol_of.emplace(s.entity, o.add_symbol({s.name, s.bind, {}, 0, undefined_nonlocal && s.weak_reference}));
  }
  for (const aarch64::Symbol* s : defined_by_another) share(*s, symbol_of.at(defined_name.at(s->name).first));
  auto missing = [](EntityId e) {
    return std::unexpected(aarch64::CodegenError{e, std::format("@{} has no symbol name", ir::raw(e))});
  };
  std::optional<std::uint32_t> data_const;
  if (!functions.empty()) {
    const std::uint32_t text = o.add_section(obj::standard_section::text);
    std::optional<std::uint32_t> frames, exception_tables, personality_cell, int_type_cell;
    const bool has_handlers = std::ranges::any_of(functions, [](const LinuxFunction& f) {
      return f.code->unwind_sites.has_value();
    });
    if (has_handlers || std::ranges::any_of(functions, &LinuxFunction::cleanup_free_unwind))
      frames = o.add_section(obj::standard_section::eh_frame);
    if (has_handlers) {
      exception_tables = o.add_section(obj::standard_section::gcc_except_table);
      data_const = o.add_section(obj::standard_section::data_const);
      personality_cell = o.define({}, obj::binding::local, *data_const);
      auto& pointers = o.sections[*data_const];
      pointers.emit64(0);
      pointers.relocate(obj::reloc_kind::abs64, o.reference("__gxx_personality_v0"));
      if (std::ranges::any_of(functions, &LinuxFunction::catch_int)) {
        int_type_cell = o.define({}, obj::binding::local, *data_const);
        pointers.emit64(0);
        pointers.relocate(obj::reloc_kind::abs64, o.reference("_ZTIi"));
      }
    }

    x86_64::EhFrameCies cies;
    obj::section& sec = o.sections[text];
    for (const LinuxFunction& f : functions) {
      auto sym = symbol_of.find(f.entity);
      if (sym == symbol_of.end()) return missing(f.entity);
      sec.align(4);
      const std::size_t base = sec.bytes.size();
      auto& symbol = o.symbols[sym->second];
      symbol.section = text;
      symbol.offset = base;
      symbol.weak = f.weak && symbol.bind != obj::binding::local;
      symbol.function_size = f.code->bytes.size();
      if (!f.base_object_name.empty()) {
        obj::symbol base_object = symbol;
        base_object.name = std::string(f.base_object_name);
        o.add_symbol(std::move(base_object));
      }
      sec.emit(f.code->bytes);
      for (const auto& r : f.code->relocs) {
        auto target = symbol_of.find(r.target);
        if (target == symbol_of.end()) return missing(r.target);
        if (base + r.offset > std::numeric_limits<std::uint32_t>::max())
          return std::unexpected(aarch64::CodegenError{f.entity, "a text relocation exceeds the object offset limit"});
        sec.relocate(linux_relocation(r.kind), target->second, r.addend, static_cast<std::uint32_t>(base + r.offset));
      }
      if (f.cleanup_free_unwind || f.code->unwind_sites) {
        std::optional<std::uint32_t> lsda_anchor;
        if (f.code->unwind_sites) {
          if (f.weak) return std::unexpected(aarch64::CodegenError{f.entity, "hosted unwind frames require strong definitions"});
          auto lsda = x86_64::exception_lsda(*f.code, f.catch_int);
          if (!lsda) return std::unexpected(aarch64::CodegenError{f.entity, lsda.error()});
          auto& tables = o.sections[*exception_tables];
          if (lsda->size() + tables.size() > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected(aarch64::CodegenError{f.entity, "the exception tables exceed the object offset limit"});
          lsda_anchor = o.define({}, obj::binding::local, *exception_tables);
          tables.emit(*lsda);
          if (f.catch_int) tables.relocate(obj::reloc_kind::x86_64_pc32, *int_type_cell);
        }
        obj::section& frame = o.sections[*frames];
        auto fields = x86_64::append_eh_frame(frame.bytes, cies, *f.code);
        if (!fields) return std::unexpected(aarch64::CodegenError{f.entity, fields.error()});
        // The host linker may preempt the callable symbol, but this FDE describes these bytes.
        const auto anchor = o.define({}, obj::binding::local, text, base);
        if (fields->personality)
          frame.relocate(obj::reloc_kind::x86_64_pc32, *personality_cell, 0, *fields->personality);
        frame.relocate(obj::reloc_kind::x86_64_pc32, anchor, 0, fields->initial_location);
        if (fields->lsda)
          frame.relocate(obj::reloc_kind::x86_64_pc32, *lsda_anchor, 0, *fields->lsda);
      }
    }
  }
  using K = aarch64::DataKind;
  using S = obj::standard_section;
  for (auto [kind, section] : {std::pair{K::cstring, S::cstring}, {K::rodata, S::rodata}, {K::data_const, S::data_const},
                               {K::data, S::data}, {K::bss, S::bss}, {K::init_func, S::init_array}}) {
    std::optional<std::uint32_t> ordinary = kind == K::data_const ? data_const : std::nullopt;
    for (const auto& d : data) {
      if (d.kind != kind) continue;
      const bool parameter = template_parameter_objects.contains(d.entity);
      if (!parameter && !ordinary) ordinary = o.add_section(section);
      const std::uint32_t index = parameter ? o.add_section(section) : *ordinary;
      obj::section& sec = o.sections[index];
      auto sym = symbol_of.find(d.entity);
      if (sym == symbol_of.end()) return missing(d.entity);
      sec.align(std::countr_zero(std::bit_ceil(d.align)));
      const std::uint64_t base = sec.size();
      auto& symbol = o.symbols[sym->second];
      symbol.section = index;
      symbol.offset = base;
      symbol.weak = symbol.bind == obj::binding::global && d.weak;
      if (parameter) {
        symbol.object_size = d.bytes.size();
        o.comdat_groups.push_back({std::string(symbol.name_view()), {index}});
      }
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
  // If a cast and a separately owned table use the same base, both reference the cast's symbol.
  std::optional<std::uint32_t> emitted_base;
  for (const auto& typeinfo : keyless_type_info) {
    const auto symbol = symbol_of.find(typeinfo.entity);
    if (symbol == symbol_of.end()) return missing(typeinfo.entity);
    const auto pointers = o.add_section(S::data_const);
    const auto names = o.add_section(S::rodata);
    emit_keyless_type_info(o, pointers, names, symbol->second, typeinfo.type_encoding);
    if (table && table->base_encoding == typeinfo.type_encoding) emitted_base = symbol->second;
  }
  if (table) {
    const auto symbol = symbol_of.find(table->entity);
    if (symbol == symbol_of.end()) return missing(table->entity);
    std::vector<std::uint32_t> slots;
    for (EntityId slot : table->slots) {
      const auto target = symbol_of.find(slot);
      if (target == symbol_of.end()) return missing(slot);
      slots.push_back(target->second);
    }
    emit_virtual_table(o, *table, symbol->second, slots, emitted_base);
  }
  std::map<std::string, std::uint32_t> descriptors;
  for (const auto& dynamic : dynamic_tables) {
    const auto symbol = symbol_of.find(dynamic.entity);
    if (symbol == symbol_of.end()) return missing(dynamic.entity);
    std::vector<std::uint32_t> slots;
    for (EntityId slot : dynamic.slots) {
      const auto target = symbol_of.find(slot);
      if (target == symbol_of.end()) return missing(slot);
      slots.push_back(target->second);
    }
    emit_dynamic_table(o, dynamic, symbol->second, slots, descriptors);
  }
  // The directive changes only visibility. Keep the symbol indices that relocations use and
  // the binding, type, size and section determined by ordinary emission.
  for (EntityId e : asm_hidden) o.symbols[symbol_of.at(e)].bind = obj::binding::hidden;
  return o;
}

}  // namespace

// Functions, variables, string literals and symbols in definition order (§A5.3): functions and
// variables as the unit lists them, string literal objects by first use in that order, and
// undefined symbols by first reference.
engine::query_task<TuValue> Tu::run(engine::Ctx& cx, Key k) {
  const TuId id = k.tu;
  const TranslationUnit& tu = co_await cx.fetch<TuUnit>(id);
  ast::Interners& in = *universe().in;
  const auto* config = unit_config(cx, id);
  const ast::Target& target = config ? *config->target : *universe().target;
  const bool emit_table_type_info = !config || config->rtti_enabled;
  const auto compatibility = config ? config->compatibility : ast::Compatibility::spec;
  const bool target_is_linux = object_target(target) == obj::target::x86_64_linux;
  TuValue out;
  std::vector<BackError>& errors = out.errors;
  if (tu.front_failed) {
    // Its diagnostics are the front end's (`TuFront`); this only keeps an object out of a link.
    errors.push_back({std::nullopt, "the translation unit has errors"});
    co_return out;
  }
  if (tu.unsupported_typed_dynamic_cast) {
    errors.push_back({tu.unsupported_typed_dynamic_cast,
                      "typed dynamic_cast requires a supported default-visible root and its direct public peer-owned derived class on Linux x86-64"});
    co_return co_await locate_errors(cx, id, std::move(out));
  }
  if (tu.virtual_emission_blocked) {
    errors.push_back({tu.virtual_declaration, "native output for a translation unit with virtual member functions is not supported yet"});
    co_return co_await locate_errors(cx, id, std::move(out));
  }
  if (target_is_linux && tu.namespace_destruction) {
    errors.push_back({tu.namespace_destruction, "namespace-scope destruction is not supported on x86-64 Linux"});
    co_return co_await locate_errors(cx, id, std::move(out));
  }
  for (const FunctionDef& f : tu.functions)
    if (f.unsupported_class_parameter_abi)
      errors.push_back({ast::Loc{f.item, f.body_offset},
                        "native functions with class-valued template arguments require supported default visibility and no closures or static locals"});
  if (!errors.empty()) co_return co_await locate_errors(cx, id, std::move(out));
  // Checked policy records can require a definition, and raw linker names can refer to
  // another entity without an ordinary typed use of it.
  const bool defer_private_constexpr = !k.check_policies && !tu.has_raw_linkage_names && tu.asm_hidden_symbols.empty();
  std::set<std::uint32_t> deferred;
  if (defer_private_constexpr) {
    std::set<EntityId> candidates;
    for (const FunctionDef& f : tu.functions)
      if (f.private_constexpr && f.coalesced && f.body.tree != ast::NodeId::none)
        candidates.insert(f.entity);
    if (!candidates.empty()) {
      const UnitIndex& index = co_await cx.fetch<UnitIndexQ>(id);
      std::erase_if(candidates, [&](EntityId entity) {
        const auto i = indexed(index.entities, entity);
        if (!i) return true;
        const Entity& symbol = tu.entities[*i];
        return symbol.linkage != ast::Linkage::external || symbol.has_local_binding() || symbol.template_type.has_value();
      });
      // Find resolved direct calls in checked constant-evaluated arms before requesting any L1
      // bodies. Other private helpers stay emission roots, including raw-name providers in
      // another translation unit.
      auto collect = [&](this auto&& visit, const ast::Red& node, bool consteval_arm) -> void {
        if (candidates.empty() || node.is<ast::ConstantProbe>()) return;
        if (node.is<ast::IfConsteval>()) {
          const auto branch = node.get<ast::IfConsteval>();
          const ast::Child arm = branch.negated ? branch.else_ : branch.then_;
          if (arm.id != ast::NodeId::none) visit(ast::Red(in.ast, arm.id), true);
          return;
        }
        if (consteval_arm && node.is<ast::Call>()) {
          const EntityId function = node.get<ast::Call>().function;
          if (candidates.erase(function)) deferred.insert(ir::raw(function));
        }
        node.for_each_child([&](const ast::Red& child) { visit(child, consteval_arm); });
      };
      for (const FunctionDef& f : tu.functions) {
        if (candidates.empty()) break;
        if (f.body.tree != ast::NodeId::none) collect(ast::Red(in.ast, f.body.tree), false);
      }
    }
  }
  std::vector<FnKey> fns;
  for (const FunctionDef& f : tu.functions) {
    if (f.emit_if_referenced) deferred.insert(ir::raw(f.entity));
    else if (!deferred.contains(ir::raw(f.entity))) fns.push_back({id, f.entity});
  }

  // Follow native references in the lowered bodies, where only the runtime edge of an
  // if consteval remains. Each demanded definition can introduce more demands.
  std::set<std::uint32_t> used;
  std::set<EntityId> unreachable_immediate;
  std::vector<FnKey> pending;
  auto use_entity = [&](std::uint32_t entity) {
    if (used.insert(entity).second && deferred.contains(entity))
      pending.push_back({id, EntityId(entity)});
  };
  auto collect_lowered = [&](std::span<const FnKey> keys, const auto& bodies) {
    for (std::size_t i = 0; i < keys.size(); ++i) {
      const Lowered& l = bodies[i];
      if (l.error) errors.push_back(*l.error);
      if (!l.body) continue;
      if (l.unreachable_immediate) unreachable_immediate.insert(keys[i].fn);
      use_entity(ir::raw(keys[i].fn));
      for (const auto& ref : references(l.body->f)) use_entity(ir::raw(ref.entity));
    }
  };
  {
    const auto ls = co_await cx.par_map<Lower>(fns);
    collect_lowered(fns, ls);
  }

  // Variables: their initial bytes, by what they hold ([basic.start.static]).
  std::vector<aarch64::DataDef> data;
  auto [shapes, shape_index] = co_await cx.par_fetch(engine::q<UnitShapes>{id}, engine::q<UnitShapeIndex>{id});
  // One layout for every variable, which keeps its class layouts between them (#938).
  QueryLayout layout(cx, shapes, shape_index, target);
  for (const Variable& v : tu.variables) {
    auto b = initial_bytes(in, layout, v.type, v.init);
    if (!b) {
      errors.push_back({v.at, std::format("{} is not supported yet", b.error())});
      continue;
    }
    if (!b->relocs.empty()) {
      const auto* world = universe().world_for(target);
      const auto* variable = world ? world->entities.find(v.entity) : nullptr;
      const auto* owner = variable && variable->scope != EntityId::none ? world->entities.find(variable->scope) : nullptr;
      const bool shared_storage = variable && variable->linkage != ast::Linkage::internal &&
          (v.inline_definition || variable->templ != EntityId::none || (owner && owner->kind == sema::EntityKind::class_ &&
                                                               owner->templ != EntityId::none));
      if (shared_storage && std::ranges::any_of(b->relocs, [&](const aarch64::DataReloc& relocation) {
            const auto* entity = world->entities.find(relocation.target);
            return entity && entity->kind == sema::EntityKind::template_parameter_object;
          })) {
        errors.push_back({v.at, "shared native storage initialized with a template parameter object address is not supported yet"});
        continue;
      }
    }
    bool is_const = (std::to_underlying(ast::cv_of(in.canonical(v.type))) & 1) != 0;
    bool zero = b->relocs.empty() && std::ranges::all_of(b->bytes, [](std::uint8_t x) { return x == 0; });
    aarch64::DataDef d{v.entity, aarch64::DataKind::data, std::move(b->bytes), 0, layout.type(v.type).align, std::move(b->relocs)};
    d.weak = v.coalesced && !v.dynamic && d.relocs.empty();
    if (is_const && !v.dynamic && !layout.contains_mutable_field(v.type))
      d.kind = d.relocs.empty() ? aarch64::DataKind::rodata : aarch64::DataKind::data_const;
    // A weak Mach-O definition stays out of zero-fill: Apple's linkers coalesce weak definitions
    // in content sections, as Clang emits them, but older ones not in __bss (#124).
    else if (zero && (target_is_linux || !d.weak)) {
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

  // Tables and data can also reference definitions. Process these roots before lowering any
  // deferred definitions, since each newly lowered body can reference another one.
  if (const auto& table = tu.owned_virtual_table) {
    use_entity(ir::raw(table->entity));
    for (EntityId slot : table->slots) use_entity(ir::raw(slot));
  }
  for (const DynamicTable& table : tu.dynamic_tables) {
    use_entity(ir::raw(table.entity));
    for (EntityId slot : table.slots) use_entity(ir::raw(slot));
  }
  for (const aarch64::DataDef& d : data) {
    use_entity(ir::raw(d.entity));
    for (const aarch64::DataReloc& r : d.relocs) use_entity(ir::raw(r.target));
  }
  while (!pending.empty()) {
    auto batch = std::exchange(pending, {});
    const auto ls = co_await cx.par_map<Lower>(batch);
    collect_lowered(batch, ls);
  }

  // Restore definition order for code and constant objects after the reference walk.
  std::vector<const FunctionDef*> definitions;
  fns.clear();
  for (const FunctionDef& f : tu.functions) {
    if (deferred.contains(ir::raw(f.entity)) && !used.contains(ir::raw(f.entity))) {
      if (f.emit_if_referenced) out.omitted_immediate_functions.insert(f.entity);
      continue;
    }
    fns.push_back({id, f.entity});
    definitions.push_back(&f);
  }
  std::vector<ConstantObjectUse> objects;
  std::vector<RuntimeUse> runtime;
  auto use = [&](std::span<const ConstantObjectUse> uses) {
    for (const ConstantObjectUse& u : uses)
      if (!std::ranges::contains(objects, u.object, &ConstantObjectUse::object)) objects.push_back(u);
  };
  {
    const auto ls = co_await cx.par_map<Lower>(fns);  // all hits
    for (const Lowered& l : ls) {
      if (!l.body) continue;
      use(l.objects);
      for (const RuntimeUse& r : l.runtime)
        if (!std::ranges::contains(runtime, r.entity, &RuntimeUse::entity)) runtime.push_back(r);
    }
  }
  for (const RuntimeUse& r : runtime)
    symbols.push_back({r.entity, std::format("{}{}", target_is_linux ? "" : "_", ir::name_of(r.function)), obj::binding::global});
  use(layout.objects());

  // Emit the constant objects referenced by functions and variables. A descriptor's
  // initializer can add the string objects to which its pointer fields refer.
  std::uint32_t n = 0, descriptors = 0;
  for (std::size_t i = 0; i < objects.size(); ++i) {
    const auto [object, entity] = objects[i];
    if (in.values.is<ast::VAggregate>(object)) {
      const auto type = in.values.get<ast::VAggregate>(object).type;
      auto bytes = initial_bytes(in, layout, type, object);
      if (!bytes) {
        errors.push_back({std::nullopt, std::format("source location descriptor: {}", bytes.error())});
        continue;
      }
      data.push_back({entity, aarch64::DataKind::data_const, std::move(bytes->bytes), 0,
                      layout.type(type).align, std::move(bytes->relocs)});
      symbols.push_back({entity, std::format(".L.source_location.{}", descriptors++), obj::binding::local});
      use(layout.objects());
      continue;
    }
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
    symbols.push_back({entity, std::format("{}.str.{}", target_is_linux ? ".L" : "l_", n++), obj::binding::local});
  }

  // The declarations that own closure types, by entity (docs/design/closures.md).
  std::map<std::uint32_t, const Entity*> by_id;
  for (const Entity& e : tu.entities) by_id.emplace(ir::raw(e.id), &e);
  auto spec_of = [](const Entity& e) {
    return TemplateSpec{e.template_type.value_or(e.type), e.template_args, e.template_params, e.template_same_heads,
                        e.template_head_constraint, e.template_trailing_constraint};
  };
  const FunctionNames function_names = [&](EntityId id) -> std::optional<FunctionName> {
    auto found = by_id.find(ir::raw(id));
    if (found == by_id.end()) {
      // An immediate function can occur in a mangled expression without emitted code.
      const sema::World* w = universe().world_for(target);
      if (!w) return std::nullopt;
      const sema::EntityRec& r = w->entities.rec(id);
      if (r.kind != sema::EntityKind::function || r.templ != EntityId::none || r.linkage != ast::Linkage::external)
        return std::nullopt;
      return FunctionName{{std::string(w->ins.names.spelling(r.name)), r.type, r.scope, false, std::nullopt}, r.lang};
    }
    const Entity& fn = *found->second;
    if (!fn.is_function || fn.linkage != ast::Linkage::external || fn.local_parameter_binding)
      return std::nullopt;
    return FunctionName{
        {fn.name, fn.type, fn.scope, false, fn.template_type ? std::optional(spec_of(fn)) : std::nullopt},
        fn.lang};
  };
  const ObjectNames object_names = [&](EntityId id) -> std::optional<ObjectName> {
    const sema::World* w = universe().world_for(target);
    if (!w) return std::nullopt;
    const auto& r = w->entities.rec(id);
    if (r.kind != sema::EntityKind::variable || r.templ != EntityId::none || r.linkage == ast::Linkage::none)
      return std::nullopt;
    auto is_namespace = [&](EntityId scope) {
      return scope == EntityId::none || w->entities.rec(scope).kind == sema::EntityKind::namespace_;
    };
    const bool namespace_object = is_namespace(r.scope);
    if (!namespace_object) {
      const auto& owner = w->entities.rec(r.scope);
      if (owner.kind != sema::EntityKind::class_ || owner.templ != EntityId::none || !is_namespace(owner.scope))
        return std::nullopt;
    }
    if (auto found = by_id.find(ir::raw(id)); found != by_id.end()) {
      const Entity& object = *found->second;
      if (object.local_function != EntityId::none || !object.asm_label.empty()) return std::nullopt;
      return ObjectName{object.name, object.type, r.scope, namespace_object && object.linkage == ast::Linkage::internal};
    }
    // A constant may occur only in a written type and have no emitted object.
    return ObjectName{std::string(w->ins.names.spelling(r.name)), r.type, r.scope,
                      namespace_object && r.linkage == ast::Linkage::internal};
  };
  ClassNames names = [&](EntityId c) {
    const sema::World* w = universe().world_for(target);
    if (!w) return ClassName{};
    const sema::EntityRec& r = w->entities.rec(c);
    ClassName n{std::string(w->ins.names.spelling(r.name)), r.scope, r.templ, r.args,
                r.kind == sema::EntityKind::class_ || r.kind == sema::EntityKind::class_template};
    n.unnamed_type_ordinal = r.unnamed_type_ordinal;
    if (n.is_class && r.templ != EntityId::none) {
      auto params = tu.class_template_parameters.find(r.templ);
      if (params != tu.class_template_parameters.end()) n.template_params = params->second;
    }
    if (r.kind == sema::EntityKind::class_ && n.name.empty() && r.scope != EntityId::none && !n.unnamed_type_ordinal) {
      auto f = by_id.find(ir::raw(r.scope));
      n.closure = true;
      n.closure_type = r.type;
      n.closure_number = r.ordinal;
      n.closure_head = r.closure_head;
      if (f != by_id.end()) {
        const Entity& fe = *f->second;
        if (fe.is_function) {
          std::string fname = fe.special == Special::constructor ? "C1" : fe.special == Special::destructor ? "D1" : fe.name;
          n.local = std::make_shared<const LocalFunction>(LocalFunction{
              fname, fe.type, fe.scope, fe.special != Special::none,
              fe.template_type ? std::optional(spec_of(fe)) : std::nullopt});
        } else {
          n.closure_variable = fe.id;
        }
      } else if (w->entities.rec(r.scope).kind == sema::EntityKind::variable) {
        n.closure_variable = r.scope;  // no symbol of the unit: an identity reserved for its closures (#338)
      }
    }
    return n;
  };
  std::optional<VirtualTable> virtual_table;
  std::vector<KeylessTypeInfo> keyless_type_info;
  std::vector<DynamicTableOut> dynamic_tables;
  std::vector<const Entity*> locals;  // named after their functions, below
  std::map<std::uint32_t, std::string> mangled;  // by function entity
  std::map<EntityId, std::string> base_object_names;
  std::map<std::string_view, bool> asm_names;
  for (const AsmHiddenSymbol& directive : tu.asm_hidden_symbols) asm_names.try_emplace(directive.name, false);
  std::vector<EntityId> asm_hidden;
  std::set<EntityId> template_parameter_objects;
  for (const Entity& e : tu.entities) {
    if (!used.contains(ir::raw(e.id))) continue;
    if (e.unsupported_class_parameter_signature) {
      errors.push_back({e.at, "native functions with class-valued template arguments require default-visible signature and argument types"});
      continue;
    }
    if (e.local_function != EntityId::none) {
      locals.push_back(&e);
      continue;
    }
    if (e.special == Special::virtual_table || e.special == Special::type_info) {
      auto encoding = mangled_type(in, in.intern_type(ast::TClass{e.scope}), names, compatibility);
      const bool table = e.special == Special::virtual_table;
      if (!encoding) {
        errors.push_back({e.at, std::format("the {} type name: {} is not supported yet",
                                            table ? "virtual table's" : "RTTI object's", encoding.error())});
        continue;
      }
      symbols.push_back({e.id, std::format("{}{}{}", target_is_linux ? "" : "_", table ? "_ZTV" : "_ZTI", *encoding),
                         obj::binding::global});
      if (const auto& owned = tu.owned_virtual_table; table && owned && owned->entity == e.id) {
        std::optional<std::string> base;
        if (emit_table_type_info && owned->primary_base != EntityId::none) {
          auto base_encoding = mangled_type(in, in.intern_type(ast::TClass{owned->primary_base}), names, compatibility);
          if (!base_encoding) {
            errors.push_back({e.at, std::format("the virtual table's base type name: {} is not supported yet",
                                                base_encoding.error())});
            continue;
          }
          base = std::move(*base_encoding);
        }
        virtual_table = VirtualTable{e.id, std::move(*encoding), owned->slots, std::move(base),
                                     owned->peer_base_rtti, emit_table_type_info};
      }
      if (const auto dynamic = std::ranges::find(tu.dynamic_tables, e.id, &DynamicTable::entity);
          dynamic != tu.dynamic_tables.end() && table != dynamic->rtti_only) {
        std::optional<std::string> base;
        if (dynamic->base != EntityId::none) {
          auto base_encoding = mangled_type(in, in.intern_type(ast::TClass{dynamic->base}), names, compatibility);
          if (!base_encoding) {
            errors.push_back({e.at, std::format("the virtual table's base type name: {} is not supported yet",
                                                base_encoding.error())});
            continue;
          }
          base = std::move(*base_encoding);
        }
        dynamic_tables.push_back({e.id, *encoding, dynamic->slots, std::move(base), dynamic->weak, emit_table_type_info,
                                  dynamic->rtti_only});
      }
      if (!table && tu.keyless_type_info.contains(e.id)) keyless_type_info.push_back({e.id, std::move(*encoding)});
      continue;
    }
    if (e.special == Special::template_parameter_object) {
      auto name = mangled_template_parameter_object(in, e.template_args.front(), names, compatibility);
      if (!name) errors.push_back({e.at, std::format("the template parameter object name: {}", name.error())});
      else {
        symbols.push_back({e.id, target_is_linux ? std::move(*name) : "_" + *name, obj::binding::global});
        template_parameter_objects.insert(e.id);
      }
      continue;
    }
    // Calls still name the complete-object entry. An eligible definition also emits C2 or D2
    // for a caller constructing or destroying a base subobject (Itanium ABI §5.1.4.3, §5.2.5).
    const std::string_view entry = e.special == Special::constructor ? "C1" :
                                   e.special == Special::destructor ? "D1" :
                                   e.special == Special::base_object_destructor ? "D2" :
                                   e.special == Special::deleting_destructor ? "D0" : std::string_view(e.name);
    const TemplateSpec spec = spec_of(e);
    const auto encode = [&](std::string_view name) {
      return symbol_name(in, name, e.type, e.is_function, e.lang, names, e.scope, e.special != Special::none,
                         e.template_type ? &spec : nullptr, !target_is_linux, function_names, object_names, compatibility);
    };
    auto name = encode(entry);
    if (!name) {
      errors.push_back({e.at, std::format("the name of '{}': {} is not supported yet", e.name, name.error())});
      continue;
    }
    if (e.local_parameter_binding && !std::ranges::contains(tu.functions, e.id, &FunctionDef::entity)) {
      errors.push_back({e.at, std::format("function '{}' names a translation-unit-local type in a parameter "
                                         "and must be defined in this translation unit", e.name)});
      continue;
    }
    mangled.emplace(ir::raw(e.id), *name);  // before an asm label: what its statics' names begin with
    if (!e.asm_label.empty()) name = e.asm_label;
    obj::binding binding = obj::binding::global;
    if (e.has_local_binding()) binding = obj::binding::local;
    else if (e.hidden) binding = obj::binding::hidden;
    if (target.supports(ast::NativeOperation::namespace_hidden_asm) &&
        e.special == Special::none && binding != obj::binding::local) {
      if (auto directive = asm_names.find(*name); directive != asm_names.end()) {
        directive->second = true;
        asm_hidden.push_back(e.id);
      }
    }
    if (tu.base_object_variants.contains(e.id)) {
      auto base_name = encode(e.special == Special::constructor ? "C2" : "D2");
      if (!base_name) {
        errors.push_back({e.at, std::format("the base-object name of '{}': {} is not supported yet", e.name, base_name.error())});
        continue;
      }
      base_object_names.emplace(e.id, std::move(*base_name));
    }
    symbols.push_back({e.id, std::move(*name), binding, e.weak_reference});
  }
  for (const AsmHiddenSymbol& directive : tu.asm_hidden_symbols)
    if (!asm_names.at(directive.name))
      errors.push_back({directive.at, std::format("the .hidden directive requires an emitted or referenced "
                                                  "ordinary ELF symbol named '{}'", directive.name)});
  // A static local and its guard: `_ZZ <encoding> E <name> [_<n>]` and `_ZGV` before it, the
  // function's own <encoding> taken from its symbol (Itanium C++ ABI §5.1.6, §5.1.4.6).
  const std::string_view object_prefix = target_is_linux ? "" : "_";
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
  if (tu.owned_virtual_table && !virtual_table && errors.empty())
    errors.push_back({std::nullopt, "the owned virtual table has no symbol name"});
  if (!errors.empty()) co_return co_await locate_errors(cx, id, std::move(out));

  // Code: codegen(F) for every function, in definition order.
  auto ms = co_await cx.par_map<Codegen>(fns | std::views::transform([&](FnKey fn) {
    return Codegen::Key{fn, k.optimization};
  }));
  for (std::size_t i = 0; i < fns.size(); ++i) {
    if (ms[i].get().ok()) continue;
    const auto& def = *definitions[i];
    errors.push_back({ast::Loc{def.item, def.body_offset}, "codegen: " + ms[i].get().error});
    co_return co_await locate_errors(cx, id, std::move(out));
  }
  const auto weak_definition = [&](const FunctionDef& def) {
    if (def.coalesced || def.explicit_weak) return true;
    if (!unreachable_immediate.contains(def.entity) || def.decl.contracts != ast::NodeId::none ||
        in.seqs.size(def.decl.annotations) != 0)
      return false;
    // Different units can retain the same implicit immediate specialization. Coalesce only its
    // synthesized unreachable definition, keeping source linkage, visibility, and named labels.
    const auto found = by_id.find(ir::raw(def.entity));
    return found != by_id.end() && found->second->linkage == ast::Linkage::external &&
           !found->second->has_local_binding() && found->second->asm_label.empty();
  };
  auto o = [&] {
    if (target_is_linux) {
      std::vector<LinuxFunction> code;
      for (std::size_t i = 0; i < fns.size(); ++i) {
        const auto& def = *definitions[i];
        const auto base_name = base_object_names.find(def.entity);
        const std::string_view name = base_name == base_object_names.end() ? std::string_view{} : base_name->second;
        const bool catch_int = def.unwind == UnwindEligibility::hosted_catch && def.hosted_catch->int_binding.has_value();
        code.push_back({fns[i].fn, &*ms[i].get().linux_code, weak_definition(def),
                        def.unwind != UnwindEligibility::none, name, catch_int});
      }
      return assemble_linux(tu.name, code, data, symbols, virtual_table, keyless_type_info, asm_hidden, dynamic_tables,
                            template_parameter_objects);
    }
    std::vector<aarch64::CompiledFunction> code;
    for (std::size_t i = 0; i < fns.size(); ++i) {
      const auto& def = *definitions[i];
      code.push_back({.entity = fns[i].fn, .code = *ms[i].get().code,
                      .cleanup_free_unwind = def.unwind == UnwindEligibility::cleanup_free,
                      .weak = weak_definition(def)});
    }
    auto o = aarch64::assemble(tu.name, code, data, symbols);
    if (!o) return o;
    // The base-object entries share their complete-object definitions' code (Itanium C++ ABI
    // §5.1.4.3): a second name at the same place.
    for (const auto& [entity, base_name] : base_object_names) {
      const auto named = std::ranges::find(symbols, entity, &aarch64::Symbol::entity);
      const auto complete = named == symbols.end() ? o->symbols.end() :
          std::ranges::find_if(o->symbols, [&](const obj::symbol& s) { return s.defined() && s.name_view() == named->name; });
      if (complete == o->symbols.end()) continue;
      obj::symbol alias = *complete;
      alias.name = base_name;
      o->add_symbol(std::move(alias));
    }
    if (!virtual_table && dynamic_tables.empty()) return o;
    // The table and its slots by name: the code may or may not have referred to them.
    auto symbol = [&](EntityId e) -> std::expected<std::uint32_t, aarch64::CodegenError> {
      const auto named = std::ranges::find(symbols, e, &aarch64::Symbol::entity);
      if (named == symbols.end())
        return std::unexpected(aarch64::CodegenError{e, std::format("@{} has no symbol name", ir::raw(e))});
      for (std::uint32_t i = 0; i < o->symbols.size(); ++i)
        if (o->symbols[i].name_view() == named->name) return i;
      return o->reference(named->name);
    };
    if (virtual_table) {
      const auto table = symbol(virtual_table->entity);
      if (!table) return std::expected<obj::object, aarch64::CodegenError>(std::unexpected(table.error()));
      std::vector<std::uint32_t> slots;
      for (EntityId slot : virtual_table->slots) {
        const auto s = symbol(slot);
        if (!s) return std::expected<obj::object, aarch64::CodegenError>(std::unexpected(s.error()));
        slots.push_back(*s);
      }
      emit_virtual_table(*o, *virtual_table, *table, slots);
    }
    std::map<std::string, std::uint32_t> descriptors;
    for (const auto& dynamic : dynamic_tables) {
      const auto table = symbol(dynamic.entity);
      if (!table) return std::expected<obj::object, aarch64::CodegenError>(std::unexpected(table.error()));
      std::vector<std::uint32_t> slots;
      for (EntityId slot : dynamic.slots) {
        const auto s = symbol(slot);
        if (!s) return std::expected<obj::object, aarch64::CodegenError>(std::unexpected(s.error()));
        slots.push_back(*s);
      }
      emit_dynamic_table(*o, dynamic, *table, slots, descriptors);
    }
    return o;
  }();
  if (!o) {
    std::optional<ast::Loc> at;
    for (const FunctionDef& fn : tu.functions)
      if (fn.entity == o.error().entity) at = ast::Loc{fn.item, fn.body_offset};
    errors.push_back({at, "codegen: " + o.error().message});
    co_return co_await locate_errors(cx, id, std::move(out));
  }
  out.object = std::move(*o);
  out.symbols = std::move(symbols);
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
  const TuValue& v = db.get<Tu>(Tu::Key{TuId{0}});
  if (!v.object) return std::unexpected(v.errors);
  return *v.object;
}

}  // namespace ycxx::pipeline
