// The front half of a translation unit's pipeline (#43, #63), as the engine query `TuFront`: the
// file's text as the engine's input, `pp` and the item splitter as engine queries, then the parser
// and sema (`item_sema`, `analyze_body`) through `parse::Program`, which plays the engine for them
// inside this one cell until contextual queries exist (#16). Its value is the diagnostics, located
// by byte offset, and the `TranslationUnit` the back half compiles; `tu_unit(TU)` projects the
// latter for the back half, so a front end that re-runs with the same definitions changes nothing
// behind it.
export module ycxx.pipeline:front;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.ir.lower;
import ycxx.pp;
import ycxx.items;
import ycxx.sema;
import ycxx.parse;
import :back;
import :lines;

export namespace ycxx::pipeline {

// A diagnostic with its raw byte offset and rendered position, when located in source.
struct Message {
  std::optional<std::uint32_t> offset;
  bool error = true;
  std::string text;
  std::string where;  // file:line:column
  // The region token it is at or before: messages are in this order, across files.
  std::uint32_t token = 0;

  void append_trace_error(std::string& out) const {
    if (!error) return;
    if (!where.empty()) {
      out += where;
      out += ": ";
    }
    out += text;
    out += '\n';
  }
};

// A function body of the unit, for an editor's position requests (#170): its item, the
// declaration part's length (the body tree's item-relative offset) and its tree.
struct BodyAt {
  ast::ItemId item{};
  std::uint32_t offset = 0;
  ast::NodeId tree = ast::NodeId::none;
};

// The region as sema reads it (#947, slice 0): its tokens without where they were written. The
// tokens' locations, expansion sites, `space_before` flags (sema reads none of them) and error
// details are dropped, so an edit that moves tokens without changing them (a comment, blank
// lines, spacing) stops here and analysis does not run again.
struct PpTokens {
  using Key = pp::FileId;
  using Value = pp::Region;
  static engine::query_task<Value> run(engine::Ctx& cx, Key file);
  static engine::SourceSpan trace_span(const engine::Db& db, Key file) { return pp::file_span(db, file); }
};

// Whether the lexer or preprocessor reported an error in a unit's file: a cell of its own, so a
// diagnostic that only moved (its byte offset is part of it) leaves the analysis green.
struct LexErrors {
  using Key = pp::FileId;
  using Value = bool;
  static engine::query_task<Value> run(engine::Ctx& cx, Key file);
  static engine::SourceSpan trace_span(const engine::Db& db, Key file) { return pp::file_span(db, file); }
};

// A sema diagnostic before it is placed: its region token, which `TuFront` turns into a file
// position.
struct SemaMessage {
  bool error = true;
  std::string text;
  std::uint32_t token = 0;
};

// The analysis of a unit (#947, slice 0): what `TuFront` was, less placing messages in files.
struct SemaResult {
  std::vector<SemaMessage> messages;  // in order of position in the unit
  bool lex_errors = false;            // the preprocessor or lexer reported an error
  TranslationUnit unit;               // meaningful when there are no errors
  Shapes shapes;                      // its classes and enumerations, for `layout` (#115)
  std::vector<std::pair<EntityId, ast::Loc>> declared;
  std::vector<BodyAt> bodies;
  bool has_errors() const {
    return lex_errors || std::ranges::any_of(messages, [](const SemaMessage& m) { return m.error; });
  }
};

struct FrontResult {
  std::vector<Message> messages;  // in order of position in the unit
  Shapes shapes;                  // its classes and enumerations, for `layout` (#115)
  // For an editor (#170), with errors or not: each entity's first declaration in the unit (its
  // declarator, else its item's start), in item order, and the analysed function bodies.
  std::vector<std::pair<EntityId, ast::Loc>> declared;
  std::vector<BodyAt> bodies;
  bool has_errors() const {
    return std::ranges::any_of(messages, [](const Message& m) { return m.error; });
  }
};

// Where in the source (#638): a unit's cells are its file (defined with the back end's cells).
engine::SourceSpan tu_span(const engine::Db& db, TuId tu);

// One translation unit from its source file (`TuSource::file`), analysed, from `PpTokens` and the
// items: it holds no file position, so an edit that only moves tokens leaves it green.
struct TuSema {
  using Key = TuId;
  using Value = SemaResult;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
  static engine::SourceSpan trace_span(const engine::Db& db, TuId tu) { return tu_span(db, tu); }
};

// The unit's diagnostics placed in its files, with what an editor reads of its analysis.

struct TuFront {
  using Key = TuId;
  using Value = FrontResult;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
  static engine::SourceSpan trace_span(const engine::Db& db, TuId tu) { return tu_span(db, tu); }
  // For traces (#245): its error messages, one a line.
  static std::string errors(const Value& v) {
    std::string out;
    for (const Message& m : v.messages) m.append_trace_error(out);
    return out;
  }
};

// Go-to-definition (#170, ast.md §8.5): the entity named at a byte offset of a file of a unit (the
// main file or a header it includes), and the place it is first declared in the unit, as a file
// (absolute) and a byte offset. Names in function bodies so far: a name that resolved to an entity
// (`DeclRef`), a member (`Member`) or a called function (`Call`).
struct DefinitionKey {
  TuId tu;
  pp::PathId file;
  std::uint32_t offset = 0;
  bool operator==(const DefinitionKey&) const = default;
};
struct FileOffset {
  std::string path;
  std::uint32_t offset = 0;
};
struct DefinitionAt {
  using Key = DefinitionKey;
  using Value = std::optional<FileOffset>;
  static engine::query_task<Value> run(engine::Ctx& cx, DefinitionKey k);
};

// String literal objects and address constants, from the values interner (`VString`, `VLvalue`).
class ValueConstants final : public Constants {
 public:
  explicit ValueConstants(sema::World& w) : w_(&w) {}
  std::optional<StringObject> string_object(ConstId c) const override {
    sema::ValueRec r = w_->values.rec(c);
    if (r.kind != sema::ValueKind::string) return std::nullopt;
    auto element = w_->types.builtin_of(w_->types.rec(r.type).inner);
    if (!element) return std::nullopt;
    return StringObject{*element, std::move(r.units)};
  }
  std::optional<Address> address(ConstId c) const override {
    sema::ValueRec r = w_->values.rec(c);
    if (r.kind != sema::ValueKind::address) return std::nullopt;
    return Address{r.base, r.offset};
  }
  ConstId address_of(ConstId object) override { return w_->values.address({}, object, 0); }

