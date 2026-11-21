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

struct FrontResult {
  std::vector<Message> messages;  // in order of position in the unit
  TranslationUnit unit;           // meaningful when there are no errors
  Shapes shapes;                  // its classes and enumerations, for `layout` (#115)
  // For an editor (#170), with errors or not: each entity's first declaration in the unit (its
  // declarator, else its item's start), in item order, and the analysed function bodies.
  std::vector<std::pair<EntityId, ast::Loc>> declared;
  std::vector<BodyAt> bodies;
  bool has_errors() const {
    return std::ranges::any_of(messages, [](const Message& m) { return m.error; });
  }
};

// One translation unit from its source file (`TuSource::file`), analysed.
struct TuFront {
  using Key = TuId;
  using Value = FrontResult;
  static engine::query_task<Value> run(engine::Ctx& cx, TuId tu);
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

// The C++ runtime's entry points the unit calls (Itanium C++ ABI §3.3), declared on first use.
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
};

// A function's static locals ([stmt.dcl]/3): each a variable of the unit, initialized with its
// constant if it has one, else zero; one initialized by code, or with a destructor, also gets a
// guard variable, and the function the runtime entry points its initialization calls.
template <class R>
void static_locals(sema::World& world, TranslationUnit& tu, FunctionDef& f, R& runtime) {
  ast::Interners& ins = world.ins;
  for (const ast::LocalRec& l : ins.seqs.elements(f.body.locals)) {
    if (l.entity == EntityId::none || l.decl == ast::NodeId::none || !ins.ast.is<ast::StaticVar>(l.decl)) continue;
    f.weak_elf = false;  // The function and its statics need group selection (#422).
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

// The definitions and declarations of the global namespace, in item order.
TranslationUnit unit_of(parse::Program& program, sema::World& world, const items::Items& items, std::string name) {
  ast::Interners& ins = world.ins;
  TranslationUnit tu;
  tu.name = std::move(name);
  const bool linux = world.target.triple == ast::x86_64_linux.triple;
  std::set<std::uint32_t> seen;
  std::set<EntityId> inline_declarations;
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
  auto declare = [&](const ast::DeclSema& d, std::optional<ast::Loc> at) {
    EntityId e = d.entity;
    if (e == EntityId::none) return;
    // Any reachable declaration can mark the entity inline ([dcl.inline]).
    if (linux && (d.flags & sema::decl_flag::inline_) &&
        (d.kind == ast::DeclKind::function || d.kind == ast::DeclKind::variable))
      inline_declarations.insert(e);
    if (!seen.insert(std::to_underlying(e)).second) return;
    const sema::EntityRec& r = world.entities.rec(e);
    if (r.kind == sema::EntityKind::class_ || r.kind == sema::EntityKind::data_member || r.kind == sema::EntityKind::type_alias ||
        r.kind == sema::EntityKind::enum_ || r.kind == sema::EntityKind::enumerator ||
        r.kind == sema::EntityKind::function_template || r.kind == sema::EntityKind::variable_template ||
        r.kind == sema::EntityKind::alias_template || r.kind == sema::EntityKind::concept_)
      return;  // no symbol: a template's specializations have them
    bool fn = r.kind != sema::EntityKind::variable;
    std::string id(ins.names.spelling(r.name));
    if (fn && id == "main" && r.scope == EntityId::none) tu.main = e;  // [basic.start.main]: the global one
    // A member of a class local to the unit is local too.
    ast::Linkage linkage = r.linkage;
    for (EntityId sc = r.scope; linkage == ast::Linkage::external && sc != EntityId::none; sc = world.entities.rec(sc).scope)
      if (world.entities.rec(sc).kind == sema::EntityKind::class_ && internal_class(sc)) linkage = ast::Linkage::internal;
    tu.entities.push_back({e, std::move(id), d.type, fn, r.lang, linkage, false, at, r.scope,
                           r.kind == sema::EntityKind::constructor  ? Special::constructor
                           : r.kind == sema::EntityKind::destructor ? Special::destructor
                                                                    : Special::none});
    tu.entities.back().asm_label = program.asm_label(e);
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
  auto weak_inline_variable = [&](const ast::DeclSema& d) {
    if (!inline_declarations.contains(d.entity)) return false;
    const auto& entity = world.entities.rec(d.entity);
    if (entity.linkage != ast::Linkage::external || !program.is_namespace(entity.scope) ||
        d.storage == ast::Storage::thread_local_ || world.types.is_reference(d.type) ||
        internal_type(d.type) || !program.asm_label(d.entity).empty())
      return false;
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
        !(decls[0].flags & sema::decl_flag::deleted) && !program.defaulted_copy_definition(k, decls[0])) {
      // A placeholder return type is the one its body deduced ([dcl.spec.auto]).
      const ast::BodySema& body = program.analyze_body(k).value;
      ast::DeclSema d = decls[0];
      if (body.deduced) d.type = body.deduced_type;
      tu.functions.push_back({d.entity, d, body, program.id(k), it.body_begin - it.begin, program.this_type(d)});
      const auto& entity = world.entities.rec(d.entity);
      tu.functions.back().weak_elf = inline_declarations.contains(d.entity) && entity.kind == sema::EntityKind::function &&
                                    entity.linkage == ast::Linkage::external && entity.templ == EntityId::none &&
                                    tu.main != d.entity && ins.seqs.size(body.closures) == 0 &&
                                    program.asm_label(d.entity).empty();
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
      tu.variables.back().weak_elf = weak_inline_variable(d);
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
    tu.functions.back().weak_elf = linux && s.kind == parse::Program::SpecialMember::copy_assignment && !internal_class(cls);
  }
  for (FunctionDef& f : tu.functions) static_locals(world, tu, f, runtime);
  // Written declarations have already supplied their locations and definitions. For the
  // remaining implicit declarations, a code reference alone emits an undefined symbol.
  for (EntityId e : program.implicit_allocation_functions()) {
    ast::DeclSema d{};
    d.entity = e;
    d.type = world.entities.rec(e).type;
    declare(d, std::nullopt);
  }
  return tu;
}

namespace {

// The parser skips attributes whose layout semantics we do not implement. Find those
// that could change a record or field even though its computed offsets look natural.
bool layout_attribute(const pp::Region& region, std::uint32_t begin, std::uint32_t end, bool leading_only = false) {
  using enum lex::Tk;
  for (std::uint32_t i = begin; i < end; ++i) {
    auto tok = region.tokens[i];
    if (tok.kind == kw_alignas) return true;
    if (i + 1 == end) break;
    const auto next = region.tokens[i + 1].kind;
    bool bracket = tok.kind == l_square && next == l_square;
    bool gnu = tok.kind == identifier && next == l_paren && region.name(tok) == "__attribute__";
    if (!bracket && !gnu) {
      if (leading_only) return false;
      continue;
    }
    if (gnu) ++i;
    const auto open = bracket ? l_square : l_paren;
    const auto close = bracket ? r_square : r_paren;
    std::uint32_t depth = 1;
    while (++i < end && depth) {
      auto part = region.tokens[i];
      if (part.kind == open) ++depth;
      else if (part.kind == close && --depth == 0) break;
      else if (part.kind == identifier) {
        auto name = region.name(part);
        if (name.size() > 4 && name.starts_with("__") && name.ends_with("__"))
          name = name.substr(2, name.size() - 4);
        if (name == "packed" || name == "aligned" || name == "mode" || name == "vector_size" ||
            name == "no_unique_address" || name == "ms_struct" || name == "gcc_struct") return true;
      }
    }
  }
  return false;
}

std::set<EntityId> uncertain_record_layouts(parse::Program& program, sema::World& world) {
  const auto& region = program.region();
  const auto& items = program.items().items;
  std::set<EntityId> records;
  std::set<EntityId> annotated_tags;
  std::set<std::uint32_t> uncertain_definitions;
  // Sema erases alias types. Remember affected spellings across alias declarations so a
  // scalar alias that changed layout also makes records using it uncertain.
  std::set<std::string_view> aliases;
  auto uncertain_alias = [&](const items::Item& item) {
    if (aliases.empty()) return false;
    for (std::uint32_t i = item.begin; i < item.end; ++i) {
      auto tok = region.tokens[i];
      if (tok.kind == lex::Tk::identifier && aliases.contains(region.name(tok)) &&
          !std::ranges::contains(item.names, i - item.begin, &items::DeclName::offset)) return true;
    }
    return false;
  };
  for (std::uint32_t k = 0; k < items.size(); ++k) {
    const auto& item = items[k];
    const auto* analysed = program.analysed_item(k);
    const bool definition = item.kind == items::ItemKind::class_def;
    if (!analysed || (!definition && item.kind != items::ItemKind::declaration)) continue;
    const auto& ranges = region.possibly_packed;
    auto pack = std::ranges::upper_bound(ranges, item.begin, {}, [](const auto& range) { return range.second; });
    bool packed = definition && pack != ranges.end() && pack->first < item.brace_end;
    // The attributes immediately after the type's closing brace can still apply to
    // the type. Those within a following declarator apply to the declared name.
    bool record_annotated = definition
        ? layout_attribute(region, item.begin, item.brace_begin) ||
          layout_attribute(region, item.brace_end, item.end, true)
        : layout_attribute(region, item.begin, item.end) || uncertain_alias(item);
    bool annotated = record_annotated || (definition && layout_attribute(region, item.brace_end, item.end));
    if (!packed && !annotated) continue;
    if (definition && (packed || record_annotated)) uncertain_definitions.insert(k);
    for (auto d : world.ins.seqs.elements(analysed->decls)) {
      if (d.entity == EntityId::none) continue;
      if (d.kind == ast::DeclKind::class_ && (packed || record_annotated)) {
        records.insert(d.entity);
        if (!definition) annotated_tags.insert(d.entity);
      }
      if (annotated && d.kind == ast::DeclKind::data_member) {
        records.insert(world.entities.rec(d.entity).scope);
        if (item.parent != items::none) uncertain_definitions.insert(item.parent);
      }
      if (annotated && d.kind == ast::DeclKind::type_alias) {
        if (world.types.is_class(d.type)) records.insert(world.types.class_of(d.type));
        else aliases.insert(world.ins.names.spelling(world.entities.rec(d.entity).name));
      }
    }
  }
  // Instantiated classes have their own entities. Their packing and field declarations
  // come from the selected pattern, which may be a partial specialization.
  for (EntityId c : program.classes().classes()) {
    auto pattern = program.class_pattern_item(c);
    if (pattern && (uncertain_definitions.contains(*pattern) || annotated_tags.contains(world.entities.rec(c).templ)))
      records.insert(c);
  }
  return records;
}

}  // namespace

engine::query_task<FrontResult> TuFront::run(engine::Ctx& cx, TuId tu) {
  const TuSource& src = cx.fetch<TuSourceIn>(tu);
  contract_assert(src.file.has_value());  // a unit given as definitions has no front end
  const pp::FileId file{*src.file};
  const pp::Source& unit_source = cx.fetch<pp::SourceOf>(file);
  sema::World& world = *universe().world_for(*unit_source.config.target);
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
  // The unit's language options: the instantiation limit (`-ftemplate-depth`).
  parse::Program program(world, region, items, unit_source.config.template_depth);
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
    const pp::TokLoc at = program.site(d.at);
    out.messages.push_back({at.offset, sema::message_of(*kind).severity == lex::Severity::error,
                            sema::render(*kind, args, [&](char how, std::uint64_t raw) { return program.spell(how, raw); }),
                            locations.at(at), program.token(d.at)});
  }
  std::ranges::stable_sort(out.messages, {}, &Message::token);
  if (!out.has_errors()) out.unit = unit_of(program, world, items, src.name);
  // The unit's own records of its classes and enumerations (#115).
  const sema::Classes& classes = program.classes();
  const auto uncertain = unit_source.config.target->triple == ast::x86_64_linux.triple
      ? uncertain_record_layouts(program, world)
      : std::set<EntityId>{};
  for (EntityId c : classes.classes()) {
    ClassShape& shape = out.shapes.classes.emplace_back(ClassShape{c, {}, {}});
    const auto& fields = classes.rec(c).fields;
    for (const sema::FieldRec& f : fields) shape.fields.push_back(f.type);
    if (std::ranges::any_of(fields, [](const sema::FieldRec& f) { return f.bit_width != 0; }))
      for (const sema::FieldRec& f : fields) shape.bit_widths.push_back(f.bit_width);
    shape.union_ = classes.rec(c).union_;
    shape.pod = classes.rec(c).pod;
    for (const sema::BaseRec& b : classes.rec(c).bases) shape.bases.push_back(b.cls);
    shape.trivial_calls = classes.trivial_for_calls(c, unit_source.config.calls());
    shape.layout_uncertain = uncertain.contains(c);
    shape.copy_abi_unknown = classes.rec(c).defaulted_copy == sema::CopyState::unsupported;
  }
  for (EntityId e : classes.enums()) out.shapes.enums.push_back({e, classes.underlying(e)});
  co_return out;
}

// A unit given as definitions has no classes of its own.
engine::query_task<Shapes> UnitShapes::run(engine::Ctx& cx, TuId tu) {
  if (!cx.fetch<TuSourceIn>(tu).file) co_return Shapes{};
  const FrontResult& u = co_await cx.fetch<TuFront>(tu);
  co_return u.shapes;
}

engine::query_task<TranslationUnit> TuUnit::run(engine::Ctx& cx, TuId tu) {
  const TuSource& src = cx.fetch<TuSourceIn>(tu);
  if (!src.file) co_return src.given;
  const FrontResult& front = co_await cx.fetch<TuFront>(tu);
  if (front.has_errors()) {
    TranslationUnit failed;
    failed.name = src.name;
    failed.front_failed = true;
    co_return failed;
  }
  co_return front.unit;
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