 private:
  sema::World* w_;
};

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

// A variable with static storage duration initialized or destroyed by code ([basic.start.dynamic],
// [basic.start.term]): its initializer's tree in its declaration's item, if it is not constant.
struct Dynamic {
  EntityId variable;
  QualType type;
  ast::NodeId init;
  EntityId destructor;
  std::uint32_t item;
};

// The C++ runtime symbols the unit references, declared on first use.
template <class Declare>
struct Runtime {
  sema::World& world;
  Declare& declare;
  EntityId atexit_ = EntityId::none, dso_ = EntityId::none, acquire_ = EntityId::none, release_ = EntityId::none;
  EntityId declared(std::string_view n, QualType t, bool fn) {
    EntityId e = world.entities.entity({fn ? sema::EntityKind::function : sema::EntityKind::variable, world.ins.names.intern(n), t,
                                        ast::LangLinkage::c, ast::Linkage::external, {}, EntityId::none});
    ast::DeclSema d{};
    d.entity = e;
    d.type = t;
    declare(d, std::nullopt);
    return e;
  }
  QualType vp() { return world.types.pointer(world.types.builtin(sema::Builtin::void_)); }
  QualType int_() { return world.types.builtin(sema::Builtin::int_); }
  // `int __cxa_atexit(void (*)(void*), void*, void*)` and `__dso_handle`.
  EntityId atexit() {
    if (atexit_ == EntityId::none) {
      QualType v = vp();
      QualType dtor = world.types.pointer(world.types.function(world.types.builtin(sema::Builtin::void_), std::span(&v, 1), false, ast::LangLinkage::c));
      QualType ps[] = {dtor, v, v};
      atexit_ = declared("__cxa_atexit", world.types.function(int_(), ps, false, ast::LangLinkage::c), true);
    }
    return atexit_;
  }
  EntityId dso() {
    if (dso_ == EntityId::none) dso_ = declared("__dso_handle", world.types.builtin(sema::Builtin::char_), false);
    return dso_;
  }
  // `int __cxa_guard_acquire(int64_t*)`, `void __cxa_guard_release(int64_t*)`.
  EntityId acquire() {
    if (acquire_ == EntityId::none) {
      QualType g = world.types.pointer(world.types.builtin(sema::Builtin::long_long));
      acquire_ = declared("__cxa_guard_acquire", world.types.function(int_(), std::span(&g, 1), false, ast::LangLinkage::c), true);
    }
    return acquire_;
  }
  EntityId release() {
    if (release_ == EntityId::none) {
      QualType g = world.types.pointer(world.types.builtin(sema::Builtin::long_long));
      release_ = declared("__cxa_guard_release",
                          world.types.function(world.types.builtin(sema::Builtin::void_), std::span(&g, 1), false, ast::LangLinkage::c), true);
    }
    return release_;
  }
  ir::IntThrowRuntime int_throw() {
    // Itanium EH ABI §2.4. The type information object is opaque to this pipeline.
    const QualType size = world.types.builtin(world.target.size_type);
    const QualType v = world.types.builtin(sema::Builtin::void_);
    const QualType p = vp();
    const auto lang = ast::LangLinkage::c;
    const QualType destroy = world.types.pointer(world.types.function(v, std::span(&p, 1), false, lang));
    const QualType params[] = {p, p, destroy};
    return {int_(), declared("__cxa_allocate_exception", world.types.function(p, std::span(&size, 1), false, lang), true),
            declared("__cxa_throw", world.types.function(v, params, false, lang), true),
            declared("_ZTIi", world.types.builtin(sema::Builtin::char_), false)};
  }
  ir::CatchAllRuntime catch_runtime() {
    // Itanium EH ABI §2.5. The unwind header crosses the runtime boundary as a pointer.
    const auto lang = ast::LangLinkage::c;
    const QualType p = vp(), v = world.types.builtin(sema::Builtin::void_);
    return {declared("__cxa_begin_catch", world.types.function(p, std::span(&p, 1), false, lang), true),
            declared("__cxa_end_catch", world.types.function(v, {}, false, lang), true),
            resume()};
  }
  EntityId resume() {
    const QualType p = vp(), v = world.types.builtin(sema::Builtin::void_);
    return declared("_Unwind_Resume", world.types.function(v, std::span(&p, 1), false, ast::LangLinkage::c), true);
  }
};

// A function's static locals ([stmt.dcl]/3): each a variable of the unit, initialized with its
// constant if it has one, else zero; one initialized by code, or with a destructor, also gets a
// guard variable, and the function the runtime entry points its initialization calls.
template <class R>
void static_locals(sema::World& world, TranslationUnit& tu, FunctionDef& f,
                   std::span<const ast::LocalRec> locals, R& runtime) {
  ast::Interners& ins = world.ins;
  for (const ast::LocalRec& l : locals) {
    if (l.entity == EntityId::none || l.decl == ast::NodeId::none || !ins.ast.is<ast::StaticVar>(l.decl)) continue;
    // The function and its statics need ELF group selection (#422), or weak Mach-O statics (#124).
    f.coalesced = false;
    auto s = ins.ast.get<ast::StaticVar>(l.decl);
    bool code = s.destructor != EntityId::none || (s.value == ast::ConstId::none && s.init.id != ast::NodeId::none);
    const sema::EntityRec& r = world.entities.rec(l.entity);
    auto local = [&](EntityId e, QualType t, bool guard) {
      tu.entities.push_back({e, std::string(ins.names.spelling(r.name)), t, false, ast::LangLinkage::cxx, ast::Linkage::internal,
                             false, std::nullopt, EntityId::none, Special::none});
      tu.entities.back().local_function = f.entity;
      tu.entities.back().discriminator = r.ordinal;
      tu.entities.back().guard = guard;
    };
    local(l.entity, l.type, false);
    tu.variables.push_back({l.entity, l.type, s.value, std::nullopt, code});
    if (!code) continue;
    QualType gt = world.types.builtin(sema::Builtin::long_long);
    sema::EntityRec gr{sema::EntityKind::variable, r.name, gt, ast::LangLinkage::cxx, ast::Linkage::none, {}, l.entity};
    gr.head = ast::seed("ycxx.pipeline.static-guard");
    EntityId g = world.entities.entity(std::move(gr));
    local(g, gt, true);
    tu.variables.push_back({g, gt, ast::ConstId::none, std::nullopt, true});
    f.guards.push_back({l.entity, g});
    f.guard_acquire = runtime.acquire();
    f.guard_release = runtime.release();
    if (s.destructor != EntityId::none) {
      f.atexit = runtime.atexit();
      f.dso = runtime.dso();
    }
  }
}

// The unit's initialization functions, which startup runs before `main`: each
// dynamic initialization in definition order ([basic.start.dynamic]/1), each destructor registered
// with `__cxa_atexit` (Itanium C++ ABI §3.3.5). Named like Clang's, with internal linkage.
void init_function_def(parse::Program& program, sema::World& world, TranslationUnit& tu, EntityId fn, QualType fn_type, ast::NodeId root,
                       std::uint32_t item);
template <class Declare, class R>
void init_function(parse::Program& program, sema::World& world, TranslationUnit& tu, std::span<const Dynamic> dynamic,
                   Declare& declare, R& runtime, std::string_view source) {
  if (dynamic.empty()) return;
  ast::Interners& ins = world.ins;
  sema::Types& types = world.types;
  QualType void_ = types.builtin(sema::Builtin::void_);
  QualType vp = types.pointer(void_);
  auto entity = [&](sema::EntityKind kind, std::string_view n, QualType t, ast::LangLinkage lang, ast::Linkage linkage) {
    return world.entities.entity({kind, ins.names.intern(n), t, lang, linkage, {}, EntityId::none});
  };
  auto symbol = [&](EntityId e, QualType t) {
    ast::DeclSema d{};
    d.entity = e;
    d.type = t;
    declare(d, std::nullopt);
  };
  EntityId atexit = runtime.atexit();
  EntityId dso = runtime.dso();
  QualType fn_type = types.function(void_, {}, false, ast::LangLinkage::cxx);
  const std::string base = "__GLOBAL__sub_I_" + std::filesystem::path(source).filename().native_encoded_string();
  // One function per declaration, so that what fails points at it; startup runs them in order.
  for (std::size_t first = 0, n = 0; first < dynamic.size(); ++n) {
    std::size_t last = first;
    while (last < dynamic.size() && dynamic[last].item == dynamic[first].item) ++last;
    std::string fn_name = n == 0 ? base : std::format("{}.{}", base, n);
    EntityId fn = entity(sema::EntityKind::function, fn_name, fn_type, ast::LangLinkage::cxx, ast::Linkage::internal);
    symbol(fn, fn_type);
    tu.entities.back().asm_label = fn_name;
    ast::Builder b(ins);
    std::vector<ast::Child> stmts;
    for (const Dynamic& x : dynamic.subspan(first, last - first)) {
      ast::Child init{x.init == ast::NodeId::none ? ast::NodeId::none : b.adopt(x.init), 0};
      stmts.push_back({b.add(ast::StaticInit{x.variable, x.type, init, x.destructor, atexit, dso}, 0), 0});
    }
    ast::Child root = b.commit(ast::Child{b.add(ast::Compound{b.list(stmts)}, 0), 0});
    init_function_def(program, world, tu, fn, fn_type, root.id, dynamic[first].item);
    tu.init_functions.push_back({fn, entity(sema::EntityKind::variable, fn_name + ".ptr", vp, ast::LangLinkage::cxx, ast::Linkage::internal)});
    first = last;
  }
}

void init_function_def(parse::Program& program, sema::World& world, TranslationUnit& tu, EntityId fn, QualType fn_type, ast::NodeId root,
                       std::uint32_t item) {
  ast::Interners& ins = world.ins;
  ast::DeclSema d{};
  d.kind = ast::DeclKind::function;
  d.type = fn_type;
  d.linkage = ast::Linkage::internal;
  d.storage = ast::Storage::static_;
  d.access = ast::Access::public_;
  d.flags = sema::decl_flag::definition;
  d.attributes = ins.intern_seq(std::span<const ast::NodeId>{});
  d.constraint = ast::NodeId::none;
  d.constraints = ast::ConstId::none;
  d.contracts = ast::NodeId::none;
  d.params = ins.intern_seq(std::span<const ast::ParamSema>{});
  d.template_params = ins.intern_seq(std::span<const ast::TemplateParamSema>{});
  d.entity = fn;
  ast::BodySema body{root, ins.intern_seq(std::span<const ast::LocalRec>{}), ins.intern_seq(std::span<const ast::ExpansionRec>{}),
                     ins.intern_seq(std::span<const std::uint32_t>{})};
  body.closures = ins.intern_seq(std::span<const ast::ClosureDef>{});
  tu.functions.push_back({fn, d, body, program.id(item), 0, std::nullopt});
}

void block_virtual_emission(TranslationUnit& unit, parse::Program& program, const sema::ClassRec& record) {
  unit.virtual_emission_blocked = true;
  const auto member = std::ranges::find_if(record.members, &sema::MemberDeclRec::is_virtual);
  if (member != record.members.end())
    if (auto d = program.member_decl(member->entity)) unit.virtual_declaration = ast::Loc{d->item, 0};
}

bool root_destructor_body_supported(parse::Program& program, EntityId cls, const ast::BodySema& body) {
  auto& world = program.world();
  const auto& tree = world.ins.ast;
  if (body.tree == ast::NodeId::none || !tree.is<ast::Destructor>(body.tree) ||
      world.ins.seqs.size(body.locals) != 0 || world.ins.seqs.size(body.closures) != 0) return false;
  const auto destructor = tree.get<ast::Destructor>(body.tree);
  if (!tree.children(destructor.members).empty() || !tree.is<ast::Compound>(destructor.body.id)) return false;
  const auto statements = tree.children(tree.get<ast::Compound>(destructor.body.id).stmts);
  if (statements.size() != 1 || !tree.is<ast::ExprStmt>(statements.front().id)) return false;
  const auto expr = tree.get<ast::ExprStmt>(statements.front().id).expr.id;
  if (!tree.is<ast::Call>(expr)) return false;
  const auto call = tree.get<ast::Call>(expr);
  if (call.function == EntityId::none || !tree.is<ast::DeclRef>(call.callee.id)) return false;
  const auto callee = tree.get<ast::DeclRef>(call.callee.id);
  if (callee.entity != call.function || !program.peer_root_destructor_observer(call.function, cls)) return false;
  const auto arguments = tree.children(call.args);
  return arguments.size() == 1 && tree.is<ast::This>(arguments.front().id);
}

ast::BodySema deleting_root_destructor_body(sema::World& world, const FunctionDef& complete,
                                           const ast::DeclSema& deallocator) {
  ast::Builder b(world.ins);
  const auto prvalue = [](QualType type) { return ast::ExprInfo{type, ast::ValueCategory::prvalue, 0}; };
  const auto void_type = world.types.builtin(sema::Builtin::void_);
  const auto size_type = world.types.builtin(world.target.size_type);
  const ast::Child receiver{b.add(ast::This{prvalue(*complete.this_type)}, 0), 0};
  const ast::Child destroy{b.add(ast::MemberCall{prvalue(void_type), complete.entity, receiver, b.list({})}, 0), 0};
  const ast::Child address{b.add(ast::ImplicitConversion{prvalue(world.types.pointer(void_type)),
      ast::ConversionKind::pointer, EntityId::none, receiver}, 0), 0};
  // [Itanium ABI §2.4]: the certified fieldless root has only the primary vptr.
  const ast::Child size{b.add(ast::Literal{prvalue(size_type), world.values.integer(size_type, world.target.pointer_size)}, 0), 0};
  const ast::Child callee{b.add(ast::DeclRef{{deallocator.type, ast::ValueCategory::lvalue, 0}, deallocator.entity}, 0), 0};
  const ast::Child release{b.add(ast::Call{prvalue(void_type), deallocator.entity, callee, b.list({address, size})}, 0), 0};
  const ast::Child statements[] = {{b.add(ast::ExprStmt{destroy}, 0), 0}, {b.add(ast::ExprStmt{release}, 0), 0}};
  ast::BodySema body = complete.body;  // the certified definition has no parameters, locals or closures
  body.tree = b.commit(ast::Child{b.add(ast::Compound{b.list(statements)}, 0), 0}).id;
  return body;
}

bool unwind_scalar(const sema::Types& types, QualType type, bool allow_void = false) {
  const auto t = types.rec(type);
  if (t.kind == sema::TypeKind::pointer || t.kind == sema::TypeKind::enum_) return true;
  if (t.kind != sema::TypeKind::builtin) return false;
  using B = ast::Builtin;
  return ast::is_integral(t.builtin) || t.builtin == B::float_ || t.builtin == B::double_ ||
         t.builtin == B::nullptr_t || (allow_void && t.builtin == B::void_);
}

bool unwind_scalar_parameters(const sema::Types& types, const sema::TypeRec& f) {
  return f.kind == sema::TypeKind::function && !f.variadic && f.fn_cv == sema::Cv::none &&
         f.fn_ref == ast::RefQual::none &&
         std::ranges::all_of(f.params, [&](QualType t) { return unwind_scalar(types, t); });
}

bool unwind_scalar_prototype(const sema::Types& types, QualType type) {
  const auto f = types.rec(type);
  return unwind_scalar_parameters(types, f) && unwind_scalar(types, f.inner, true);
}

bool unwind_indirect_callee(const sema::Types& types, const ast::Ast& tree, ast::NodeId callee) {
  return ast::dispatch<ast::Nodes>(ast::kind_of(callee), [&]<class T>(std::type_identity<T>) {
    if constexpr (requires(T n) { n.info; }) {
      const auto info = tree.get<T>(callee).info;
      const auto type = types.rec(info.type);
      return info.category == ast::ValueCategory::prvalue && type.kind == sema::TypeKind::pointer &&
             unwind_scalar_prototype(types, type.inner);
    } else {
      return false;
    }
  });
}

// Every subobject of this aggregate can start and end its lifetime without a cleanup.
bool unwind_scalar_aggregate(parse::Program& program, QualType type) {
  const auto& types = program.world().types;
  if (!types.is_class(type)) return false;
  const auto* record = program.classes().find(types.class_of(type));
  return record && record->defined && !record->user_ctors && !record->closure && !record->union_ &&
         !record->polymorphic && record->bases.empty() && record->destructor_usable &&
         record->destructor == EntityId::none &&
         std::ranges::all_of(record->fields, [&](const sema::FieldRec& field) {
           return unwind_scalar(types, field.type);
         });
}

// The result has no exceptional cleanup even if its initialization has started. Native lowering
// still checks the target's result classification and transports the result as usual.
bool unwind_aggregate_result(parse::Program& program, QualType type) {
  if (!unwind_scalar_aggregate(program, type)) return false;
  const auto cls = program.world().types.class_of(type);
  const auto& record = program.classes().rec(cls);
  return !record.fields.empty() && !record.unapplied_layout &&
         program.classes().trivial_for_calls(cls, ast::CallTriviality::itanium) &&
         std::ranges::none_of(record.fields, [](const sema::FieldRec& field) {
           return field.bit_width || field.no_unique_address;
         });
}

// Prove a subset of the typed tree. Static initialization, calls with separately analysed
// default arguments, and any unfamiliar operation need their own proof.
using UnwindNodes = ast::closed_list<
    ast::Compound, ast::Return, ast::ExprStmt, ast::If, ast::While, ast::Do, ast::For, ast::Break,
    ast::Continue, ast::DeclStmt, ast::LocalVar, ast::CondDecl, ast::Switch, ast::Case, ast::Default,
    ast::Label, ast::Goto, ast::Literal, ast::DeclRef, ast::LocalRef, ast::Paren, ast::Unary,
    ast::Binary, ast::OrderedBinary, ast::Assign, ast::Conditional, ast::Subscript, ast::Call, ast::Member,
    ast::NewArray, ast::DeleteArray, ast::ImplicitConversion, ast::InitList, ast::DefaultMemberInit,
    ast::Materialize, ast::This, ast::ValueInit, ast::DefaultInit, ast::Throw, ast::DynamicCastVoid,
    ast::DynamicCastPointer>;
using UnwindAggregateNodes = ast::closed_list<ast::InitList, ast::LocalRef, ast::Materialize, ast::Paren>;

enum class UnwindContext { function, cleanup_initializer, default_member };

bool unwind_body_node(parse::Program& program, ast::NodeId node, bool& has_throw,
                      UnwindContext context = UnwindContext::function) {
  if (node == ast::NodeId::none) return true;
  const bool initializer = context == UnwindContext::cleanup_initializer;
  auto& world = program.world();
  const auto& tree = world.ins.ast;
  return ast::dispatch<ast::Nodes>(ast::kind_of(node), [&]<class T>(std::type_identity<T>) {
    if constexpr (!UnwindNodes::contains<T>) {
      return false;
    } else {
      if constexpr (std::is_same_v<T, ast::Call> || std::is_same_v<T, ast::NewArray> ||
                    std::is_same_v<T, ast::DeleteArray> || std::is_same_v<T, ast::Throw>)
        if (initializer) return false;
      const T n = tree.get<T>(node);
      if constexpr (std::is_same_v<T, ast::DeclRef>) {
        if (n.reference || (!unwind_scalar(world.types, n.info.type) &&
                            !unwind_scalar_prototype(world.types, n.info.type))) return false;
      } else if constexpr (requires { n.info; }) {
        if (!unwind_scalar(world.types, n.info.type, true)) {
          if constexpr (!UnwindAggregateNodes::contains<T>) return false;
          else if (initializer || !unwind_scalar_aggregate(program, n.info.type)) return false;
        }
      }
      if constexpr (std::is_same_v<T, ast::Return>) {
        // A named aggregate result can copy its source without calling a constructor.
        if (n.value.id != ast::NodeId::none && tree.is<ast::ImplicitConversion>(n.value.id)) {
          const auto copy = tree.get<ast::ImplicitConversion>(n.value.id);
          if (world.types.is_class(copy.info.type)) {
            if (initializer || copy.function != EntityId::none || copy.conversion != ast::ConversionKind::lvalue_to_rvalue ||
                !unwind_aggregate_result(program, copy.info.type)) return false;
            ast::NodeId source = copy.operand.id;
            if (source != ast::NodeId::none && tree.is<ast::ImplicitConversion>(source)) {
              const auto cast = tree.get<ast::ImplicitConversion>(source);
              // [stmt.return]: implicit move's qualification changes category but still names
              // the same object. Its operand must pass the ordinary lifetime proof.
              if (cast.function == EntityId::none && cast.conversion == ast::ConversionKind::qualification &&
                  cast.info.category == ast::ValueCategory::xvalue &&
                  sema::Types::unqualified(world.types.canonical(cast.info.type)) ==
                      sema::Types::unqualified(world.types.canonical(copy.info.type)))
                source = cast.operand.id;
            }
            return unwind_body_node(program, source, has_throw, context);
          }
        }
      } else if constexpr (std::is_same_v<T, ast::LocalVar>) {
        if (n.destructor != EntityId::none) return false;
      } else if constexpr (std::is_same_v<T, ast::Member>) {
        // A selected default can read another field directly from its construction object.
        if (initializer || n.reference || (n.arrow && !tree.is<ast::This>(n.object.id))) return false;
      } else if constexpr (std::is_same_v<T, ast::DefaultMemberInit>) {
        if (initializer) return false;
      } else if constexpr (std::is_same_v<T, ast::This>) {
        if (context != UnwindContext::default_member) return false;
      } else if constexpr (std::is_same_v<T, ast::Materialize>) {
        if (!world.types.is_class(n.info.type) || n.destructor != EntityId::none || n.extended) return false;
      } else if constexpr (std::is_same_v<T, ast::Call>) {
        if (n.callee.id == ast::NodeId::none) return false;
        if (n.function == EntityId::none) {
          // The pointer type supplies the prototype. Its expression and the arguments still
          // need the same recursive lifetime proof below.
          if (!unwind_indirect_callee(world.types, tree, n.callee.id)) return false;
        } else {
          if (!tree.is<ast::DeclRef>(n.callee.id)) return false;
          const auto callee = tree.get<ast::DeclRef>(n.callee.id);
          const auto& f = world.entities.rec(n.function);
          if (callee.entity != n.function || f.kind != sema::EntityKind::function || f.templ != EntityId::none ||
              !program.is_namespace(f.scope) || !unwind_scalar_prototype(world.types, callee.info.type)) return false;
        }
      } else if constexpr (std::is_same_v<T, ast::NewArray>) {
        // The fixed-size allocation may throw before any element lifetime starts.
        if (n.allocation.id == ast::NodeId::none || n.element_init.id == ast::NodeId::none ||
            !tree.is<ast::Call>(n.allocation.id) || !tree.is<ast::DefaultInit>(n.element_init.id)) return false;
        const auto allocate = world.allocation_function(sema::AllocationFunction::allocate, /*array=*/true);
        if (tree.get<ast::Call>(n.allocation.id).function != allocate.entity) return false;
      } else if constexpr (std::is_same_v<T, ast::DeleteArray>) {
        // The typed scalar array has no element cleanup. The operand still has to pass the
        // recursive proof, and the only call made by deletion is the usual noexcept deallocator.
        const auto release = world.allocation_function(sema::AllocationFunction::deallocate, /*array=*/true);
        if (n.operand.id == ast::NodeId::none || n.function != release.entity) return false;
      } else if constexpr (std::is_same_v<T, ast::ImplicitConversion>) {
        if (n.function != EntityId::none || n.conversion == ast::ConversionKind::user_defined) return false;
      } else if constexpr (std::is_same_v<T, ast::Throw>) {
        if (n.operand.id == ast::NodeId::none) return false;
        has_throw = true;  // the runtime-reachable operation checks the exact exception type
      }
      const auto children = std::is_same_v<T, ast::DefaultMemberInit> ? UnwindContext::default_member : context;
      bool safe = true;
      ast::for_each_ref(n, [&]<class H>(const H& h) {
        if constexpr (std::is_same_v<H, ast::NodeId>) safe = safe && unwind_body_node(program, h, has_throw, children);
        else if constexpr (std::is_same_v<H, ast::ChildList>)
          for (ast::Child c : tree.children(h)) safe = safe && unwind_body_node(program, c.id, has_throw, children);
      });
      return safe;
    }
  });
}

UnwindEligibility cleanup_free_definition(parse::Program& program, const ast::DeclSema& d, const ast::BodySema& body) {
  using enum UnwindEligibility;
  auto& world = program.world();
  const auto prototype = world.types.rec(d.type);
  if (!unwind_scalar_parameters(world.types, prototype) || prototype.fn_noexcept ||
      (!unwind_scalar(world.types, prototype.inner, true) && !unwind_aggregate_result(program, prototype.inner)) ||
      d.contracts != ast::NodeId::none || body.tree == ast::NodeId::none || world.ins.seqs.size(body.closures) != 0)
    return none;
  const auto& entity = world.entities.rec(d.entity);
  if (entity.kind != sema::EntityKind::function || entity.templ != EntityId::none || !program.is_namespace(entity.scope) ||
      (entity.linkage != ast::Linkage::external && entity.linkage != ast::Linkage::internal) ||
      sema::is_allocation_name(world.ins.names.spelling(entity.name))) return none;
  if (!std::ranges::all_of(world.ins.seqs.elements(body.locals), [&](const ast::LocalRec& local) {
        return local.entity == EntityId::none &&
               (unwind_scalar(world.types, local.type) || unwind_scalar_aggregate(program, local.type)) &&
               (local.decl == ast::NodeId::none || world.ins.ast.is<ast::LocalVar>(local.decl));
      })) return none;
  bool has_throw = false;
  if (!unwind_body_node(program, body.tree, has_throw)) return none;
  return has_throw ? cleanup_free_with_throw : cleanup_free;
}

bool hosted_void_definition(parse::Program& program, const ast::DeclSema& d, const ast::BodySema& body) {
  auto& world = program.world();
  const auto& tree = world.ins.ast;
  const auto& definition = world.entities.rec(d.entity);
  const auto fn = world.types.rec(d.type);
  return definition.kind == sema::EntityKind::function && definition.templ == EntityId::none &&
         program.is_namespace(definition.scope) && definition.linkage == ast::Linkage::external &&
         unwind_scalar_prototype(world.types, d.type) && !fn.fn_noexcept &&
         world.types.is(fn.inner, sema::Builtin::void_) && d.contracts == ast::NodeId::none &&
         !(d.flags & (sema::decl_flag::constexpr_ | sema::decl_flag::consteval_)) &&
         world.ins.seqs.size(body.closures) == 0 && body.tree != ast::NodeId::none && tree.is<ast::Compound>(body.tree);
}

EntityId hosted_external_call(parse::Program& program, ast::NodeId at, sema::Builtin result) {
  const auto& world = program.world();
  const auto& tree = world.ins.ast;
  if (at == ast::NodeId::none || !tree.is<ast::Call>(at)) return EntityId::none;
  const auto call = tree.get<ast::Call>(at);
  if (call.function == EntityId::none || call.callee.id == ast::NodeId::none ||
      !tree.is<ast::DeclRef>(call.callee.id) || !tree.children(call.args).empty()) return EntityId::none;
  const auto& entity = world.entities.rec(call.function);
  const auto callee = tree.get<ast::DeclRef>(call.callee.id);
  const auto prototype = world.types.rec(callee.info.type);
  if (callee.reference || callee.entity != call.function || entity.kind != sema::EntityKind::function ||
      entity.templ != EntityId::none || !program.is_namespace(entity.scope) || entity.linkage != ast::Linkage::external ||
      !unwind_scalar_prototype(world.types, callee.info.type) || prototype.fn_noexcept ||
      !world.types.is(prototype.inner, result) || !prototype.params.empty()) return EntityId::none;
  return call.function;
}

std::optional<ast::LocalVar> single_local(const ast::Ast& tree, ast::NodeId statement) {
  if (!tree.is<ast::DeclStmt>(statement)) return std::nullopt;
  const auto declarations = tree.children(tree.get<ast::DeclStmt>(statement).decls);
  if (declarations.size() != 1 || !tree.is<ast::LocalVar>(declarations.front().id)) return std::nullopt;
  return tree.get<ast::LocalVar>(declarations.front().id);
}

// One aggregate local followed by an external void call. The initializer has no calls,
// throwing operations, class temporaries, or partially constructed class subobjects.
ast::NodeId hosted_cleanup_call(parse::Program& program, const ast::DeclSema& d, const ast::BodySema& body) {
  if (!hosted_void_definition(program, d, body)) return ast::NodeId::none;
  const auto& world = program.world();
  const auto& tree = world.ins.ast;
  const auto statements = tree.children(tree.get<ast::Compound>(body.tree).stmts);
  if (statements.size() != 2 || !tree.is<ast::ExprStmt>(statements[1].id)) return ast::NodeId::none;
  const auto local = single_local(tree, statements.front().id);
  if (!local || local->destructor == EntityId::none || local->init.id == ast::NodeId::none ||
      !tree.is<ast::InitList>(local->init.id)) return ast::NodeId::none;
  const auto type = world.ins.seqs.at(body.locals, local->local).type;
  if (!world.types.is_class(type)) return ast::NodeId::none;
  const auto& record = program.classes().rec(world.types.class_of(type));
  if (!record.destructor_noexcept.value_or(false) || record.virtual_destructor) return ast::NodeId::none;
  bool has_throw = false;
  for (auto element : tree.children(tree.get<ast::InitList>(local->init.id).elems))
    if (!unwind_body_node(program, element.id, has_throw, UnwindContext::cleanup_initializer)) return ast::NodeId::none;
  const auto call = tree.get<ast::ExprStmt>(statements[1].id).expr.id;
  return hosted_external_call(program, call, sema::Builtin::void_) != EntityId::none ? call : ast::NodeId::none;
}

// The selected variant needs cleanup only after its scalar aggregate initialization completed.
// A direct external int() in the initializer can throw before it completes, with no cleanup.
void hosted_union_constructor(parse::Program& program, FunctionDef& f) {
  const auto& world = program.world();
  const auto& tree = world.ins.ast;
  const auto& entity = world.entities.rec(f.entity);
  const auto& body = f.body;
  const auto fn = world.types.rec(f.decl.type);
  if (entity.kind != sema::EntityKind::constructor || fn.fn_noexcept || body.tree == ast::NodeId::none ||
      !tree.is<ast::Compound>(body.tree) || !program.classes().rec(entity.scope).union_) return;
  const auto steps = tree.children(tree.get<ast::Compound>(body.tree).stmts);
  for (const auto step : steps) {
    if (!tree.is<ast::MemberInit>(step.id)) continue;
    const auto member = tree.get<ast::MemberInit>(step.id);
    f.constructor_cleanup = program.classes().destructor_of(member.type);
    if (f.constructor_cleanup != EntityId::none) break;
  }
  if (f.constructor_cleanup == EntityId::none || steps.size() != 2 || !tree.is<ast::MemberInit>(steps[0].id) ||
      !tree.is<ast::Compound>(steps[1].id) || entity.templ != EntityId::none || fn.variadic || !fn.params.empty() ||
      f.decl.contracts != ast::NodeId::none || (f.decl.flags & (sema::decl_flag::constexpr_ | sema::decl_flag::consteval_)) ||
      world.ins.seqs.size(body.closures) != 0 || world.ins.seqs.size(body.locals) != 0) return;
  const auto member = tree.get<ast::MemberInit>(steps[0].id);
  if (member.field == ~0u || member.init.id == ast::NodeId::none || !tree.is<ast::InitList>(member.init.id) ||
      !world.types.is_class(member.type)) return;
  const auto& record = program.classes().rec(world.types.class_of(member.type));
  if (!record.destructor_noexcept.value_or(false) || record.virtual_destructor || record.union_ || !record.bases.empty() ||
      !std::ranges::all_of(record.fields, [&](const sema::FieldRec& field) {
        return !field.dmi && unwind_scalar(world.types, field.type);
      })) return;

  EntityId initializer = EntityId::none;
  bool has_throw = false;
  for (const auto element : tree.children(tree.get<ast::InitList>(member.init.id).elems)) {
    if (unwind_body_node(program, element.id, has_throw, UnwindContext::cleanup_initializer)) continue;
    if (initializer != EntityId::none) return;
    initializer = hosted_external_call(program, element.id, sema::Builtin::int_);
    if (initializer == EntityId::none) return;
  }
  const auto statements = tree.children(tree.get<ast::Compound>(steps[1].id).stmts);
  if (statements.size() != 1 || !tree.is<ast::ExprStmt>(statements.front().id)) return;
  const auto call = tree.get<ast::ExprStmt>(statements.front().id).expr.id;
  if (hosted_external_call(program, call, sema::Builtin::void_) == EntityId::none) return;
  f.cleanup_call = call;
  f.constructor_initializer_callee = initializer;
  f.unwind = UnwindEligibility::hosted_cleanup;
}

// The automatic object's lifetime starts only after its constructor returns. There is no local
// to clean up if that call throws, so this caller only needs a frame.
EntityId hosted_union_constructor_caller(parse::Program& program, const ast::DeclSema& d, const ast::BodySema& body) {
  if (!hosted_void_definition(program, d, body)) return EntityId::none;
  const auto& world = program.world();
  const auto& tree = world.ins.ast;
  const auto statements = tree.children(tree.get<ast::Compound>(body.tree).stmts);
  if (statements.size() != 1) return EntityId::none;
  const auto local = single_local(tree, statements.front().id);
  if (!local || local->destructor == EntityId::none || local->init.id == ast::NodeId::none ||
      !tree.is<ast::CtorCall>(local->init.id)) return EntityId::none;
  const auto type = world.ins.seqs.at(body.locals, local->local).type;
  if (!world.types.is_class(type)) return EntityId::none;
  const auto& record = program.classes().rec(world.types.class_of(type));
  const auto init = tree.get<ast::CtorCall>(local->init.id);
  if (!record.union_ || !record.destructor_noexcept.value_or(false) || init.zero_first ||
      !tree.children(init.args).empty()) return EntityId::none;
  return init.ctor;
}

// Prove the whole handler occurrence while it still has C++ types and lifetime information.
// Neither a call-name spelling nor an untyped shape certifies the callee.
std::optional<HostedCatch> hosted_catch(parse::Program& program, const ast::DeclSema& d, const ast::BodySema& body) {
  const auto& world = program.world();
  const auto& tree = world.ins.ast;
  auto sole_statement = [&](ast::NodeId id) {
    if (id == ast::NodeId::none || !tree.is<ast::Compound>(id)) return ast::NodeId::none;
    const auto stmts = tree.children(tree.get<ast::Compound>(id).stmts);
    return stmts.size() == 1 ? stmts.front().id : ast::NodeId::none;
  };
  const auto trial = sole_statement(body.tree);
  if (trial == ast::NodeId::none || !tree.is<ast::TryBlock>(trial)) return std::nullopt;
  auto prototype = [&](QualType q) {
    const auto f = world.types.rec(q);
    return f.kind == sema::TypeKind::function && !f.variadic && !f.fn_noexcept &&
           f.fn_cv == sema::Cv::none && f.fn_ref == ast::RefQual::none &&
           world.types.is(f.inner, sema::Builtin::int_) && f.params.size() == 1 &&
           world.types.is(f.params.front(), sema::Builtin::int_);
  };
  auto namespace_function = [&](EntityId id) {
    const auto& e = world.entities.rec(id);
    return e.kind == sema::EntityKind::function && e.templ == EntityId::none && program.is_namespace(e.scope) &&
           (e.linkage == ast::Linkage::external || e.linkage == ast::Linkage::internal);
  };
  if (!namespace_function(d.entity) || !prototype(d.type) ||
      (d.flags & (sema::decl_flag::constexpr_ | sema::decl_flag::consteval_)) ||
      d.contracts != ast::NodeId::none || world.ins.seqs.size(body.closures) != 0)
    return std::nullopt;
  const auto local_count = world.ins.seqs.size(body.locals);
  if (local_count < 1 || local_count > 2) return std::nullopt;
  const auto local = world.ins.seqs.at(body.locals, 0);
  if (local.entity != EntityId::none || local.decl != ast::NodeId::none ||
      sema::Types::is_volatile(world.types.canonical(local.type)))
    return std::nullopt;

  auto return_value = [&](ast::NodeId id) {
    const auto statement = sole_statement(id);
    return statement != ast::NodeId::none && tree.is<ast::Return>(statement)
        ? tree.get<ast::Return>(statement).value.id : ast::NodeId::none;
  };
  auto reads_local = [&](ast::NodeId value, std::uint32_t index) {
    if (value == ast::NodeId::none || !tree.is<ast::ImplicitConversion>(value)) return false;
    const auto read = tree.get<ast::ImplicitConversion>(value);
    return read.conversion == ast::ConversionKind::lvalue_to_rvalue && read.operand.id != ast::NodeId::none &&
           tree.is<ast::LocalRef>(read.operand.id) && tree.get<ast::LocalRef>(read.operand.id).local == index;
  };
  const auto block = tree.get<ast::TryBlock>(trial);
  const auto handlers = tree.children(block.handlers);
  if (handlers.size() != 1 || !tree.is<ast::Handler>(handlers.front().id)) return std::nullopt;
  const auto handler = tree.get<ast::Handler>(handlers.front().id);
  const auto caught_result = return_value(handler.body.id);
  std::optional<std::uint32_t> int_binding;
  if (handler.kind == ast::HandlerKind::catch_all) {
    if (local_count != 1 || caught_result == ast::NodeId::none || !tree.is<ast::Literal>(caught_result) ||
        !world.types.is(tree.get<ast::Literal>(caught_result).info.type, sema::Builtin::int_))
      return std::nullopt;
  } else if (handler.kind == ast::HandlerKind::typed) {
    const auto type = world.types.canonical(handler.type);
    if (local_count != 2 || handler.local != 1 || !world.types.is(type, sema::Builtin::int_) ||
        sema::Types::is_volatile(type) || caught_result == ast::NodeId::none)
      return std::nullopt;
    auto value = caught_result;
    if (tree.is<ast::ImplicitConversion>(value)) {
      // A sugared int alias can add an identity integral conversion to the lvalue read.
      const auto conversion = tree.get<ast::ImplicitConversion>(value);
      const auto operand = conversion.operand.id;
      if (conversion.conversion == ast::ConversionKind::integral && world.types.is(conversion.info.type, sema::Builtin::int_) &&
          operand != ast::NodeId::none && tree.is<ast::ImplicitConversion>(operand) &&
          world.types.is(tree.get<ast::ImplicitConversion>(operand).info.type, sema::Builtin::int_))
        value = operand;
    }
    if (!reads_local(value, handler.local)) return std::nullopt;
    int_binding = handler.local;
  } else {
    return std::nullopt;
  }

  const auto value = return_value(block.body.id);
  if (value == ast::NodeId::none || !tree.is<ast::Call>(value)) return std::nullopt;
  const auto call = tree.get<ast::Call>(value);
  if (call.function == EntityId::none || call.callee.id == ast::NodeId::none ||
      !tree.is<ast::DeclRef>(call.callee.id) || !namespace_function(call.function) ||
      world.entities.rec(call.function).linkage != ast::Linkage::external)
    return std::nullopt;
  const auto callee = tree.get<ast::DeclRef>(call.callee.id);
  if (callee.reference || callee.entity != call.function || !prototype(callee.info.type)) return std::nullopt;
  const auto args = tree.children(call.args);
  if (args.size() != 1 || !reads_local(args.front().id, 0)) return std::nullopt;
  return HostedCatch{call.function, int_binding};
}

bool names_unnamed_namespace(parse::Program& program, QualType type);
bool names_unnamed_namespace(parse::Program& program, EntityId entity);

bool names_unnamed_namespace(parse::Program& program, const ast::TemplateArg& arg) {
  switch (arg.kind) {
  case ast::TemplateArgKind::type: return names_unnamed_namespace(program, arg.type);
  case ast::TemplateArgKind::template_: return names_unnamed_namespace(program, arg.templ);
  case ast::TemplateArgKind::pack:
    return std::ranges::any_of(program.world().ins.seqs.elements(arg.elems), [&](const ast::TemplateArg& element) {
      return names_unnamed_namespace(program, element);
    });
  default: return false;
  }
}

// Look through nominal owners and encoded template arguments, without inspecting class definitions.
bool names_unnamed_namespace(parse::Program& program, EntityId entity) {
  auto& entities = program.world().entities;
  while (entity != EntityId::none) {
    if (program.is_namespace(entity)) return parse::Program::unnamed(program.namespace_path(entity));
    const auto& r = entities.rec(entity);
    if (r.kind != sema::EntityKind::class_ && r.kind != sema::EntityKind::enum_ &&
        r.kind != sema::EntityKind::class_template && r.kind != sema::EntityKind::alias_template)
      return false;
    if (std::ranges::any_of(r.args, [&](const ast::TemplateArg& arg) { return names_unnamed_namespace(program, arg); }))
      return true;
    entity = r.scope;
  }
  return false;
}

bool names_unnamed_namespace(parse::Program& program, QualType type) {
  const auto r = program.world().types.rec(type);
  switch (r.kind) {
  case sema::TypeKind::class_:
  case sema::TypeKind::enum_: return names_unnamed_namespace(program, r.entity);
  case sema::TypeKind::pointer:
  case sema::TypeKind::reference:
  case sema::TypeKind::array: return names_unnamed_namespace(program, r.inner);
  case sema::TypeKind::member_pointer:
    return names_unnamed_namespace(program, r.member_owner) || names_unnamed_namespace(program, r.inner);
  case sema::TypeKind::function:
    return names_unnamed_namespace(program, r.inner) ||
           std::ranges::any_of(r.params, [&](QualType param) { return names_unnamed_namespace(program, param); });
  default: return false;
  }
}

// Native compilation may consume peer-owned objects, including one non-key definition.
bool native_virtual_layouts(TranslationUnit& tu, parse::Program& program, sema::World& world,
                            const std::set<EntityId>& uncertain_layouts) {
  const auto& classes = program.classes();
  std::set<EntityId> automatic_primary_bases;
  if (classes.has_virtual_layouts()) {
    for (EntityId c : classes.classes()) {
      const auto& record = classes.rec(c);
      if (const EntityId base = record.peer_automatic_primary_base; base != EntityId::none) {
        // Check both classes after the last declaration, even when nothing constructs the derived class.
        const auto key = program.automatic_derived_vtable_key(c);
        if (uncertain_layouts.contains(c) || uncertain_layouts.contains(base) || !key ||
            (*key != EntityId::none && tu.owned_virtual_table)) {
          block_virtual_emission(tu, program, record);
          return false;
        }
        if (*key != EntityId::none)
          tu.owned_virtual_table = OwnedVirtualTable{sema::virtual_table_entity(world, c), {*key}, base};
        automatic_primary_bases.insert(base);
      }
    }
  }
  EntityId defined_nonkey = EntityId::none;
  auto peer_member_declaration = [&](EntityId f) -> std::optional<ast::DeclSema> {
    // Inspect all definitions at the end of the unit, including a key written after the
    // supported definition. A derived consumer may ask about the same root again.
    if (program.function_defined_before(f, program.end())) {
      const auto def = program.declaration(f, program.end());
      const auto* owner = classes.find(world.entities.rec(f).scope);
      if (!def || !def->is_virtual || !(def->flags & sema::decl_flag::definition) ||
          (def->flags & (sema::decl_flag::inline_ | sema::decl_flag::deleted)) ||
          !owner || !sema::native_virtual_nonkey_member(*owner, f) ||
          (defined_nonkey != EntityId::none && defined_nonkey != f)) return std::nullopt;
      defined_nonkey = f;
    }
    const auto d = program.member_decl(f);
    return d ? program.decl_sema(*d) : std::nullopt;
  };
  for (EntityId c : classes.classes()) {
    const auto& record = classes.rec(c);
    if (!record.layout_requires_virtual_support) continue;
    if (record.peer_automatic_primary_base != EntityId::none || automatic_primary_bases.contains(c)) continue;
    if (!uncertain_layouts.contains(c) && program.peer_root_destructor_producer(c)) continue;
    // Check the full unit even if a pure call was admitted before a later redeclaration.
    if (!uncertain_layouts.contains(c) && program.peer_pure_dispatch(c)) continue;
    // The first slot is the key function of the admitted root. Classify its ownership using
    // the complete unit, even when no function uses the class or constructs an object here.
    if (const auto slots = sema::native_virtual_table_slots(record);
        slots && program.function_defined_before(slots->front(), program.end())) {
      // An elaborated declaration may introduce the class with namespace attributes. A friend
      // may also redeclare either slot with its own attributes, including after the key.
      if (!tu.owned_virtual_table && !program.function_defined_before(slots->back(), program.end()) &&
          !program.has_unapplied_declaration_attributes(c) &&
          std::ranges::none_of(*slots, [&](EntityId f) { return program.has_unapplied_declaration_attributes(f); })) {
        tu.owned_virtual_table = OwnedVirtualTable{sema::virtual_table_entity(world, c),
                                                  {slots->begin(), slots->end()}};
        continue;
      }
    } else if (sema::native_virtual_consumer(record, classes, world.types, peer_member_declaration)) {
      continue;
    }
    block_virtual_emission(tu, program, record);
    return false;
  }
  return true;
}

// The definitions and declarations of the global namespace, in item order.
TranslationUnit unit_of(parse::Program& program, sema::World& world, const items::Items& items, std::string name,
                        const std::set<EntityId>& uncertain_layouts) {
  ast::Interners& ins = world.ins;
  TranslationUnit tu;
  tu.name = std::move(name);
  const auto& classes = program.classes();
  const bool has_typed_cast = program.has_typed_dynamic_cast();
  if (!has_typed_cast && !native_virtual_layouts(tu, program, world, uncertain_layouts)) return tu;
  const bool linux = world.target.triple == ast::x86_64_linux.triple;
  const bool exceptions_enabled = program.exceptions_enabled();
  std::set<std::uint32_t> seen;
  std::set<EntityId> inline_declarations;
  std::set<EntityId> noexcept_declarations;
  std::set<EntityId> hidden_declarations;
  std::set<EntityId> weak_declarations;
  using Visibility = ast::GnuVisibility;
  struct FunctionVisibility {
    Visibility written = Visibility::none;
    bool namespace_hidden = false;
    bool namespace_default = false;
  };
  std::map<EntityId, FunctionVisibility> function_visibility;
  std::vector<Dynamic> dynamic;
  // The type from the declaration, not the entity's record: a variable's type is not part of its
  // identity, so a record made before an edit may hold the old one (#115).
  // Whether a class is local to the unit ([basic.link]/4): it has internal linkage, as a closure
  // type in a function that is not inline does, or is a specialization for such a type.
  std::function<bool(QualType)> internal_type;
  std::function<bool(EntityId)> internal_class = [&](EntityId c) {
    const sema::EntityRec& r = world.entities.rec(c);
    if (r.linkage == ast::Linkage::internal) return true;
    for (const ast::TemplateArg& a : r.args)
      if (a.kind == ast::TemplateArgKind::type && internal_type(a.type)) return true;
    return false;
  };
  internal_type = [&](QualType t) {
    sema::TypeRec r = world.types.rec(t);
    if (r.kind == sema::TypeKind::class_) return internal_class(r.entity);
    if (r.kind == sema::TypeKind::pointer || r.kind == sema::TypeKind::reference || r.kind == sema::TypeKind::array)
      return internal_type(r.inner);
    return false;
  };
  // An ordinary in-class static constexpr member is inline ([dcl.constexpr]). An owner or
  // a declared type in an unnamed namespace gives the member's symbol TU-local binding.
  auto constexpr_member_symbol_linkage = [&](const ast::DeclSema& d) -> std::optional<ast::Linkage> {
    if (d.kind != ast::DeclKind::variable || d.storage != ast::Storage::static_ ||
        !(d.flags & sema::decl_flag::member) || !(d.flags & sema::decl_flag::constexpr_))
      return {};
    const auto& member = world.entities.rec(d.entity);
    const auto& owner = world.entities.rec(member.scope);
    if (owner.kind != sema::EntityKind::class_ || owner.templ != EntityId::none || !program.is_namespace(owner.scope))
      return {};
    if (names_unnamed_namespace(program, member.scope) || names_unnamed_namespace(program, d.type))
      return ast::Linkage::internal;
    return owner.linkage;
  };
  auto declare = [&](const ast::DeclSema& d, std::optional<ast::Loc> at) {
    EntityId e = d.entity;
    if (e == EntityId::none) return;
    // Any reachable declaration can mark the entity inline ([dcl.inline]).
    if ((d.flags & sema::decl_flag::inline_) && (d.kind == ast::DeclKind::function || d.kind == ast::DeclKind::variable))
      inline_declarations.insert(e);
    if (d.kind == ast::DeclKind::function && d.has_written_noexcept) noexcept_declarations.insert(e);
    if (d.hidden_visibility) hidden_declarations.insert(e);
    if (linux && d.explicit_weak) weak_declarations.insert(e);
    if (linux && d.kind == ast::DeclKind::function &&
        (d.hidden_visibility || d.explicit_default_visibility || d.namespace_visibility != Visibility::none)) {
      auto& visibility = function_visibility[e];
      if (visibility.written == Visibility::none) {
        if (d.hidden_visibility) visibility.written = Visibility::hidden;
        else if (d.explicit_default_visibility) visibility.written = Visibility::default_;
      }
      visibility.namespace_hidden |= d.namespace_visibility == Visibility::hidden;
      visibility.namespace_default |= d.namespace_visibility == Visibility::default_;
    }
    if (!seen.insert(std::to_underlying(e)).second) return;
    const sema::EntityRec& r = world.entities.rec(e);
    if (r.kind == sema::EntityKind::class_ || r.kind == sema::EntityKind::data_member || r.kind == sema::EntityKind::type_alias ||
        r.kind == sema::EntityKind::enum_ || r.kind == sema::EntityKind::enumerator ||
        r.kind == sema::EntityKind::function_template || r.kind == sema::EntityKind::variable_template ||
        r.kind == sema::EntityKind::alias_template || r.kind == sema::EntityKind::concept_)
      return;  // no symbol: a template's specializations have them
    const bool table = r.kind == sema::EntityKind::virtual_table;
    const bool rtti = r.kind == sema::EntityKind::type_info;
    bool fn = r.kind != sema::EntityKind::variable && !table && !rtti;
    std::string id(ins.names.spelling(r.name));
    if (fn && id == "main" && r.scope == EntityId::none) tu.main = e;  // [basic.start.main]: the global one
    // A member of a class local to the unit is local too.
    ast::Linkage linkage = constexpr_member_symbol_linkage(d).value_or(r.linkage);
    for (EntityId sc = r.scope; linkage == ast::Linkage::external && sc != EntityId::none; sc = world.entities.rec(sc).scope)
      if (world.entities.rec(sc).kind == sema::EntityKind::class_ && internal_class(sc)) linkage = ast::Linkage::internal;
    tu.entities.push_back({e, std::move(id), d.type, fn, r.lang, linkage, false, at, r.scope,
                           table ? Special::virtual_table
                           : rtti ? Special::type_info
                           : r.kind == sema::EntityKind::constructor  ? Special::constructor
                           : r.kind == sema::EntityKind::destructor ? Special::destructor
                           : r.kind == sema::EntityKind::deleting_destructor ? Special::deleting_destructor
                                                                    : Special::none});
    Entity& emitted = tu.entities.back();
    emitted.asm_label = program.asm_label(e);
    if (emitted.special == Special::destructor || emitted.special == Special::deleting_destructor) {
      if (const auto* owner = classes.find(r.scope))
        emitted.nonthrowing = owner->destructor_noexcept.value_or(false);
    } else if (fn) {
      emitted.nonthrowing = world.types.rec(d.type).fn_noexcept;
    }
    if (r.kind == sema::EntityKind::function && r.lang == ast::LangLinkage::cxx &&
        r.linkage == ast::Linkage::external && r.templ == EntityId::none && program.is_namespace(r.scope) &&
        emitted.asm_label.empty()) {
      // An ordinary function's name encodes its adjusted parameters. A nested function type
      // also encodes its result, which names_unnamed_namespace handles above.
      const auto type = world.types.rec(d.type);
      emitted.local_parameter_binding =
          std::ranges::any_of(type.params, [&](QualType param) { return names_unnamed_namespace(program, param); });
    }
    if (r.templ != EntityId::none) {
      tu.entities.back().template_type = world.entities.rec(r.templ).type;
      // A generic lambda's operator: its closure's record has the type with the head at depth 0.
      if (sema::Sema::closure_member(world, r.templ))
        tu.entities.back().template_type = world.entities.rec(world.entities.rec(r.templ).scope).type;
      tu.entities.back().template_args = r.args;
      // Only a function template's own arguments carry Clang's <template-param-decl> prefixes.
      if (world.entities.rec(r.templ).kind == sema::EntityKind::function_template)
        if (auto td = program.declaration(r.templ, program.end())) {
          std::vector<ast::TemplateParamSema> ps = ins.seqs.elements(td->template_params);
          sema::Templates t = program.templates();
          std::vector<std::uint8_t> same;
          for (std::size_t i = 0; i < r.args.size(); ++i) {
            // A pack's first element stands for it.
            ast::TemplateArg a = r.args[i];
            if (a.kind == ast::TemplateArgKind::pack && ins.seqs.size(a.elems) > 0) a = ins.seqs.at(a.elems, 0);
            same.push_back(i < ps.size() && ps[i].kind == ast::TemplateArgKind::template_ && a.templ != EntityId::none &&
                           t.heads_equivalent(ps[i], a.templ));
          }
          tu.entities.back().template_params = std::move(ps);
          tu.entities.back().template_same_heads = std::move(same);
        }
    }
  };
  // A part's closure members (docs/design/closures.md, decision 10). Their offsets are relative
  // to the enclosing declaration or body part, which begins at `offset` in the item.
  auto closures_of = [&](ast::SeqId<ast::ClosureDef> closures, ast::ItemId item, std::uint32_t offset) {
    for (const ast::ClosureDef& c : ins.seqs.elements(closures)) {
      // A template's or a generic lambda's definition is not code; its instances are.
      if (!sema::Sema::closure_has_code(world, c)) continue;
      // Its typed return is available to lookup, but calls and address formation require the ABI.
      if (c.flags & sema::closure_flag::polymorphic_result) continue;
      std::vector<ast::LocalRec> locals = ins.seqs.elements(c.locals);
      for (EntityId f : {c.call, c.invoker}) {
        if (f == EntityId::none) continue;
        sema::Sema::ClosureMember m = sema::Sema::closure_member_def(world, c, f, f == c.call ? c.body : c.invoker_body, locals);
        declare(m.decl, ast::Loc{item, offset + c.begin});
        tu.functions.push_back({f, m.decl, m.body, item, offset + c.body_begin, m.this_type});
      }
    }
  };
  Runtime<decltype(declare)> runtime{world, declare};
  std::optional<ast::DeclSema> root_deallocator;
  auto weak_inline_variable = [&](const ast::DeclSema& d) {
    if (!inline_declarations.contains(d.entity)) return false;
    const auto& entity = world.entities.rec(d.entity);
    if (entity.linkage != ast::Linkage::external || d.storage == ast::Storage::thread_local_ ||
        world.types.is_reference(d.type) || internal_type(d.type) || !program.asm_label(d.entity).empty())
      return false;
    if (!program.is_namespace(entity.scope)) return constexpr_member_symbol_linkage(d) == ast::Linkage::external;
    // The inline declaration belongs to the specialization itself ([temp.expl.spec]).
    // Builtin type arguments identify the same specialization in every translation unit.
    return entity.templ == EntityId::none || ((d.flags & sema::decl_flag::specialization) &&
        std::ranges::all_of(entity.args, [&](const ast::TemplateArg& arg) {
          return arg.kind == ast::TemplateArgKind::type && world.types.builtin_of(arg.type).has_value();
        }));
  };
  for (std::uint32_t k = 0; k < items.items.size(); ++k) {
    const items::Item& it = items.items[k];
    // Items of the global namespace, and the members of its classes.
    bool member = it.parent != items::none && items.items[it.parent].kind == items::ItemKind::class_def &&
                  program.in_global_scope(it.parent);
    if (!program.in_global_scope(k) && !member) continue;
    // A class template's members are patterns; its specializations' are instances, below.
    if (member && items.items[it.parent].template_depth > 0) continue;
    if (it.kind != items::ItemKind::declaration && it.kind != items::ItemKind::function &&
        it.kind != items::ItemKind::class_def && it.kind != items::ItemKind::enum_def)  // its declarators: `struct S { … } s;`
      continue;
    const ast::ItemSema& is = program.item_sema(k).value;
    std::vector<ast::DeclSema> decls = ins.seqs.elements(is.decls);
    for (ast::DeclSema& decl : decls) decl = program.completed_declaration(decl);
    // The declarators, by declaration ordinal: their constants and item-relative locations (the
    // group is at the item's start).
    std::vector<std::optional<ast::Declarator>> declarators(decls.size());
    std::vector<std::optional<ast::Loc>> at(decls.size());
    if (is.tree != ast::NodeId::none && ins.ast.is<ast::DeclGroup>(is.tree)) {
      for (ast::Child c : ins.ast.children(ins.ast.get<ast::DeclGroup>(is.tree).declarators)) {
        auto dr = ins.ast.get<ast::Declarator>(c.id);
        if (dr.decl >= decls.size()) continue;
        declarators[dr.decl] = dr;
        at[dr.decl] = ast::Loc{program.id(k), c.at};
      }
    }
    for (std::size_t i = 0; i < decls.size(); ++i) declare(decls[i], at[i]);
    // A template's definition, or a class template's member defined outside it, is not code; its
    // instances are, below.
    if (!decls.empty() && ((decls[0].flags & sema::decl_flag::template_) || ins.seqs.size(decls[0].template_params) > 0))
      continue;
    closures_of(is.closures, program.id(k), 0);
    if (it.kind == items::ItemKind::function && it.has_body() && !decls.empty() && decls[0].entity != EntityId::none &&
        !(decls[0].flags & sema::decl_flag::deleted) && !program.defaulted_member_definition(k, decls[0])) {
      // A placeholder return type is the one its body deduced ([dcl.spec.auto]).
      const ast::BodySema& body = program.analyze_body(k).value;
      ast::DeclSema d = decls[0];
      if (body.deduced) d.type = body.deduced_type;
      tu.functions.push_back({d.entity, d, body, program.id(k), it.body_begin - it.begin, program.this_type(d)});
      const auto& entity = world.entities.rec(d.entity);
      if (entity.kind == sema::EntityKind::destructor && d.is_virtual &&
          program.peer_root_destructor_producer(entity.scope)) {
        if (!root_destructor_body_supported(program, entity.scope, body)) {
          block_virtual_emission(tu, program, classes.rec(entity.scope));
          return tu;
        }
        root_deallocator = world.allocation_function(sema::AllocationFunction::deallocate_sized);
        // The peer can replace this ordinary implicit declaration. A written declaration in
        // the producer can instead carry ABI attributes erased by sema.
        if (program.declaration(root_deallocator->entity, program.end()) ||
            !program.asm_label(root_deallocator->entity).empty() ||
            program.has_unapplied_declaration_attributes(root_deallocator->entity)) {
          block_virtual_emission(tu, program, classes.rec(entity.scope));
          return tu;
        }
        auto& complete = tu.functions.back();
        complete.root_destructor_table = sema::virtual_table_entity(world, entity.scope);
        tu.base_object_variants.insert(complete.entity);
        ast::DeclSema deleting = d;
        deleting.entity = world.entities.entity({.kind = sema::EntityKind::deleting_destructor,
                                                .name = entity.name, .type = d.type, .scope = entity.scope});
        const ast::Loc location{complete.item, complete.body_offset};
        declare(deleting, location);
        tu.functions.push_back({deleting.entity, deleting, deleting_root_destructor_body(world, complete, *root_deallocator),
                                location.item, location.offset, complete.this_type});
        continue;
      }
      if (!member && program.supports_base_object_variant(d.entity, uncertain_layouts))
        tu.base_object_variants.insert(d.entity);
      tu.functions.back().coalesced = inline_declarations.contains(d.entity) && entity.kind == sema::EntityKind::function &&
                                     entity.linkage == ast::Linkage::external && entity.templ == EntityId::none &&
                                     tu.main != d.entity && ins.seqs.size(body.closures) == 0 &&
                                     program.asm_label(d.entity).empty();
      // A written ordinary body starts at '{'. Other starts include a function-try-block and a
      // constructor initializer. Synthetic bodies and instantiations never enter this path.
      if (linux && entity.kind == sema::EntityKind::constructor) {
        if (exceptions_enabled) hosted_union_constructor(program, tu.functions.back());
      } else if (!member && tu.main != d.entity && !it.specialization &&
                 program.region().tokens[it.body_begin].kind == lex::Tk::l_brace) {
        auto& f = tu.functions.back();
        if (linux && exceptions_enabled) {
          f.hosted_catch = hosted_catch(program, d, body);
          f.unwind = f.hosted_catch ? UnwindEligibility::hosted_catch : cleanup_free_definition(program, d, body);
        } else if (cleanup_free_definition(program, d, body) == UnwindEligibility::cleanup_free) {
          f.unwind = UnwindEligibility::cleanup_free;
        }
        if (exceptions_enabled && f.unwind == UnwindEligibility::none) {
          f.cleanup_call = hosted_cleanup_call(program, d, body);
          if (f.cleanup_call != ast::NodeId::none) f.unwind = UnwindEligibility::hosted_cleanup;
          else if (linux) f.unwind_through_constructor = hosted_union_constructor_caller(program, d, body);
        }
      }
      closures_of(body.closures, program.id(k), it.body_begin - it.begin);
      continue;
    }
    // Variable definitions, with the constants their declarators were initialized with.
    for (std::size_t i = 0; i < decls.size(); ++i) {
      const ast::DeclSema& d = decls[i];
      if (!declarators[i] || d.kind != ast::DeclKind::variable || d.entity == EntityId::none ||
          !(d.flags & sema::decl_flag::definition))
        continue;
      tu.variables.push_back({d.entity, d.type, declarators[i]->value, at[i]});
      tu.variables.back().coalesced = weak_inline_variable(d);
      // Not constant-initialized: initialized dynamically, in order ([basic.start.dynamic]); a
      // destructor runs at exit ([basic.start.term]).
      ast::NodeId init = declarators[i]->value == ast::ConstId::none ? declarators[i]->init.id : ast::NodeId::none;
      EntityId dtor = world.types.is_reference(d.type) ? EntityId::none : program.classes().destructor_of(d.type);
      if (dtor != EntityId::none && !tu.namespace_destruction) tu.namespace_destruction = at[i];
      if (init != ast::NodeId::none || dtor != EntityId::none) {
        dynamic.push_back({d.entity, d.type, init, dtor, k});
        tu.variables.back().dynamic = true;  // written at run time: by its initialization or its destructor
      }
    }
  }
  init_function(program, world, tu, dynamic, declare, runtime, program.region().files.at(0));
  // The function template specializations instantiated at the end of the unit ([temp.inst]), in
  // order of instantiation. A specialization that is only declared here is an external reference.
  for (const parse::Program::Instance* in : program.instances()) {
    if (in->failed || in->body.value.tree == ast::NodeId::none) continue;
    const items::Item& it = items.items[in->item];
    declare(in->decl, ast::Loc{program.id(in->item), 0});  // errors in its name point at the template
    tu.functions.push_back({in->spec, in->decl, in->body.value, program.id(in->item), it.body_begin - it.begin,
                            program.this_type(in->decl)});
    closures_of(in->body.value.closures, program.id(in->item), it.body_begin - it.begin);
  }
  // The variable template specializations instantiated ([temp.inst]/7), with their constants.
  for (const parse::Program::VarInstance& v : program.variable_instances()) {
    if (!v.cell.diags.empty()) continue;
    const ast::ItemSema& is = v.cell.value;
    for (std::size_t i = 0; i < ins.seqs.size(is.decls); ++i) {
      ast::DeclSema d = ins.seqs.at(is.decls, i);
      if (d.entity != v.spec) continue;
      ast::Loc at{program.id(v.item), 0};
      declare(d, at);
      ast::ConstId value = ast::ConstId::none;
      if (is.tree != ast::NodeId::none && ins.ast.is<ast::DeclGroup>(is.tree))
        for (ast::Child c : ins.ast.children(ins.ast.get<ast::DeclGroup>(is.tree).declarators))
          if (auto dr = ins.ast.get<ast::Declarator>(c.id); dr.decl == i) value = dr.value;
      if (d.flags & sema::decl_flag::definition) tu.variables.push_back({d.entity, d.type, value, at});
    }
  }
  // The static data members of class template specializations that are defined in the class
  // (constexpr or inline, [class.static.data]/4) and used, with their constants.
  for (const parse::Program::MemberInstance& m : program.member_instances()) {
    if (!m.cell->diags.empty() || world.types.is_dependent(world.types.class_type(m.spec))) continue;
    const ast::ItemSema& is = m.cell->value;
    if (is.tree == ast::NodeId::none || !ins.ast.is<ast::DeclGroup>(is.tree)) continue;
    for (ast::Child c : ins.ast.children(ins.ast.get<ast::DeclGroup>(is.tree).declarators)) {
      auto dr = ins.ast.get<ast::Declarator>(c.id);
      if (dr.decl >= ins.seqs.size(is.decls)) continue;
      ast::DeclSema d = ins.seqs.at(is.decls, dr.decl);
      if (d.kind != ast::DeclKind::variable || d.entity == EntityId::none || !(d.flags & sema::decl_flag::definition)) continue;
      // One whose initializer waits for a use is defined when it was used ([temp.inst]/3).
      ast::ConstId value = dr.value;
      if (program.is_lazy_member(d.entity)) {
        if (!std::ranges::contains(program.used_members(), d.entity)) continue;
        value = program.member_init(d.entity);
      }
      ast::Loc at{program.id(m.item), c.at};
      declare(d, at);
      tu.variables.push_back({d.entity, d.type, value, at});
    }
  }
  for (EntityId spec : program.required()) {
    ast::DeclSema d{};
    d.entity = spec;
    d.type = world.entities.rec(spec).type;
    declare(d, std::nullopt);
  }
  // The implicitly defined special members needed by this unit ([special]).
  for (std::size_t i = 0; i < program.synthesized().size(); ++i) {
    const parse::Program::Synthesized& s = program.synthesized()[i];
    const EntityId cls = world.entities.rec(s.decl.entity).scope;
    // A class template's current instantiation is not code ([temp.dep.type]).
    if (world.types.is_dependent(world.types.class_type(cls))) continue;
    declare(s.decl, std::nullopt);
    tu.functions.push_back({s.decl.entity, s.decl, program.synthesized_body(i), program.id(s.item), 0, program.this_type(s.decl)});
    using enum parse::Program::SpecialMember;
    tu.functions.back().coalesced = (s.kind == copy_assignment || s.kind == move_constructor) && !internal_class(cls);
  }
  auto unwind_provider = [&](EntityId callee) {
    return !program.function_defined_before(callee, program.end()) && !noexcept_declarations.contains(callee) &&
           program.asm_label(callee).empty();
  };
  auto unwind_definition = [&](const FunctionDef& f) {
    return !noexcept_declarations.contains(f.entity) && !f.coalesced && f.guards.empty() &&
           program.asm_label(f.entity).empty() &&
           (!inline_declarations.contains(f.entity) || f.constructor_cleanup != EntityId::none);
  };
  auto declare_virtual_table = [&](EntityId entity, std::optional<ast::Loc> at) {
    ast::DeclSema table{};
    table.kind = ast::DeclKind::variable;
    table.entity = entity;
    table.type = world.entities.rec(entity).type;
    declare(table, at);
  };
  for (FunctionDef& f : tu.functions) {
    f.explicit_weak = weak_declarations.contains(f.entity) && !inline_declarations.contains(f.entity);
    if (f.root_destructor_table != EntityId::none)
      declare_virtual_table(f.root_destructor_table, ast::Loc{f.item, f.body_offset});
    const auto locals = ins.seqs.elements(f.body.locals);
    if (classes.has_virtual_layouts()) {
      for (const auto& local : locals) {
        if (local.decl == ast::NodeId::none || !ins.ast.is<ast::LocalVar>(local.decl)) continue;
        const auto init = ins.ast.get<ast::LocalVar>(local.decl).init.id;
        if (init == ast::NodeId::none || !ins.ast.is<ast::PeerRootInit>(init)) continue;
        const EntityId table = ins.ast.get<ast::PeerRootInit>(init).vtable;
        if (seen.contains(std::to_underlying(table))) continue;
        const EntityId cls = world.entities.rec(table).scope;
        if (!program.peer_root_vtable_visible(cls)) {
          block_virtual_emission(tu, program, classes.rec(cls));
          return tu;
        }
        declare_virtual_table(table, ast::Loc{f.item, f.body_offset});
      }
    }
    static_locals(world, tu, f, locals, runtime);
    // Check the complete unit by entity, including later redeclarations and qualified friends.
    if (f.unwind == UnwindEligibility::none) continue;
    if (!unwind_definition(f)) {
      f.unwind = UnwindEligibility::none;
    } else if (f.unwind == UnwindEligibility::cleanup_free_with_throw && !tu.int_throw) {
      tu.int_throw = runtime.int_throw();
    } else if (f.unwind == UnwindEligibility::hosted_catch) {
      // An external provider supplies its own unwind coverage. A definition in this unit can
      // introduce intervening frames without it. The ordinary function type erases written noexcept.
      if (!unwind_provider(f.hosted_catch->callee))
        f.unwind = UnwindEligibility::none;
      else if (!tu.catch_runtime) tu.catch_runtime = runtime.catch_runtime();
    } else if (f.unwind == UnwindEligibility::hosted_cleanup) {
      const EntityId callee = ins.ast.get<ast::Call>(f.cleanup_call).function;
      if (!unwind_provider(callee) || (f.constructor_initializer_callee != EntityId::none &&
                                      !unwind_provider(f.constructor_initializer_callee)))
        f.unwind = UnwindEligibility::none;
      else if (tu.cleanup_resume == EntityId::none) tu.cleanup_resume = runtime.resume();
    }
  }
  if (has_typed_cast) {
    // Only emitted trees need native RTTI. In particular, syntax-only expressions and
    // constant initializers do not introduce references to the host runtime.
    std::set<std::pair<EntityId, EntityId>> checked;
    for (const FunctionDef& f : tu.functions) {
      if (f.body.tree == ast::NodeId::none) continue;
      auto collect = [&](this auto&& visit, const ast::Red& node) -> void {
        if (node.is<ast::DynamicCastPointer>()) {
          const auto cast = node.get<ast::DynamicCastPointer>();
          const EntityId source = world.entities.rec(cast.source_rtti).scope;
          const EntityId target = world.entities.rec(cast.target_rtti).scope;
          if (checked.emplace(source, target).second) {
            const ast::Loc at{f.item, node.offset()};
            const auto rtti = program.peer_typed_dynamic_cast(source, target);
            if (uncertain_layouts.contains(source) || uncertain_layouts.contains(target) ||
                rtti == parse::Program::TypedCastRtti::unsupported) {
              tu.unsupported_typed_dynamic_cast = at;
              return;
            }
            if (rtti == parse::Program::TypedCastRtti::keyless_base)
              tu.keyless_type_info.insert(cast.source_rtti);
            for (EntityId id : {cast.source_rtti, cast.target_rtti, cast.runtime}) {
              ast::DeclSema d{};
              d.kind = id == cast.runtime ? ast::DeclKind::function : ast::DeclKind::variable;
              d.entity = id;
              d.type = world.entities.rec(id).type;
              declare(d, at);
            }
          }
        }
        node.for_each_child([&](const ast::Red& child) {
          if (!tu.unsupported_typed_dynamic_cast) visit(child);
        });
      };
      collect(ast::Red(ins.ast, f.body.tree, f.body_offset));
      if (tu.unsupported_typed_dynamic_cast) return tu;
    }
    if (!native_virtual_layouts(tu, program, world, uncertain_layouts)) return tu;
  }
  // Register the owner after checking the makers. An owned declaration cannot stand in for
  // the visibility check required when a maker requests the address point.
  if (const auto& owned = tu.owned_virtual_table) {
    const auto key = program.member_decl(owned->slots.front());
    declare_virtual_table(owned->entity, key ? std::optional(ast::Loc{key->item, 0}) : std::nullopt);
  }
  for (FunctionDef& f : tu.functions) {
    if (f.unwind_through_constructor == EntityId::none) continue;
    const auto constructor = std::ranges::find(tu.functions, f.unwind_through_constructor, &FunctionDef::entity);
    if (unwind_definition(f) && constructor != tu.functions.end() && constructor->unwind == UnwindEligibility::hosted_cleanup)
      f.unwind = UnwindEligibility::cleanup_free;
    else f.unwind_through_constructor = EntityId::none;
  }
  // Written declarations have already supplied their locations and definitions. For the
  // remaining implicit declarations, a code reference alone emits an undefined symbol.
  for (EntityId e : program.implicit_allocation_functions()) {
    ast::DeclSema d{};
    d.entity = e;
    d.type = world.entities.rec(e).type;
    declare(d, std::nullopt);
  }
  if (root_deallocator) declare(*root_deallocator, std::nullopt);
  // A declaration anywhere in the unit can supply the visibility of the one object symbol.
  if (!hidden_declarations.empty() || !function_visibility.empty()) {
    for (Entity& e : tu.entities) {
      // An inline variable falls outside the ordinary data visibility profile.
      e.hidden = hidden_declarations.contains(e.id) && (e.is_function || (linux && !inline_declarations.contains(e.id)));
      if (!e.is_function || inline_declarations.contains(e.id)) continue;
      const auto found = function_visibility.find(e.id);
      if (found == function_visibility.end()) continue;
      const auto& visibility = found->second;
      // With no namespace setting or conflicting settings, use the direct-attribute behavior.
      if (visibility.namespace_hidden != visibility.namespace_default)
        e.hidden = visibility.written == Visibility::none
                       ? visibility.namespace_hidden : visibility.written == Visibility::hidden;
    }
  }
  return tu;
}

engine::query_task<pp::Region> PpTokens::run(engine::Ctx& cx, pp::FileId file) {
  const pp::Region& full = co_await cx.fetch<pp::Pp>(file);
  pp::Region out;
  out.tokens = full.tokens;
  for (lex::Tok& tok : out.tokens) tok.flags &= static_cast<std::uint8_t>(~lex::tok_flag::space_before);
  out.names = full.names;
  out.literals = full.literals;
  out.files = full.files;
  out.file_names = full.file_names;
  out.possibly_packed = full.possibly_packed;
  out.first_visibility_pragma = full.first_visibility_pragma;
  out.has_clang_attribute_pragma = full.has_clang_attribute_pragma;
  co_return out;
}

engine::query_task<bool> LexErrors::run(engine::Ctx& cx, pp::FileId file) {
  const std::vector<engine::Diagnostic>& lexed = co_await cx.fetch<engine::DiagsOf>(cx.cell_of<items::RegionItems>(file));
  co_return std::ranges::any_of(lexed, [](const engine::Diagnostic& d) {
    auto kind = lex::lex_diag_of(d.id);
    return kind && lex::message_of(*kind).severity == lex::Severity::error;
  });
}

engine::query_task<SemaResult> TuSema::run(engine::Ctx& cx, TuId tu) {
  const TuSource& src = cx.fetch<TuSourceIn>(tu);
  contract_assert(src.file.has_value());  // a unit given as definitions has no front end
  const pp::FileId file{*src.file};
  const pp::Source& unit_source = cx.fetch<pp::SourceOf>(file);
  sema::World& world = *universe().world_for(*unit_source.config.target);
  const pp::Region& region = co_await cx.fetch<PpTokens>(file);
  const items::Items& items = co_await cx.fetch<items::RegionItems>(file);
  SemaResult out;
  // Whether the lexer or preprocessor reported an error, not where: `TuFront` places them.
  out.lex_errors = co_await cx.fetch<LexErrors>(file);
  parse::Program program(world, region, items, unit_source.config.template_depth, unit_source.config.exceptions_enabled);
  program.analyze_all();
  // What an editor's position requests read (#170): where each entity is first declared, and the
  // bodies, whether or not the unit has errors; only what the analysis reached, which analysing
  // more would change (its diagnostics).
  {
    std::set<std::uint32_t> seen;
    for (std::uint32_t k = 0; k < items.items.size(); ++k) {
      const items::Item& it = items.items[k];
      const ast::ItemSema* analysed = program.analysed_item(k);
      if (!analysed) continue;
      const ast::ItemSema& is = *analysed;
      const std::vector<ast::DeclSema> decls = world.ins.seqs.elements(is.decls);
      std::vector<std::uint32_t> at(decls.size(), 0);
      if (is.tree != ast::NodeId::none && world.ins.ast.is<ast::DeclGroup>(is.tree))
        for (ast::Child c : world.ins.ast.children(world.ins.ast.get<ast::DeclGroup>(is.tree).declarators)) {
          auto dr = world.ins.ast.get<ast::Declarator>(c.id);
          if (dr.decl < decls.size()) at[dr.decl] = c.at;
        }
      for (std::size_t i = 0; i < decls.size(); ++i)
        if (decls[i].entity != EntityId::none && seen.insert(std::to_underlying(decls[i].entity)).second)
          out.declared.emplace_back(decls[i].entity, ast::Loc{program.id(k), at[i]});
      if (const ast::BodySema* body = program.analysed_body(k); body && body->tree != ast::NodeId::none && it.has_body())
        out.bodies.push_back({program.id(k), it.decl_length(), body->tree});
    }
  }
  for (const engine::Diagnostic& d : program.diagnostics()) {
    auto kind = sema::sema_diag_of(d.id);
    if (!kind) continue;
    std::vector<std::uint64_t> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    out.messages.push_back({sema::message_of(*kind).severity == lex::Severity::error,
                            sema::render(*kind, args, [&](char how, std::uint64_t raw) { return program.spell(how, raw); }),
                            program.token(d.at)});
  }
  std::ranges::stable_sort(out.messages, {}, &SemaMessage::token);
  // The unit's own records of its classes and enumerations (#115).
  const sema::Classes& classes = program.classes();
  const bool linux = unit_source.config.target->triple == ast::x86_64_linux.triple;
  // Both targets: the base-object entries and the virtual shapes rely on it, and the System V
  // aggregate rules read it on x86-64 (#650).
  const auto uncertain = program.uncertain_object_layouts(/*include_lifecycle_signatures=*/true);
  if (!out.has_errors()) out.unit = unit_of(program, world, items, src.name, uncertain);
  auto nontrivial_destructor = [&](this auto&& recurse, EntityId cls) -> bool {
    const auto* record = classes.find(cls);
    if (!record || record->destructor == EntityId::none || record->layout_requires_virtual_support) return false;
    using enum sema::CopyOperation;
    if (record->trivial_copyability.destructor != unknown)
      return record->trivial_copyability.destructor == eligible_nontrivial;
    if (auto member = std::ranges::find(record->members, record->destructor, &sema::MemberDeclRec::entity);
        member != record->members.end()) {
      // Defaulting the first declaration can still yield a trivial destructor ([dcl.fct.def.default]).
      auto first = program.member_decl(member->entity);
      return !member->is_deleted && first && !program.defaulted_body(program.index(first->item));
    }
    // A variant member does not make the union's own destructor nontrivial ([class.dtor]).
    if (record->union_) return false;
    // An implicit destructor is nontrivial when a direct base or by-value member's is nontrivial.
    return std::ranges::any_of(record->bases, [&](const auto& base) { return recurse(base.cls); }) ||
           std::ranges::any_of(record->fields, [&](const auto& field) {
             auto type = world.types.rec(field.type);
             while (type.kind == sema::TypeKind::array) type = world.types.rec(type.inner);
             return type.kind == sema::TypeKind::class_ && recurse(type.entity);
           });
  };
  for (EntityId c : classes.classes()) {
    ClassShape& shape = out.shapes.classes.emplace_back(ClassShape{c, {}, {}});
    const auto& record = classes.rec(c);
    const auto& fields = record.fields;
    for (const sema::FieldRec& f : fields) {
      if (f.no_unique_address) shape.overlapping_fields.push_back(std::uint32_t(shape.fields.size()));
      shape.fields.push_back(f.type);
      shape.has_mutable_field |= f.mutable_;
    }
    if (std::ranges::any_of(fields, [](const sema::FieldRec& f) { return f.bit_width != 0; }))
      for (const sema::FieldRec& f : fields) shape.bit_widths.push_back(f.bit_width);
    shape.union_ = record.union_;
    shape.pod = record.pod;
    shape.explicit_alignment = record.explicit_alignment;
    for (const sema::BaseRec& b : record.bases) shape.bases.push_back(b.cls);
    shape.trivial_calls = classes.trivial_for_calls(c, unit_source.config.calls());
    shape.layout_uncertain = uncertain.contains(c);
    shape.copy_abi_unknown = record.defaulted_copy == sema::CopyState::unsupported;
    {
      // Itanium C++ ABI §3.1.2.3 from the special members one by one ([class.prop]): a
      // non-trivial destructor or copy or move constructor, or neither constructor usable.
      using enum sema::CopyOperation;
      const sema::TrivialCopyability& t = record.trivial_copyability;
      const bool known = t.destructor != unknown && t.copy_constructor != unknown && t.move_constructor != unknown;
      const bool nontrivial = t.destructor == eligible_nontrivial || t.copy_constructor == eligible_nontrivial ||
                              t.move_constructor == eligible_nontrivial ||
                              (t.copy_constructor == ineligible && t.move_constructor == ineligible);
      // A proven nontrivial destructor settles the ABI even when the copy/move traits are unknown.
      shape.calls_abi_uncertain = !shape.trivial_calls && (!known || !nontrivial) &&
                                  !(linux && nontrivial_destructor(c));
    }
    shape.layout_requires_virtual_support = record.layout_requires_virtual_support;
    if (record.peer_virtual_layout != sema::PeerVirtualLayout::none ||
        record.peer_automatic_primary_base != EntityId::none) {
      const EntityId primary = record.peer_virtual_layout == sema::PeerVirtualLayout::primary_base
                                   ? record.bases.front().cls : record.peer_automatic_primary_base;
      const bool uncertain_base = primary != EntityId::none && uncertain.contains(primary);
      if (shape.layout_uncertain || uncertain_base) {
        if (!out.unit.virtual_emission_blocked) block_virtual_emission(out.unit, program, record);
      } else {
        shape.peer_virtual_layout = record.peer_virtual_layout;
        shape.peer_automatic_primary_base = record.peer_automatic_primary_base;
      }
    }
  }
  for (EntityId e : classes.enums()) out.shapes.enums.push_back({e, classes.underlying(e)});
  co_return out;
}

engine::query_task<FrontResult> TuFront::run(engine::Ctx& cx, TuId tu) {
  const TuSource& src = cx.fetch<TuSourceIn>(tu);
  contract_assert(src.file.has_value());  // a unit given as definitions has no front end
  const pp::FileId file{*src.file};
  const pp::Source& unit_source = cx.fetch<pp::SourceOf>(file);
  const pp::Region& region = co_await cx.fetch<pp::Pp>(file);
  const items::Items& items = co_await cx.fetch<items::RegionItems>(file);
  const std::vector<engine::Diagnostic>& lexed = co_await cx.fetch<engine::DiagsOf>(cx.cell_of<items::RegionItems>(file));
  FrontResult out;
  SourceLocations locations(cx, unit_source, file, region, items);
  // `pp` reports at byte offsets in the file (`lex::file_level`).
  for (const engine::Diagnostic& d : lexed) {
    auto kind = lex::lex_diag_of(d.id);
    if (!kind) continue;
    std::vector<std::uint64_t> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    const auto f = static_cast<std::uint16_t>(lex::file_level - d.at.item);
    Message m{d.at.offset, lex::message_of(*kind).severity == lex::Severity::error, lex::render(*kind, args),
              locations.at({f, 0, d.at.offset})};
    // The first token at or after the position in its file, else (a header of only directives)
    // the first token of a file included after it (files are numbered as they are first met).
    m.token = static_cast<std::uint32_t>(region.locs.size() - 1);
    std::uint32_t later = m.token;
    for (std::uint32_t t = 0; t + 1 < region.locs.size(); ++t) {
      pp::TokLoc l = region.site(t);
      if (l.file == f && l.offset >= d.at.offset) {
        m.token = t;
        break;
      }
      if (l.file > f && later == region.locs.size() - 1) later = t;
    }
    m.token = std::min(m.token, later);
    out.messages.push_back(std::move(m));
  }
  // Sema's messages, placed: a macro's token is where it was expanded (ast.md §8.3).
  const SemaResult& sema = co_await cx.fetch<TuSema>(tu);
  for (const SemaMessage& m : sema.messages) {
    const pp::TokLoc at = region.site(std::min<std::size_t>(m.token, region.locs.size() - 1));
    out.messages.push_back({at.offset, m.error, m.text, locations.at(at), m.token});
  }
  std::ranges::stable_sort(out.messages, {}, &Message::token);
  out.shapes = sema.shapes;
  out.declared = sema.declared;
  out.bodies = sema.bodies;
  co_return out;
}

// A unit given as definitions has no classes of its own.
engine::query_task<Shapes> UnitShapes::run(engine::Ctx& cx, TuId tu) {
  if (!cx.fetch<TuSourceIn>(tu).file) co_return Shapes{};
  const SemaResult& u = co_await cx.fetch<TuSema>(tu);
  co_return u.shapes;
}

engine::query_task<TranslationUnit> TuUnit::run(engine::Ctx& cx, TuId tu) {
  const TuSource& src = cx.fetch<TuSourceIn>(tu);
  if (!src.file) co_return src.given;
  // The analysis, not its placed messages: an edit that only moves tokens stops before this.
  const SemaResult& sema = co_await cx.fetch<TuSema>(tu);
  if (sema.has_errors()) {
    TranslationUnit failed;
    failed.name = src.name;
    failed.front_failed = true;
    co_return failed;
  }
  co_return sema.unit;
}

engine::query_task<std::optional<FileOffset>> DefinitionAt::run(engine::Ctx& cx, DefinitionKey k) {
  const pp::FileId file{k.tu.n};
  const pp::Source& source = cx.fetch<pp::SourceOf>(file);
  const pp::Region& region = co_await cx.fetch<pp::Pp>(file);
  const items::Items& items = co_await cx.fetch<items::RegionItems>(file);
  const FrontResult& front = co_await cx.fetch<TuFront>(k.tu);
  ast::Interners& in = *universe().in;
  auto absolute = [&](const std::string& name) {
    std::filesystem::path p(name);
    if (p.is_relative() && !source.cwd.empty()) p = std::filesystem::path(source.cwd) / p;
    return p.lexically_normal().generic_native_encoded_string();
  };
  // The region's number for the file, and the token at the offset: the last whose site in that
  // file starts at or before it (a macro's tokens all sit at its name).
  const std::string& want = pp::path_of(k.file);
  std::optional<std::uint16_t> f;
  for (std::size_t i = 0; i < region.files.size() && !f; ++i)
    if (!region.files[i].starts_with('<') && absolute(region.files[i]) == want) f = static_cast<std::uint16_t>(i);
  if (!f) co_return std::nullopt;
  std::optional<std::uint32_t> tok;
  for (std::uint32_t t = 0; t + 1 < region.locs.size(); ++t) {
    const pp::TokLoc l = region.site(t);
    if (l.file != *f || l.offset > k.offset) continue;
    if (!tok || region.site(*tok).offset < l.offset) tok = t;
  }
  // A name: an identifier the cursor is on (a macro's tokens sit at its name, of another length).
  if (!tok || region.tokens[*tok].kind != lex::Tk::identifier) co_return std::nullopt;
  if (region.locs[*tok].expansion == 0 && k.offset >= region.locs[*tok].offset + region.name(region.tokens[*tok]).size())
    co_return std::nullopt;
  // The innermost item holding it (items are in preorder), and the body that is that item's.
  std::optional<std::uint32_t> item;
  for (std::uint32_t i = 0; i < items.items.size(); ++i)
    if (items.items[i].begin <= *tok && *tok < items.items[i].end) item = i;
  if (!item) co_return std::nullopt;
  const ast::ItemId id = in.items.add<ast::ItemId>(items.items[*item].id);
  const std::uint32_t at = *tok - items.items[*item].begin;
  EntityId named = EntityId::none;
  for (const BodyAt& b : front.bodies) {
    if (b.item != id) continue;
    // The innermost node, through the nodes that wrap a name without being one (an implicit
    // conversion, parentheses); a call names its function only from its callee.
    ast::Red(in.ast, b.tree, b.offset).find(at, [&](const ast::Red& r) {
      const ast::Red* from = nullptr;
      for (const ast::Red* n = &r; n; from = n, n = n->parent()) {
        if (n->is<ast::DeclRef>()) named = n->get<ast::DeclRef>().entity;
        else if (n->is<ast::Member>()) named = n->get<ast::Member>().member;
        else if (n->is<ast::Call>() && from && from->id() == n->get<ast::Call>().callee.id) named = n->get<ast::Call>().function;
        else if (n->is<ast::ImplicitConversion>() || n->is<ast::Paren>()) continue;
        break;
      }
    });
  }
  if (named == EntityId::none) co_return std::nullopt;
  // A function template's specialization is declared as its template.
  const sema::World& world = *universe().world_for(*source.config.target);
  auto d = std::ranges::find(front.declared, named, &std::pair<EntityId, ast::Loc>::first);
  if (d == front.declared.end())
    if (EntityId templ = world.entities.rec(named).templ; templ != EntityId::none) {
      named = templ;
      d = std::ranges::find(front.declared, named, &std::pair<EntityId, ast::Loc>::first);
    }
  if (d == front.declared.end()) co_return std::nullopt;
  // Where it is first declared: its declarator's token in its item, moved to the declared name
  // (`*p` and `(*f)()` start before it), then that token's site.
  const std::string_view name = in.names.spelling(world.entities.rec(named).name);
  for (const items::Item& it : items.items) {
    if (in.items.add<ast::ItemId>(it.id) != d->second.item) continue;
    std::size_t t = std::min<std::size_t>(it.begin + d->second.offset, region.locs.size() - 1);
    for (std::size_t u = t; u < it.end && u + 1 < region.tokens.size(); ++u)
      if (region.tokens[u].kind == lex::Tk::identifier && region.name(region.tokens[u]) == name) {
        t = u;
        break;
      }
    const pp::TokLoc l = region.site(t);
    if (region.files[l.file].starts_with('<')) co_return std::nullopt;
    co_return FileOffset{absolute(region.files[l.file]), l.offset};
  }
  co_return std::nullopt;
}

}  // namespace ycxx::pipeline
