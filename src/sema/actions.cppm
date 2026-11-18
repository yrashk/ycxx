// Semantic actions: what the parser calls as it parses (docs/design/ast.md §3.2), building the
// analysed tree in a frame builder (§9.3). Every expression node carries its type and value
// category (§3.3); implicit conversions are explicit nodes of the operand's width ([conv],
// [expr.arith.conv], [expr.context]).
//
// Positions: an `Ex` knows its token range within the part being analysed (an item's
// declaration part, or a function body, §3.4), so a parent stores each child at its offset
// relative to the parent. Diagnostics are located item-relative (§8.2).
//
// Lookups go through `Questions`, the stand-in for the engine context of query-engine.md §B1.4:
// once contextual queries exist (#16), each of its functions is a recorded question or a fetch.
export module ycxx.sema:actions;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.interp;
import ycxx.lex;
import :types;
import :diag;
import :templates;

export namespace ycxx::sema {

using ast::Child;
using ast::ExprInfo;
using ast::NodeId;
using ast::ValueCategory;

// What sema asks of its context (§B1.4, §B3). In the engine these are `visible_decls(::, name,
// at item)` and fetches of `decl_sema`.
class Questions {
public:
  // The declarations of `name` in the global namespace visible at the current point: those that
  // precede the current item, and, inside a function body, the item's own (§B2, [basic.scope.pdecl]).
  // `directives` are the namespaces block-scope using-directives nominate ([namespace.udir]): their
  // names join the lookup where [namespace.udir]/2 puts them.
  virtual std::vector<DeclId> visible_decls(NameId name, std::span<const EntityId> directives = {}) = 0;
  virtual ast::DeclSema decl_sema(DeclId d) = 0;
  // What a constant evaluation fetches (§B7): `function_ir` of a function defined before the
  // current point ([expr.const]), and a namespace-scope variable as the interpreter sees it.
  virtual interp::Callee function_ir(EntityId f) = 0;
  virtual std::optional<interp::GlobalVar> global(EntityId v) = 0;
  virtual bool is_constexpr(EntityId f) = 0;  // a constexpr or consteval function
  // Whether class `c` is complete at the current point ([class.mem.general]): its definition
  // precedes the point and does not enclose it.
  virtual bool is_complete(EntityId c) = 0;
  // Inside the enumerator list of an enumeration this unit is defining: its record so far, which
  // is published only after the list (#156).
  virtual const EnumRec* defining_enum(EntityId) { return nullptr; }
  // This unit's class and enumeration records (#115).
  virtual const Classes& classes() = 0;
  // The constant a variable usable in constant expressions holds ([expr.const]), if it is known.
  virtual std::optional<ConstId> constant_value(EntityId v) = 0;
  // In a member declaration of a class being defined: the member named `name` declared before it
  // ([basic.scope.class]; the class's member list is not complete yet).
  virtual std::optional<ast::DeclSema> earlier_member(NameId name) = 0;
  // Namespaces ([basic.namespace]), the global one being `EntityId::none`: the namespace (or
  // alias) unqualified lookup finds for `name`, and qualified lookup of `name` in namespace `ns`
  // ([namespace.qual]).
  virtual std::optional<EntityId> namespace_named(NameId name, std::span<const EntityId> directives = {}) = 0;
  virtual std::vector<ast::DeclSema> namespace_members(EntityId ns, NameId name) = 0;
  // In an instantiation: whether the call whose callee begins at `offset` was dependent in the
  // template's definition, and the members of namespace `ns` named `name` that are visible at the
  // point of instantiation, which argument-dependent lookup searches ([temp.dep.candidate]).
  // Outside one, false and none.
  virtual bool dependent_call(u32 /*offset*/) { return false; }
  // Whether instantiation has given up at its nesting limit, which was diagnosed ([temp.inst]/16):
  // what fails because of it says nothing more.
  virtual bool instantiation_gave_up() { return false; }
  virtual std::vector<ast::DeclSema> members_at_instantiation(EntityId /*ns*/, NameId) { return {}; }
  virtual ~Questions() = default;
};

// An analysed expression or statement under construction: a draft node and its token range in
// the part. `id` is none after an error that has been diagnosed; actions given such an operand
// give up silently, so one mistake gives one diagnostic.
struct Ex {
  NodeId id = NodeId::none;
  u32 begin = 0;
  u32 end = 0;
  ExprInfo info{};
  bool ok() const { return id != NodeId::none; }
  u32 width() const { return end - begin; }
  QualType type() const { return info.type; }
  ValueCategory cat() const { return info.category; }
  bool glvalue() const { return info.category != ValueCategory::prvalue; }
};

// A braced-init-list as written ([dcl.init.general]): each initializer-clause is an analysed
// expression or a nested list, with its designator (`.name =`, [dcl.init.aggr]) if it has one.
// Sema decides what it initializes once it knows the target type.
struct Braced {
  struct Clause {
    Ex expr;
    std::unique_ptr<Braced> list;
    NameId designator{};
    bool designated = false;
    u32 begin = 0;
  };
  u32 begin = 0;
  u32 end = 0;
  std::vector<Clause> clauses;
};

// Flags of `ExprInfo::flags`.
namespace expr_flag {
// Type- or value-dependent ([temp.dep.expr], [temp.dep.constexpr]): its type or value depends on
// a template parameter, so it is analysed when the template is instantiated.
inline constexpr u8 dependent = 1;
}

// Flags of `LocalRec::flags` and `DeclSema::flags`.
namespace local_flag {
inline constexpr u16 parameter = 1;
}
namespace decl_flag {
inline constexpr u16 definition = 1;
inline constexpr u16 inline_ = 2;
inline constexpr u16 constexpr_ = 4;
inline constexpr u16 variadic = 8;
inline constexpr u16 lang_c = 16;  // declared with C language linkage ([dcl.link])
inline constexpr u16 consteval_ = 32;  // an immediate function ([dcl.constexpr])
inline constexpr u16 member = 64;      // a member of a class ([class.mem])
inline constexpr u16 static_ = 128;    // a static member ([class.static])
inline constexpr u16 deleted = 256;    // a deleted definition ([dcl.fct.def.delete])
inline constexpr u16 constructor = 512;
inline constexpr u16 destructor = 1024;
inline constexpr u16 implicit = 2048;  // an implicitly declared special member ([special])
inline constexpr u16 template_ = 4096;  // a template ([temp.pre]); its head is `template_params`
}

// What a cell demands of the program besides its value (query-engine.md §B1.4): a function
// template specialization whose definition is required ([temp.inst]/8), instantiated at the end
// of the translation unit (§B6.2).
struct Demand {
  EntityId spec;
  ast::Loc at;
};

class Sema {
public:
  Sema(World& w, ast::Builder& b, Questions& q, std::vector<engine::Diagnostic>& diags, ast::ItemId item,
       u32 loc_base)
      : w_(&w), b_(&b), q_(&q), diags_(&diags), item_(item), loc_base_(loc_base) {}

  World& world() { return *w_; }
  ast::ItemId item() const { return item_; }
  Types& types() { return w_->types; }
  ast::Builder& builder() { return *b_; }
  ast::Interners& ins() { return w_->ins; }

  // ------------------------------------------------------------ diagnostics

  engine::DiagArg arg(NameId n) { return {std::to_underlying(n), ins().names.fp(n)}; }
  engine::DiagArg arg(QualType t) { return {std::to_underlying(t), types().fp(t)}; }
  engine::DiagArg arg(lex::Tk k) { return engine::diag_arg(std::to_underlying(k)); }
  engine::DiagArg arg(u64 v) { return engine::diag_arg(v); }
  engine::DiagArg arg(ConstId c) { return {std::to_underlying(c), ins().values.fp(c)}; }

  template <class... A>
  void diag(u32 at, Diag d, const A&... args) {
    diags_->push_back({engine::Loc{std::to_underlying(item_), loc_base_ + at}, diag_id(d), {arg(args)...}});
  }
  std::size_t diag_count() const { return diags_->size(); }
  // Drops the diagnostics after the first `n`: a part analysed again where it was diagnosed already.
  void truncate_diags(std::size_t n) { diags_->resize(n); }

  // ------------------------------------------------------------ building

  template <class T>
  Ex node(const T& v, u32 begin, u32 end, ExprInfo info = {}) {
    return {b_->add(v, end - begin), begin, end, info};
  }
  static Child at(const Ex& child, u32 parent_begin) {
    return {child.id, child.ok() ? child.begin - parent_begin : 0};
  }
  // A draft node read back, as the parser does while it builds.
  template <class T>
  std::optional<T> as(const Ex& e) const {
    if (!e.ok() || ast::kind_of(e.id) != ast::Nodes::index_of<T>()) return std::nullopt;
    return b_->get<T>(e.id);
  }

  QualType builtin(Builtin k) { return types().builtin(k); }

  // What constant evaluation answers for list-initialization's narrowing check ([dcl.init.list]);
  // set by the parser, which owns the evaluator.
  std::function<std::optional<ConstId>(Ex)> constant_of;

  // ------------------------------------------------------------ classes ([class], [expr.ref])

  // A type-name naming a class, found by unqualified lookup ([basic.lookup.unqual]): none when
  // the name is not declared, or a variable or function hides it ([basic.scope.hiding]). With
  // `elaborated`, non-type declarations are ignored ([basic.lookup.elab]).
  // A typedef-name too, unless `aliases` is false (after a class-key, [dcl.type.elab]/2).
  std::optional<QualType> class_name(NameId name, bool elaborated = false, bool aliases = true) {
    // Innermost block first: a block-scope typedef-name, or a local that hides the type.
    for (std::size_t d = scopes_.size(); d-- > 0;) {
      for (auto t = block_types_.rbegin(); t != block_types_.rend(); ++t)
        if (t->depth == d && t->name == name) {
          if (aliases) return t->type;
          break;
        }
      if (!elaborated && std::ranges::any_of(scopes_[d], [&](const auto& p) { return p.first == name; })) return std::nullopt;
    }
    // A template parameter hides what the enclosing scopes declare ([basic.scope.temp]).
    if (const ast::TemplateParamSema* p = template_param(name)) {
      if (elaborated || p->kind != ast::TemplateArgKind::type) return std::nullopt;
      if (auto a = template_args.at(p->depth, p->index)) return a->type;
      return p->type;
    }
    // In a class template, its name without arguments is the injected-class-name: the current
    // instantiation, or in an instantiation the specialization ([temp.local]/1).
    if (current_class != EntityId::none) {
      const EntityRec& c = w_->entities.rec(current_class);
      if (c.templ != EntityId::none && c.name == name) return types().class_type(current_class);
    }
    // A type nested in the class whose scope this is ([basic.scope.class]).
    if (current_class != EntityId::none) {
      if (const ClassRec* cr = class_rec(current_class); cr && cr->defined) {
        for (const auto& [n, t] : cr->types)
          if (n == name) return t;
      } else if (auto m = q_->earlier_member(name); m && m->kind == ast::DeclKind::enum_) {
        return m->type;
      }
    }
    std::optional<ast::DeclSema> found;
    for (auto p = own.rbegin(); p != own.rend() && !found; ++p)
      if (p->name == name) found = p->sema;
    if (!found)
      for (DeclId d : q_->visible_decls(name, directives())) {
        ast::DeclSema s = q_->decl_sema(d);
        if (s.entity == EntityId::none || is_special(s)) continue;
        if (is_type(s.kind) || !elaborated) found = s;
      }
    if (!found || !is_type(found->kind)) return std::nullopt;
    if (found->kind == ast::DeclKind::type_alias && !aliases) return std::nullopt;
    // A class template's name needs its arguments outside the template ([temp.names]).
    if (found->flags & decl_flag::template_) return std::nullopt;
    return found->type;
  }
  static bool is_type(ast::DeclKind k) {
    return k == ast::DeclKind::class_ || k == ast::DeclKind::enum_ || k == ast::DeclKind::type_alias;
  }
  // A block-scope typedef-name or alias-declaration ([dcl.typedef]); it may redeclare one of the
  // same block to the same type.
  void local_type_alias(NameId name, QualType type, u32 at_) {
    std::size_t depth = scopes_.size() - 1;
    // [basic.scope.block]/2: nor a parameter's, a for-init-statement's or a condition's name in
    // the outermost block of the guarded substatement.
    bool guarded = scopes_.size() >= 2 && !guarded_.empty() && guarded_.back() == scopes_.size() - 2 &&
                   std::ranges::any_of(scopes_[guarded_.back()], [&](const auto& p) { return p.first == name; });
    if (guarded || std::ranges::any_of(scopes_.back(), [&](const auto& p) { return p.first == name; }) ||
        std::ranges::any_of(block_types_, [&](const BlockType& t) {
          return t.depth == depth && t.name == name && types().canonical(t.type) != types().canonical(type);
        })) {
      diag(at_, Diag::redeclared_local, name);
      return;
    }
    block_types_.push_back({depth, name, type});
  }

  // The class template unqualified lookup finds for `name`, which a `<` after it begins the
  // arguments of ([temp.names]/3); none if it finds anything else.
  std::optional<ast::DeclSema> class_template(NameId name) {
    for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s)
      for (auto p = s->rbegin(); p != s->rend(); ++p)
        if (p->first == name) return std::nullopt;
    if (template_param(name)) return std::nullopt;
    std::optional<ast::DeclSema> found;
    // Block-scope using-declarations, innermost first ([namespace.udecl]).
    for (auto u = block_usings_.rbegin(); u != block_usings_.rend() && !found; ++u)
      if (u->name == name)
        for (const ast::DeclSema& d : u->decls)
          if (d.kind == ast::DeclKind::class_ && (d.flags & decl_flag::template_)) found = d;
    for (auto p = own.rbegin(); p != own.rend() && !found; ++p)
      if (p->name == name) found = p->sema;
    std::vector<ast::DeclSema> decls;
    for (DeclId d : q_->visible_decls(name, directives())) {
      ast::DeclSema s = q_->decl_sema(d);
      if (s.entity != EntityId::none && !is_special(s)) decls.push_back(s);
    }
    if (!found && !decls.empty()) found = decls.back();
    if (!found || found->kind != ast::DeclKind::class_ || !(found->flags & decl_flag::template_)) return std::nullopt;
    // Default arguments from every earlier declaration of the template ([temp.param]/12).
    std::vector<ast::TemplateParamSema> params = ins().seqs.elements(found->template_params);
    bool merged = false;
    for (const ast::DeclSema& d : decls) {
      if (d.entity != found->entity) continue;
      std::vector<ast::TemplateParamSema> other = ins().seqs.elements(d.template_params);
      for (std::size_t i = 0; i < params.size() && i < other.size(); ++i)
        if (!params[i].has_default && other[i].has_default) {
          params[i].has_default = 1;
          params[i].default_ = other[i].default_;
          merged = true;
        }
    }
    if (merged) found->template_params = ins().intern_seq(std::span<const ast::TemplateParamSema>(params));
    return found;
  }

  // The class template named `name` that is a member of namespace `ns` ([namespace.qual]).
  std::optional<ast::DeclSema> namespace_class_template(EntityId ns, NameId name) {
    for (const ast::DeclSema& d : q_->namespace_members(ns, name))
      if (d.kind == ast::DeclKind::class_ && (d.flags & decl_flag::template_)) return d;
    return std::nullopt;
  }

  // The specialization of class template `templ` for `args` ([temp.spec.general]): its entity is
  // the template and the canonical arguments (§B1.5). With dependent arguments it is a dependent
  // type ([temp.dep.type]).
  EntityId class_specialization(EntityId templ, std::span<const TemplateArg> args) {
    return templates().specialize_class(templ, args);
  }

  // `X<args>` naming a class template specialization ([temp.names], [temp.arg]): each argument of
  // its parameter's kind, values converted to the parameter's type, defaults for the rest.
  std::optional<QualType> class_template_id(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, u32 at) {
    std::vector<ast::TemplateParamSema> params = ins().seqs.elements(tmpl.template_params);
    NameId name = w_->entities.rec(tmpl.entity).name;
    if (args.size() > params.size()) {
      diag(at, Diag::template_args_mismatch, name);
      return std::nullopt;
    }
    Templates t = templates();
    Bindings given;
    given.levels.resize(params.empty() ? 1 : params[0].depth + 1);
    auto& out = given.levels.back();
    out.resize(params.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (args[i].kind != params[i].kind) {
        diag(at, Diag::template_args_mismatch, name);
        return std::nullopt;
      }
      out[i] = args[i];
      if (params[i].kind == ast::TemplateArgKind::value) {
        auto pt = t.substitute(params[i].type, given);
        auto v = pt ? t.converted_value(args[i], *pt) : std::nullopt;
        if (!v) {
          diag(at, Diag::template_args_mismatch, name);
          return std::nullopt;
        }
        out[i] = *v;
      }
    }
    auto full = t.complete(tmpl, params, out);
    if (!full) {
      diag(at, Diag::template_args_mismatch, name);
      return std::nullopt;
    }
    return types().class_type(class_specialization(tmpl.entity, *full));
  }

  // `using enum E;` in a block ([enum.udecl]): E's enumerators are found in this scope.
  void using_enum(QualType t, u32 at_) {
    if (!types().is_enum(t)) {
      diag(at_, Diag::not_a_type, no_name());
      return;
    }
    using_enums_.push_back({scopes_.size() - 1, types().enum_of(t)});
  }

  // ------------------------------------------------------------ qualified names ([basic.lookup.qual])

  // What a nested-name-specifier names: a namespace (none for the global one) or a class or
  // enumeration.
  struct Scope {
    std::optional<EntityId> ns;
    std::optional<QualType> type;
  };
  static Scope global_scope() { return {EntityId::none, std::nullopt}; }
  // The first component of a nested-name-specifier, by unqualified lookup ([basic.lookup.qual]/1:
  // only namespaces, types and templates).
  std::optional<Scope> scope_named(NameId n) {
    // A type template parameter as a nested-name-specifier names a dependent type
    // ([temp.dep.type]); in an instantiation, its argument.
    if (const ast::TemplateParamSema* p = template_param(n)) {
      if (p->kind != ast::TemplateArgKind::type) return std::nullopt;
      // Bound to a type that has no members, it names no scope ([basic.lookup.qual]/1).
      if (auto a = template_args.at(p->depth, p->index)) {
        if (!types().is_class(a->type) && !types().is_enum(a->type)) return std::nullopt;
        return Scope{std::nullopt, a->type};
      }
      return Scope{std::nullopt, p->type};
    }
    for (auto a = block_aliases_.rbegin(); a != block_aliases_.rend(); ++a)
      if (a->name == n) return Scope{a->ns, std::nullopt};
    if (auto t = class_name(n, true)) return Scope{std::nullopt, t};
    if (auto ns = q_->namespace_named(n, directives())) return Scope{ns, std::nullopt};
    return std::nullopt;
  }
  // A later component: a namespace or type member of a namespace, or a type nested in a class.
  std::optional<Scope> scope_member(const Scope& s, NameId n) {
    // A member of a dependent type is looked up when the template is instantiated
    // ([temp.dep.type]/7): a dependent type here.
    if (s.type && types().is_dependent(*s.type)) return Scope{std::nullopt, types().dep_name(*s.type, n)};
    if (s.ns) {
      for (const ast::DeclSema& d : q_->namespace_members(*s.ns, n)) {
        if (d.kind == ast::DeclKind::namespace_) return Scope{d.entity, std::nullopt};
        if (is_type(d.kind)) return Scope{std::nullopt, d.type};
      }
      return std::nullopt;
    }
    if (s.type && types().is_class(*s.type))
      if (const ClassRec* cr = class_rec(types().class_of(*s.type)))
        for (const auto& [name, t] : cr->types)
          if (name == n) return Scope{std::nullopt, t};
    return std::nullopt;
  }
  // `S::n` naming a type.
  std::optional<QualType> scope_type(const Scope& s, NameId n) {
    auto m = scope_member(s, n);
    if (!m || !m->type) return std::nullopt;
    return m->type;
  }
  // `S::n` as an expression ([expr.prim.id.qual]).
  Ex scope_expr(const Scope& s, NameId n, u32 begin, u32 name_at, u32 end) {
    if (s.type && types().is_dependent(*s.type)) return dependent_node({}, begin, end, std::nullopt, ValueCategory::lvalue);
    if (s.type) {
      if (types().is_enum(*s.type)) {
        if (auto r = find_enumerator(types().enum_of(*s.type), n, begin, end)) return *r;
        diag(name_at, Diag::no_member, n, *s.type);
        return {NodeId::none, begin, end};
      }
      if (!types().is_class(*s.type)) {
        diag(name_at, Diag::no_member, n, *s.type);
        return {NodeId::none, begin, end};
      }
      if (!require_complete(*s.type, begin)) return {NodeId::none, begin, end};
      if (auto r = class_member(types().class_of(*s.type), n, begin, end)) return *r;
      diag(name_at, Diag::no_member, n, *s.type);
      return {NodeId::none, begin, end};
    }
    std::vector<ast::DeclSema> found = q_->namespace_members(*s.ns, n);
    if (found.empty()) {
      diag(name_at, Diag::no_namespace_member, n);
      return {NodeId::none, begin, end};
    }
    return decls_expr(std::move(found), true, n, begin, end);
  }
  // Block-scope `using namespace N;`, `using N::x;` and `namespace A = N;` ([namespace.udir],
  // [namespace.udecl], [namespace.alias]).
  void using_directive(EntityId ns) { block_directives_.push_back({scopes_.size() - 1, ns}); }
  // The namespaces the block-scope using-directives in force nominate.
  std::vector<EntityId> directives() const {
    std::vector<EntityId> r;
    for (const auto& [depth, ns] : block_directives_) r.push_back(ns);
    return r;
  }
  void using_declaration(const Scope& s, NameId n, u32 at_) {
    // [basic.scope.scope]/4: it may not clash with a local declared in the same block before it.
    if (!scopes_.empty() && std::ranges::any_of(scopes_.back(), [&](const auto& p) { return p.first == n; })) {
      diag(at_, Diag::redeclared_local, n);
      return;
    }
    std::vector<ast::DeclSema> found = s.ns ? q_->namespace_members(*s.ns, n) : std::vector<ast::DeclSema>{};
    if (found.empty()) {
      diag(at_, Diag::no_namespace_member, n);
      return;
    }
    block_usings_.push_back({scopes_.size() - 1, n, std::move(found)});
  }
  void namespace_alias(NameId n, EntityId ns) { block_aliases_.push_back({scopes_.size() - 1, n, ns}); }

  // A type nested in a class: `C::T` ([class.mem], [basic.lookup.qual]).
  std::optional<QualType> nested_type(NameId cls, NameId name) {
    auto t = class_name(cls, true);
    if (!t || !types().is_class(*t)) return std::nullopt;
    if (const ClassRec* cr = class_rec(types().class_of(*t)))
      for (const auto& [n, nt] : cr->types)
        if (n == name) return nt;
    return std::nullopt;
  }

  // [dcl.init.string]/1: the code units, terminating null included, of string literal `e` when it
  // can initialize an array of `elem`: an ordinary literal one of char, signed char or unsigned
  // char, a UTF-8 literal one of char8_t, char or unsigned char.
  std::optional<std::string> string_units(QualType elem, Ex e) {
    if (!e.ok() || !types().is_array(e.type())) return std::nullopt;
    auto lit = as<ast::Literal>(e);
    auto src = types().builtin_of(Types::unqualified(types().rec(e.type()).inner));
    auto dst = types().builtin_of(Types::unqualified(elem));
    if (!lit || !src || !dst) return std::nullopt;
    bool fits = (*src == Builtin::char_ && (*dst == Builtin::char_ || *dst == Builtin::signed_char || *dst == Builtin::unsigned_char)) ||
                (*src == Builtin::char8_t_ && (*dst == Builtin::char8_t_ || *dst == Builtin::char_ || *dst == Builtin::unsigned_char));
    if (!fits) return std::nullopt;
    return w_->values.rec(lit->value).units;
  }
  // An aggregate's element of character array type from string literal `e`, if it is one.
  std::optional<Ex> string_element(QualType et, Ex e) {
    if (!types().is_array(et) || !string_units(Types::unqualified(types().rec(et).inner), e)) return std::nullopt;
    return string_init(et, e);
  }
  // A character array initialized by a string literal ([dcl.init.string]): its elements are the
  // literal's code units and the rest zero (/3); there must be room for the null (/2).
  std::optional<Ex> string_init(QualType t, Ex e) {
    if (!types().is_array(t)) return std::nullopt;
    QualType elem = Types::unqualified(types().rec(t).inner);
    auto units = string_units(elem, e);
    if (!units) return std::nullopt;
    if (units->size() > types().rec(t).bound) {
      diag(e.begin, Diag::too_many_initializers, t);
      return Ex{NodeId::none, e.begin, e.end};
    }
    Braced b;
    b.begin = e.begin;
    b.end = e.end;
    for (char u : *units)
      b.clauses.push_back({literal(elem, w_->values.integer(elem, static_cast<unsigned char>(u)), ValueCategory::prvalue, e.begin, e.end),
                           nullptr, {}, false, e.begin});
    return list_init(t, b);
  }

  // Whether `t` is complete at this point; a class needs its definition before the point.
  bool is_complete(QualType t) {
    // A dependent type is checked when the template is instantiated; the current instantiation's
    // class is defined by its template's definition, which completeness asks for.
    if (types().is_dependent(t)) {
      if (types().is_class(t)) q_->is_complete(types().class_of(t));
      return true;
    }
    if (types().is_void(t)) return false;
    if (types().is_array(t))
      return types().rec(t).bound_value != ConstId::none && is_complete(types().rec(t).inner);  // [dcl.array]/6
    if (types().is_class(t)) return q_->is_complete(types().class_of(t));
    return true;
  }
  // Diagnoses an object of incomplete type, at `at`.
  bool require_complete(QualType t, u32 at) {
    if (is_complete(t)) return true;
    diag(at, Diag::incomplete_type, t);
    return false;
  }

  // [expr.ref]: `object.name`, or `object->name` with `arrow`.
  Ex member(Ex object, bool arrow, NameId name, u32 end, u32 op_at, u32 name_at) {
    u32 begin = object.begin;
    Ex fail{NodeId::none, begin, end};
    if (!object.ok()) return fail;
    // A member of the current instantiation or of an unknown specialization is looked up when the
    // template is instantiated ([temp.dep.expr]/5).
    if (dependent(object)) return dependent_node({object}, begin, end, std::nullopt, ValueCategory::lvalue);
    QualType ct = object.type();
    if (arrow) {
      object = rvalue(object);
      if (!types().is_pointer(object.type()) || !types().is_class(types().pointee(object.type()))) {
        diag(op_at, Diag::arrow_of_non_pointer, object.type());
        return fail;
      }
      ct = types().pointee(object.type());
    } else if (!types().is_class(ct)) {
      diag(op_at, Diag::member_of_non_class, ct);
      return fail;
    } else if (!object.glvalue()) {
      object = materialize(object, ct, false);  // [expr.ref]: a class prvalue is materialized
    }
    if (!require_complete(ct, op_at)) return fail;
    EntityId c = types().class_of(ct);
    auto found = find_member(c, name);
    if (!found) {
      diag(name_at, Diag::no_member, name, Types::unqualified(ct));
      return fail;
    }
    if (found->member && found->member->is_function) return bound_member(c, name, object, arrow, begin, end);
    if (found->member) {
      // A static data member: the object expression is evaluated, then discarded ([expr.ref]).
      const MemberDeclRec& m = *found->member;
      ExprInfo info{m.type, ValueCategory::lvalue, 0};
      Ex ref = node(ast::DeclRef{info, m.entity}, name_at, end, info);
      Ex discarded = convert_node(object, ast::ConversionKind::to_void, builtin(Builtin::void_));
      return node(ast::OrderedBinary{info, ast::OrderedOp::comma, at(discarded, begin), at(ref, begin)}, begin, end, info);
    }
    u32 index = found->index;
    const FieldRec& f = *found->field;
    // [expr.ref]/7.2: the member's type, with the object's cv-qualifiers added; the result is an
    // lvalue if the object is (or is reached through a pointer), else an xvalue.
    // A reference member gives the object it refers to, an lvalue.
    bool ref = types().is_reference(f.type);
    QualType t = ref ? types().referee(f.type) : types().add_cv(f.type, Types::cv(ct));
    ValueCategory cat = ref || arrow || object.cat() == ValueCategory::lvalue ? ValueCategory::lvalue : ValueCategory::xvalue;
    ExprInfo info{t, cat, 0};
    return node(ast::Member{info, f.entity, index, arrow ? 1u : 0u, ref ? 1u : 0u, at(object, begin)}, begin, end, info);
  }

  // ------------------------------------------------------------ initialization ([dcl.init])

  // An aggregate ([dcl.init.aggr]): an array, or a class (every class the front end knows so far
  // is one: no constructors, bases or virtual functions yet).
  bool is_aggregate(QualType t) {
    if (types().is_array(t)) return true;
    if (!types().is_class(t)) return false;
    const ClassRec* c = class_rec(types().class_of(t));
    return !c || !c->user_ctors;  // [dcl.init.aggr]/1: no user-declared constructors
  }
  bool has_user_ctors(QualType t) { return types().is_class(t) && !is_aggregate(t); }

  // The destructor that ends an object of type `t`, or none if it is trivial ([class.dtor]); for
  // an array, its elements'.
  EntityId destructor_of(QualType t) { return q_->classes().destructor_of(t); }
  // This unit's records (#115).
  const Classes& classes() { return q_->classes(); }
  // A class's record where the class is complete ([class.mem.general]), not while the unit
  // analyses its definition (#156).
  const ClassRec* class_rec(EntityId c) { return q_->is_complete(c) ? q_->classes().find(c) : nullptr; }
  // An enumeration's record: in its own enumerator list, this unit's so far.
  const EnumRec* enum_rec(EntityId e) {
    if (const EnumRec* r = q_->defining_enum(e)) return r;
    return q_->classes().find_enum(e);
  }

  // The elements of an aggregate, in order: an array's elements, a class's non-static data
  // members, each with the aggregate's cv added.
  struct Element {
    QualType type;
    NameId name;
    ConstId init = ConstId::none;  // a member's default member initializer, if constant
    bool dmi = false;              // it has one
  };
  std::vector<Element> elements(QualType t) {
    std::vector<Element> r;
    TypeRec tr = types().rec(t);
    if (tr.kind == TypeKind::array) {
      r.assign(tr.bound, Element{tr.inner, no_name()});
    } else if (const ClassRec* c = tr.kind == TypeKind::class_ ? class_rec(tr.entity) : nullptr) {
      for (const FieldRec& f : c->fields) r.push_back({types().add_cv(f.type, Types::cv(t)), f.name, f.init, f.dmi});
    }
    return r;
  }

  // Whether initializing a `t` runs a default member initializer.
  // (or a constructor).
  bool has_member_inits(QualType t) {
    TypeRec r = types().rec(t);
    if (r.kind == TypeKind::array) return has_member_inits(r.inner);
    if (r.kind != TypeKind::class_) return false;
    if (const ClassRec* c = class_rec(r.entity); c && c->user_ctors) return true;
    if (const ClassRec* c = class_rec(r.entity))
      for (const FieldRec& f : c->fields)
        if (f.init != ConstId::none || f.dmi || has_member_inits(f.type)) return true;
    return false;
  }

  // Value-initialization ([dcl.init.general]) of an object of type `t`. An aggregate whose
  // members have default member initializers gets them (the implicit default constructor's).
  Ex value_init(QualType t, u32 begin, u32 end) {
    if (types().is_dependent(t)) return dependent_node({}, begin, end);
    if (has_user_ctors(t)) return construct(t, {}, begin, end);  // the default constructor
    if (has_member_inits(t)) return member_inits(t, true, begin, end);
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    return node(ast::ValueInit{info}, begin, end, info);
  }
  // Default-initialization ([dcl.init.general]/7): the default member initializers, and no
  // initialization for the rest.
  Ex default_init(QualType t, u32 begin, u32 end) {
    if (types().is_dependent(t)) return dependent_node({}, begin, end);
    if (has_user_ctors(t)) return construct(t, {}, begin, end);
    if (has_member_inits(t)) return member_inits(t, false, begin, end);
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    return node(ast::DefaultInit{info}, begin, end, info);
  }
  // An aggregate's elements each from its default member initializer, else value- or
  // default-initialized.
  Ex member_inits(QualType t, bool value, u32 begin, u32 end) {
    std::vector<Ex> out;
    for (const Element& e : elements(t)) out.push_back(element_default(e, value, begin, end));
    return list_of(t, std::move(out), begin, end);
  }
  Ex element_default(const Element& e, bool value, u32 begin, u32 end) {
    if (e.init != ConstId::none) return literal(Types::unqualified(e.type), e.init, ValueCategory::prvalue, begin, end);
    if (e.dmi) {
      diag(begin, Diag::not_supported, lex::Tk::equal);  // a non-constant default member initializer in an aggregate (#118)
      return {NodeId::none, begin, end};
    }
    return value ? value_init(e.type, begin, end) : default_init(e.type, begin, end);
  }
  Ex list_of(QualType t, std::vector<Ex> out, u32 begin, u32 end) {
    std::vector<Child> cs;
    for (const Ex& e : out) cs.push_back({e.id, e.begin >= begin ? e.begin - begin : 0});
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    Ex r = node(ast::InitList{info, b_->list(cs)}, begin, end, info);
    list_elems_[std::to_underlying(r.id)] = std::move(out);
    return r;
  }

  // List-initialization of an object of type `t` from `b` ([dcl.init.list]/3).
  Ex list_init(QualType t, const Braced& b) {
    Ex fail{NodeId::none, b.begin, b.end};
    if (types().is_dependent(t) || dependent(b)) {
      std::vector<Ex> ops;
      for (const Braced::Clause& c : b.clauses)
        if (!c.list) ops.push_back(c.expr);
      return dependent_node(ops, b.begin, b.end, types().is_dependent(t) ? std::nullopt : std::optional(Types::unqualified(t)));
    }
    if (types().is_void(t) && b.clauses.empty()) return void_prvalue(b.begin, b.end);  // [expr.type.conv]/2
    if (!require_complete(t, b.begin)) return fail;
    // [dcl.init.string]/1: a character array from a string literal in braces.
    if (b.clauses.size() == 1 && !b.clauses[0].list && !b.clauses[0].designated)
      if (auto r = string_init(t, b.clauses[0].expr)) return *r;
    // [dcl.init.general]: an initializer-list or a designated-initializer-list, not a mix.
    if (std::ranges::any_of(b.clauses, &Braced::Clause::designated) &&
        !std::ranges::all_of(b.clauses, &Braced::Clause::designated)) {
      diag(b.begin, Diag::mixed_designators);
      return fail;
    }
    // [dcl.init.list]/3.2: a class from one element of its own class.
    if (types().is_class(t) && b.clauses.size() == 1 && !b.clauses[0].list && !b.clauses[0].designated &&
        initializes(t, b.clauses[0].expr))
      return convert(b.clauses[0].expr, t, b.clauses[0].begin);
    // [dcl.init.list]/3.7: a class with constructors: `{}` value-initializes it, else the clauses
    // are a constructor's arguments, without narrowing.
    if (has_user_ctors(t)) {
      if (b.clauses.empty()) return value_init(t, b.begin, b.end);
      std::vector<Ex> args;
      for (const Braced::Clause& c : b.clauses) {
        if (c.list || c.designated) {
          diag(c.begin, Diag::not_supported, lex::Tk::l_brace);  // nested lists as constructor arguments
          return fail;
        }
        args.push_back(c.expr);
      }
      return construct(t, std::move(args), b.begin, b.end, true);
    }
    // A reference from one element ([dcl.init.list]/3.10).
    if (types().is_reference(t) && b.clauses.size() == 1 && !b.clauses[0].list && !b.clauses[0].designated)
      return bind_reference(b.clauses[0].expr, t, b.clauses[0].begin);
    if (is_aggregate(t)) {
      std::size_t next = 0;
      Ex r = aggregate(t, b, next, b.begin, b.end);
      if (r.ok() && next < b.clauses.size()) {
        diag(b.clauses[next].begin, Diag::too_many_initializers, Types::unqualified(t));
        return fail;
      }
      return r;
    }
    // A scalar: `{}` value-initializes it, `{e}` copy-initializes it from `e`, without narrowing.
    if (b.clauses.empty()) return value_init(t, b.begin, b.end);
    if (b.clauses.size() > 1) {
      diag(b.clauses[1].begin, Diag::too_many_initializers, Types::unqualified(t));
      return fail;
    }
    const Braced::Clause& c = b.clauses[0];
    if (c.list || c.designated) {
      diag(c.begin, Diag::braces_for_scalar, Types::unqualified(t));
      return fail;
    }
    // [dcl.init.list]/3.8: an enumeration with a fixed underlying type from one integer, without
    // narrowing.
    if (types().is_enum(t) && c.expr.ok() && !types().is_enum(c.expr.type())) {
      const EnumRec* er = enum_rec(types().enum_of(t));
      if (er && er->fixed) {
        Ex v = list_element(*er->underlying, c.expr);
        if (!v.ok()) return fail;
        return convert_node(v, ast::ConversionKind::integral, Types::unqualified(t));
      }
    }
    return list_element(t, c.expr);
  }

  // The elements of aggregate `t` from the clauses of `b` starting at `next`, which advances past
  // those used. Called for a braced list, and for brace elision ([dcl.init.aggr]/16), where the
  // sub-aggregate takes as many clauses as it has elements.
  Ex aggregate(QualType t, const Braced& b, std::size_t& next, u32 begin, u32 end) {
    Ex fail{NodeId::none, begin, end};
    std::vector<Element> elems = elements(t);
    std::vector<Ex> out;
    bool elided = begin != b.begin || end != b.end;
    for (std::size_t i = 0; i < elems.size(); ++i) {
      if (next == b.clauses.size()) {
        out.push_back(element_default(elems[i], true, end, end));
        if (!out.back().ok()) return fail;  // diagnosed (a default member initializer not supported yet)
        continue;
      }
      const Braced::Clause& c = b.clauses[next];
      if (c.designated) {
        // [dcl.init.aggr]/3.1: designators name members in declaration order; the members
        // skipped are initialized as if there were no initializer.
        std::size_t j = i;
        while (j < elems.size() && elems[j].name != c.designator) ++j;
        if (elided || !types().is_class(t) || j == elems.size()) {
          diag(c.begin, Diag::bad_designator, c.designator, Types::unqualified(t));
          return fail;
        }
        for (; i < j; ++i) out.push_back(element_default(elems[i], true, c.begin, c.begin));
      }
      QualType et = elems[i].type;
      if (c.list) {
        ++next;
        out.push_back(list_init(et, *c.list));
      } else if (auto str = string_element(et, c.expr)) {
        // [dcl.init.string]: a character array element from a string literal, not elided into.
        ++next;
        out.push_back(*str);
      } else if (is_aggregate(et) && !c.designated && !initializes(et, c.expr)) {
        // Brace elision: the clause begins the sub-aggregate's own elements.
        if (!require_complete(et, c.begin)) return fail;
        out.push_back(aggregate(et, b, next, c.begin, c.begin));
        out.back().end = next > 0 ? b.clauses[next - 1].expr.end : c.begin;
      } else {
        ++next;
        out.push_back(list_element(et, c.expr));
      }
      if (!out.back().ok()) return fail;
    }
    return list_of(t, std::move(out), begin, end);
  }
  // The elements of an `InitList` this frame built, as analysed.
  std::span<const Ex> list_elements(const Ex& list) const {
    auto it = list_elems_.find(std::to_underlying(list.id));
    if (it == list_elems_.end()) return {};
    return it->second;
  }

  // The bound of an array of `elem` initialized by `b` ([dcl.array]/8): the number of elements
  // its clauses initialize, brace elision included.
  u64 deduce_bound(QualType elem, const Braced& b) {
    if (b.clauses.size() == 1 && !b.clauses[0].list && !b.clauses[0].designated)
      if (auto u = string_units(elem, b.clauses[0].expr)) return u->size();  // [dcl.init.string]/1
    u64 n = 0;
    std::size_t next = 0;
    while (next < b.clauses.size()) {
      const Braced::Clause& c = b.clauses[next];
      if (c.list || c.designated || !is_aggregate(elem) || initializes(elem, c.expr) || !is_complete(elem) ||
          (types().is_array(elem) && string_units(types().rec(elem).inner, c.expr)))
        ++next;
      else {
        std::size_t before = next;
        aggregate(elem, b, next, c.begin, c.begin);
        if (next == before) ++next;  // an element with no elements of its own takes the clause
      }
      ++n;
    }
    return n;
  }

  // Whether `e` initializes an object of aggregate type `t` by itself rather than starting its
  // elements: a class object from an expression of its own class ([dcl.init.aggr]/16).
  bool initializes(QualType t, const Ex& e) {
    return e.ok() && types().is_class(t) && types().is_class(e.type()) &&
           types().class_of(t) == types().class_of(e.type());
  }

  // An element copy-initialized from `e`, where a narrowing conversion is ill-formed
  // ([dcl.init.list]/7).
  Ex list_element(QualType t, Ex e) {
    if (!e.ok()) return e;
    Ex from = rvalue(e);
    Ex r = convert(e, t, e.begin);
    if (r.ok() && narrows(from, t)) {
      diag(e.begin, Diag::narrowing, from.type(), Types::unqualified(t));
      return {NodeId::none, e.begin, e.end};
    }
    return r;
  }

  // [dcl.init.list]/7: an integral conversion to a type that cannot represent every value of the
  // source type, unless the source is a constant whose value fits; a pointer to bool.
  bool narrows(const Ex& from, QualType to) {
    if (!from.ok() || dependent(from) || types().is_dependent(to)) return false;
    auto a = types().builtin_of(from.type()), b = types().builtin_of(to);
    if (types().is_pointer(from.type()) && b == Builtin::bool_) return true;
    if (!a || !b || !ast::is_integral(*a) || !ast::is_integral(*b) || *a == *b) return false;
    IntShape sa = shape(*a), sb = shape(*b);
    if (*b == Builtin::bool_) sb = {1, false};
    if (*a == Builtin::bool_) sa = {1, false};
    if (ast::represents(target, *b, sa.bits, sa.is_signed) || (*b == Builtin::bool_ && *a == Builtin::bool_)) return false;
    auto v = constant_of ? constant_of(from) : std::nullopt;
    if (!v || w_->values.rec(*v).kind != ValueKind::integer) return true;
    // The constant, read in the source type, must lie in the target type's range.
    u64 mask = sa.bits >= 64 ? ~u64{0} : (u64{1} << sa.bits) - 1;
    u64 raw = w_->values.rec(*v).bits & mask;
    bool negative = sa.is_signed && ((raw >> (sa.bits - 1)) & 1);
    u64 max = sb.is_signed ? (u64{1} << (sb.bits - 1)) - 1 : sb.bits >= 64 ? ~u64{0} : (u64{1} << sb.bits) - 1;
    if (!negative) return raw > max;
    if (!sb.is_signed) return true;
    std::int64_t value = std::int64_t(raw | ~mask);
    std::int64_t min = sb.bits >= 64 ? std::numeric_limits<std::int64_t>::min() : -(std::int64_t{1} << (sb.bits - 1));
    return value < min;
  }

  // ------------------------------------------------------------ scopes and locals (§4.4)

  void push_scope() { scopes_.emplace_back(); }
  void pop_scope() {
    // What the innermost scope's using-declarations, using-directives and aliases brought in ends
    // with it.
    std::size_t inner = scopes_.size() - 1;
    std::erase_if(using_enums_, [&](const auto& u) { return u.first >= inner; });
    std::erase_if(block_usings_, [&](const auto& u) { return u.depth >= inner; });
    std::erase_if(block_directives_, [&](const auto& u) { return u.first >= inner; });
    std::erase_if(block_aliases_, [&](const auto& u) { return u.depth >= inner; });
    std::erase_if(block_types_, [&](const auto& u) { return u.depth >= inner; });
    if (!guarded_.empty() && guarded_.back() == scopes_.size() - 1) guarded_.pop_back();
    scopes_.pop_back();
  }
  std::size_t scope_depth() const { return scopes_.size(); }

  // Declares a block-scope variable or a parameter in the innermost scope ([basic.scope.block]).
  // A name may not be redeclared in its scope, nor in the outermost block of a function body
  // when it names a parameter ([basic.scope.param]).
  // A name may not be redeclared in its scope, nor in the outermost block of a substatement whose
  // enclosing scope is guarded: a function's parameters, a for-init-statement's or a condition's
  // declarations ([basic.scope.block]/2). Parameters are checked by the parser (`check` false).
  u32 declare_local(NameId name, QualType type, u16 flags, u32 at, bool check = true) {
    // [temp.local]/6: a template parameter's name is not redeclared in its scope.
    // (Parameters are checked where the declarator is.)
    if (!(flags & local_flag::parameter) && name != no_name() && template_param(name)) diag(at, Diag::redeclared_local, name);
    if (name != no_name() && check) {
      bool clash = std::ranges::any_of(scopes_.back(), [&](auto& p) { return p.first == name; }) ||
                   std::ranges::any_of(block_types_, [&](auto& t) { return t.depth == scopes_.size() - 1 && t.name == name; });
      if (scopes_.size() >= 2 && !guarded_.empty() && guarded_.back() == scopes_.size() - 2)
        clash = clash || std::ranges::any_of(scopes_[guarded_.back()], [&](auto& p) { return p.first == name; });
      if (clash) diag(at, Diag::redeclared_local, name);
    }
    u32 index = static_cast<u32>(locals_.size());
    locals_.push_back({name, type, EntityId::none, NodeId::none, flags});
    local_values_.push_back(std::nullopt);
    if (name != no_name()) scopes_.back().push_back({name, index});
    return index;
  }
  void set_local_decl(u32 local, NodeId decl) { locals_[local].decl = decl; }
  // An array of unknown bound gets its bound from its initializer ([dcl.array]/8).
  void set_local_type(u32 local, QualType t) { locals_[local].type = t; }
  // A local usable in constant expressions ([expr.const]): constexpr, or const integral with a
  // constant initializer; its value.
  void set_local_value(u32 local, ConstId v) { local_values_[local] = v; }
  std::span<const std::optional<ConstId>> local_values() const { return local_values_; }
  const ast::LocalRec& local(u32 i) const { return locals_[i]; }
  std::span<const ast::LocalRec> locals() const { return locals_; }
  // The function's parameters are in the scope at this depth; its body's outermost block is the
  // next one.
  // Marks the innermost scope as guarded (`declare_local`); it stays so until it is popped.
  void guard_scope() { guarded_.push_back(scopes_.size() - 1); }
  // Hides every block scope, for a part analysed in the class's scope inside a function (a
  // default member initializer in a constructor), and gives them back.
  using Scopes = std::vector<std::vector<std::pair<NameId, u32>>>;
  Scopes take_scopes() { return std::exchange(scopes_, {}); }
  void put_scopes(Scopes s) { scopes_ = std::move(s); }

  NameId no_name() { return ins().names.intern(""); }

  // ------------------------------------------------------------ templates ([temp.param], [temp.res])

  // The template-heads enclosing the current point, outermost first ([basic.scope.temp]), and in
  // an instantiation the arguments bound to their parameters ([temp.inst]).
  std::vector<std::vector<ast::TemplateParamSema>> template_scopes;
  Bindings template_args;
  const ast::TemplateParamSema* template_param(NameId name) const {
    for (auto s = template_scopes.rbegin(); s != template_scopes.rend(); ++s)
      for (const ast::TemplateParamSema& p : *s)
        if (p.name == name) return &p;  // an unnamed parameter's name is never looked up
    return nullptr;
  }
  // What the cell demands of the program (§B1.4), in program order.
  std::vector<Demand> demands;

  Templates templates() { return Templates(*w_, classes()); }
  bool dependent(const Ex& e) {
    return e.ok() && ((e.info.flags & expr_flag::dependent) || types().is_dependent(e.type()));
  }
  bool dependent(const TemplateArg& a) {
    return a.kind == ast::TemplateArgKind::type ? types().is_dependent(a.type) : ins().values.is_dependent(a.value);
  }
  bool any_dependent(std::span<const Ex> es) {
    return std::ranges::any_of(es, [&](const Ex& e) { return dependent(e); });
  }
  bool dependent(const Braced& b) {
    return std::ranges::any_of(b.clauses, [&](const Braced::Clause& c) { return c.list ? dependent(*c.list) : dependent(c.expr); });
  }
  // A dependent construct over `ops` in [begin, end): of type `t` when only its value depends on
  // a template parameter, else of the dependent type.
  Ex dependent_node(std::span<const Ex> ops, u32 begin, u32 end, std::optional<QualType> t = std::nullopt,
                    ValueCategory cat = ValueCategory::prvalue) {
    std::vector<Child> cs;
    for (const Ex& o : ops)
      if (o.ok()) cs.push_back({o.id, o.begin >= begin ? o.begin - begin : 0});
    ExprInfo info{t ? *t : builtin(Builtin::dependent), cat, expr_flag::dependent};
    return node(ast::Dependent{info, b_->list(cs)}, begin, end, info);
  }
  Ex dependent_node(std::initializer_list<Ex> ops, u32 begin, u32 end, std::optional<QualType> t = std::nullopt,
                    ValueCategory cat = ValueCategory::prvalue) {
    return dependent_node(std::span<const Ex>(ops.begin(), ops.size()), begin, end, t, cat);
  }
  // The offsets of the unqualified calls whose candidates depend on the arguments, in a template's
  // definition ([temp.dep.candidate]); an instantiation adds argument-dependent lookup at its point.
  std::vector<u32> dependent_calls;

  // The specialization of function template `tmpl` for `args`, whose type is `type` (§B1.5).
  EntityId specialization(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, QualType type) {
    const EntityRec& t = w_->entities.rec(tmpl.entity);
    EntityRec r{EntityKind::function, t.name, type, t.lang, t.linkage, t.first, t.scope};
    r.templ = tmpl.entity;
    r.args.assign(args.begin(), args.end());
    return w_->entities.entity(std::move(r));
  }

  // Whether unqualified lookup of `name` finds a function template here, so that a `<` after it
  // begins a template argument list ([temp.names]/3).
  bool is_template_name(NameId name) {
    for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s)
      for (auto p = s->rbegin(); p != s->rend(); ++p)
        if (p->first == name) return false;
    if (template_param(name)) return false;
    for (const BlockUsing& u : block_usings_)
      if (u.name == name) return std::ranges::any_of(u.decls, [](const ast::DeclSema& d) { return (d.flags & decl_flag::template_) != 0; });
    for (const OwnDecl& o : own)
      if (o.name == name) return (o.sema.flags & decl_flag::template_) != 0;
    for (DeclId d : q_->visible_decls(name, directives()))
      if (q_->decl_sema(d).flags & decl_flag::template_) return true;
    return false;
  }

  // A template-id naming a function template ([temp.names]): the overload set of `name`, with its
  // explicitly specified template arguments ([temp.arg.explicit]).
  Ex template_id(NameId name, std::vector<TemplateArg> args, u32 begin, u32 end) {
    Ex r = name_expr(name, begin, end);
    if (!r.ok()) return r;
    if (!types().is(r.type(), Builtin::overload_set)) {
      diag(begin, Diag::not_a_template, name);
      return {NodeId::none, begin, end};
    }
    explicit_args_[std::to_underlying(r.id)] = std::move(args);
    return r;
  }

  // ------------------------------------------------------------ the function being defined

  void set_return_type(QualType t) { return_type_ = t; }
  QualType return_type() const { return return_type_; }
  u32 loop_depth = 0;
  // Inside an immediate function context ([expr.const]): a consteval function's body, or the
  // consteval branch of `if consteval`. A call to a consteval function elsewhere is an immediate
  // invocation, evaluated where it appears.
  u32 immediate_context = 0;
  bool is_consteval(EntityId f) const { return consteval_.contains(std::to_underlying(f)); }

  // Names the current item has declared so far in its declaration part; visible after their
  // declarators ([basic.scope.pdecl]).
  struct OwnDecl {
    NameId name;
    ast::DeclSema sema;
  };
  std::vector<OwnDecl> own;

  // ------------------------------------------------------------ member functions ([class.mfct])

  // The class whose member function body is being analysed, and the type of `this` there: none
  // outside a non-static member function ([expr.prim.this]).
  EntityId current_class = EntityId::none;
  std::optional<QualType> this_type;

  Ex this_expr(u32 begin, u32 end) {
    if (!this_type) {
      diag(begin, Diag::this_outside_member);
      return {NodeId::none, begin, end};
    }
    ExprInfo info{*this_type, ValueCategory::prvalue, 0};
    return node(ast::This{info}, begin, end, info);
  }

  // A member of class `c` named `name` ([class.member.lookup], one class, no bases yet): a
  // non-static data member, or a member function or static data member.
  struct Found {
    const FieldRec* field = nullptr;
    u32 index = 0;
    const MemberDeclRec* member = nullptr;
  };
  std::optional<Found> find_member(EntityId c, NameId name) {
    const ClassRec* cr = class_rec(c);
    if (!cr) return std::nullopt;
    for (u32 i = 0; i < cr->fields.size(); ++i)
      if (cr->fields[i].name == name) return Found{&cr->fields[i], i, nullptr};
    for (const MemberDeclRec& m : cr->members)
      if (m.name == name && !m.is_ctor && !m.is_dtor) return Found{nullptr, 0, &m};
    return std::nullopt;
  }
  // The member functions of `c` named `name`: an overload set ([over.match.funcs]).
  std::vector<const MemberDeclRec*> member_functions(EntityId c, NameId name) {
    std::vector<const MemberDeclRec*> r;
    if (const ClassRec* cr = class_rec(c))
      for (const MemberDeclRec& m : cr->members)
        if (m.name == name && m.is_function && !m.is_ctor && !m.is_dtor) r.push_back(&m);
    return r;
  }
  const MemberDeclRec* member_decl(EntityId f) {
    EntityId c = w_->entities.rec(f).scope;
    if (const ClassRec* cr = c == EntityId::none ? nullptr : class_rec(c))
      for (const MemberDeclRec& m : cr->members)
        if (m.entity == f) return &m;
    return nullptr;
  }

  // A member function named without a call yet: `a.f`, `p->f`, `f` or `S::f`, bound to its object
  // (none when there is none to bind, as in a static member function). Only a call may follow
  // ([expr.ref]/6.3.2).
  Ex bound_member(EntityId c, NameId name, std::optional<Ex> object, bool arrow, u32 begin, u32 end) {
    auto cands = member_functions(c, name);
    ExprInfo info{builtin(Builtin::bound_member), ValueCategory::prvalue, 0};
    Ex r = node(ast::Member{info, cands.front()->entity, ~u32{0}, arrow ? 1u : 0u, 0, object ? at(*object, begin) : Child{}},
                begin, end, info);
    bound_[std::to_underlying(r.id)] = {object, std::move(cands)};
    return r;
  }
  bool is_bound_member(const Ex& e) { return e.ok() && types().is(e.type(), Builtin::bound_member); }

  // The implicit member access `name` inside a member function of `c` ([class.mfct.non.static]),
  // or a qualified name `C::name`: none if `c` has no such member.
  std::optional<Ex> class_member(EntityId c, NameId name, u32 begin, u32 end) {
    auto f = find_member(c, name);
    if (!f) {
      // The enumerators of an unscoped enumeration nested in the class are its members.
      if (const ClassRec* cr = class_rec(c))
        for (const auto& [n, t] : cr->types)
          if (is_unscoped_enum(t))
            if (auto r = find_enumerator(types().enum_of(t), name, begin, end)) return r;
      return std::nullopt;
    }
    Ex fail{NodeId::none, begin, end};
    bool has_this = this_type && current_class == c;
    if (f->field) {
      if (!has_this) {
        diag(begin, Diag::member_without_object, name);
        return fail;
      }
      Ex self = this_expr(begin, begin);
      bool ref = types().is_reference(f->field->type);
      QualType t = ref ? types().referee(f->field->type) : types().add_cv(f->field->type, Types::cv(types().pointee(*this_type)));
      ExprInfo info{t, ValueCategory::lvalue, 0};
      return node(ast::Member{info, f->field->entity, f->index, 1, ref ? 1u : 0u, at(self, begin)}, begin, end, info);
    }
    const MemberDeclRec& m = *f->member;
    // A static member, data or function, is named like a namespace-scope entity
    // ([expr.prim.id.qual]): `S::f` can have its address taken or be called.
    if (!m.is_function || (m.is_static && member_functions(c, name).size() == 1)) {
      ExprInfo info{m.type, ValueCategory::lvalue, 0};
      return node(ast::DeclRef{info, m.entity}, begin, end, info);
    }
    std::optional<Ex> object;
    if (has_this) object = this_expr(begin, begin);
    return bound_member(c, name, object, true, begin, end);
  }

  // An enumerator ([dcl.enum]): a prvalue of its enumeration with its value.
  Ex enumerator(EntityId e, QualType t, u32 begin, u32 end) {
    auto v = q_->constant_value(e);
    if (!v) return {NodeId::none, begin, end};  // its enumeration was diagnosed
    return literal(Types::unqualified(t), *v, ValueCategory::prvalue, begin, end);
  }
  // The enumerator `name` of enumeration `e`, if it has one.
  std::optional<Ex> find_enumerator(EntityId e, NameId name, u32 begin, u32 end) {
    if (const EnumRec* r = enum_rec(e))
      for (const EnumeratorRec& x : r->enumerators)
        if (x.name == name) return enumerator(x.entity, types().enum_type(e), begin, end);
    return std::nullopt;
  }

  // `C::name` ([expr.prim.id.qual]) for a class or enumeration C.
  Ex qualified_name(NameId cls, NameId name, u32 begin, u32 name_at, u32 end) {
    Ex fail{NodeId::none, begin, end};
    auto t = class_name(cls, true);
    if (!t) {
      diag(begin, Diag::not_a_type, cls);
      return fail;
    }
    if (types().is_enum(*t)) {
      if (auto r = find_enumerator(types().enum_of(*t), name, begin, end)) return *r;
      diag(name_at, Diag::no_member, name, *t);
      return fail;
    }
    if (!require_complete(*t, begin)) return fail;
    if (auto r = class_member(types().class_of(*t), name, begin, end)) return *r;
    diag(name_at, Diag::no_member, name, *t);
    return fail;
  }

  // A call of a member function ([expr.call], [over.match.funcs]): a static one is an ordinary
  // call; a non-static one passes its object, which must be at least as cv-qualified as the
  // function ([class.this]).
  Ex member_call(Ex callee, std::vector<Ex> args, u32 end, u32 paren_at) {
    u32 begin = callee.begin;
    Ex fail{NodeId::none, begin, end};
    auto m = *as<ast::Member>(callee);
    const Bound& bound = bound_.at(std::to_underlying(callee.id));
    std::optional<Ex> object = bound.object;
    // The object's cv-qualifiers, which a non-static member function must have ([class.this]).
    std::optional<Cv> object_cv;
    if (object) object_cv = Types::cv(m.arrow ? types().pointee(object->type()) : object->type());
    std::vector<Candidate> cands;
    for (const MemberDeclRec* c : bound.cands)
      cands.push_back({c->entity, c->type, c->is_static, c->is_deleted});
    // [class.this]: a non-static member function needs the object's cv-qualifiers.
    if (object_cv && std::ranges::none_of(bound.cands, [&](const MemberDeclRec* c) {
          return c->is_static || cv_within(*object_cv, types().rec(c->type).fn_cv);
        })) {
      diag(begin, Diag::const_member_call, bound.cands.front()->name);
      return fail;
    }
    auto chosen = resolve(cands, args, begin, bound.cands.front()->name, object_cv);
    if (!chosen) return fail;
    const MemberDeclRec* d = bound.cands[*chosen];
    TypeRec fr = types().rec(d->type);
    auto children = call_args(fr, args, begin, paren_at);
    if (!children) return fail;
    ExprInfo info = result_info(fr.inner);
    if (d->is_static) {
      // The object expression is evaluated, then discarded ([expr.ref]).
      Ex call = node(ast::Call{info, d->entity, at(callee, begin), b_->list(*children)}, begin, end, info);
      if (!object) return call;
      Ex discarded = convert_node(*object, ast::ConversionKind::to_void, builtin(Builtin::void_));
      return node(ast::OrderedBinary{info, ast::OrderedOp::comma, at(discarded, begin), at(call, begin)}, begin, end, info);
    }
    if (!object) {
      diag(begin, Diag::member_without_object, d->name);
      return fail;
    }
    Ex o = *object;
    if (!m.arrow) {
      if (!o.glvalue()) {
        diag(paren_at, Diag::not_supported, lex::Tk::l_paren);  // a member call on a temporary
        return fail;
      }
      ExprInfo pi{types().pointer(o.type()), ValueCategory::prvalue, 0};
      o = node(ast::Unary{pi, ast::UnaryOp::address_of, at(o, o.begin)}, o.begin, o.end, pi);
    }
    return node(ast::MemberCall{info, d->entity, at(o, begin), b_->list(*children)}, begin, end, info);
  }

  // ------------------------------------------------------------ overload resolution ([over.match])

  // A candidate function: its entity (none for an implicit trivial special member), its type,
  // whether it takes no implicit object (a non-member or static function), and whether it is
  // deleted.
  struct Candidate {
    EntityId entity;
    QualType type;
    bool no_object;
    bool deleted;
    // A function template specialization's template ([over.match.best]/2.4–2.5), else null.
    const ast::DeclSema* pattern = nullptr;
  };

  // An implicit conversion sequence's ranking ([over.ics.rank]): exact match, promotion,
  // conversion, then a tie-break among exact matches (an added qualification, or a reference
  // to a more cv-qualified type, is worse); `viable` false when there is none.
  struct Ics {
    bool viable = false;
    int rank = 0;
    int sub = 0;              // a qualification conversion among exact matches ([over.ics.rank]/3.2.5)
    bool ref = false;         // a reference binding: the tie-breaks of /3.2.3 and /3.2.6 apply
    bool rvalue_ref = false;  // binding an rvalue reference
    bool to_rvalue = false;   // to an rvalue
    int ref_cv = 0;           // the cv-qualifiers the referred-to type adds
    bool operator<(const Ics& o) const {
      if (rank != o.rank) return rank < o.rank;
      if (ref && o.ref) {
        if (to_rvalue && o.to_rvalue && rvalue_ref != o.rvalue_ref) return rvalue_ref;  // /3.2.3
        return ref_cv < o.ref_cv;                                                         // /3.2.6
      }
      if (!ref && !o.ref) return sub < o.sub;
      return false;
    }
  };
  static Ics binding(Ics v, bool rvalue_ref, bool to_rvalue, int cv) {
    v.ref = true;
    v.rvalue_ref = rvalue_ref;
    v.to_rvalue = to_rvalue;
    v.ref_cv = cv;
    return v;
  }
  static int cv_bits(Cv c) { return std::popcount(unsigned(std::to_underlying(c))); }
  static bool cv_within(Cv a, Cv b) { return (std::to_underlying(a) & ~std::to_underlying(b)) == 0; }
  bool same_unqualified(QualType a, QualType b) { return object_unqualified(a) == object_unqualified(b); }
  // An object type's cv-qualifiers; an array's are its elements' ([basic.type.qualifier]/3).
  Cv object_cv(QualType t) {
    TypeRec r = types().rec(t);
    return r.kind == TypeKind::array ? object_cv(r.inner) : Types::cv(types().canonical(t));
  }
  // The canonical object type without them.
  QualType object_unqualified(QualType t) {
    TypeRec r = types().rec(t);
    if (r.kind == TypeKind::array && object_cv(r.inner) != Cv::none)
      return types().canonical(types().array_dependent(object_unqualified(r.inner), r.bound_value));
    return types().canonical(Types::unqualified(types().canonical(t)));
  }

  // The type `a` has after the lvalue conversions ([conv.lval], [conv.array], [conv.func]).
  QualType value_type(const Ex& a) {
    QualType t = a.type();
    if (!a.glvalue()) return t;
    if (types().is_array(t)) return types().pointer(types().rec(t).inner);
    if (types().is_function(t)) return types().pointer(t);
    return Types::unqualified(t);
  }

  Ics ics(const Ex& a, QualType p) {
    if (!a.ok()) return {};
    if (types().is_reference(p)) {
      TypeRec r = types().rec(p);
      QualType t = r.inner;
      // [dcl.init.ref]: a direct binding to an lvalue (or, for an rvalue reference, an xvalue)
      // of the type, no less qualified.
      bool direct_cat = r.rvalue_ref ? a.cat() == ValueCategory::xvalue : a.cat() == ValueCategory::lvalue;
      if (direct_cat && same_unqualified(a.type(), t) && cv_within(object_cv(a.type()), object_cv(t)))
        return binding({true, 0, 0}, r.rvalue_ref, a.cat() != ValueCategory::lvalue,
                       cv_bits(object_cv(t)) - cv_bits(object_cv(a.type())));
      bool const_ref = object_cv(t) == Cv::const_;
      if (!r.rvalue_ref && !const_ref) return {};
      if (r.rvalue_ref && a.cat() == ValueCategory::lvalue) return {};
      Ics v = ics(a, Types::unqualified(t));  // to a temporary
      if (!v.viable) return v;
      return binding(v, r.rvalue_ref, true, cv_bits(Types::cv(t)));
    }
    QualType to = Types::unqualified(p);
    if (types().is_class(to)) {
      if (initializes(to, a)) return {true, 0, 0};
      return {};
    }
    if (types().is_pointer(to) && is_null_pointer_constant(a)) return {true, 2, 0};
    QualType from = value_type(a);
    if (types().is_class(from) || types().is_void(from)) return {};
    if (types().canonical(from) == types().canonical(to)) return {true, 0, 0};
    if (types().is_enum(to)) return {};
    if (types().is_enum(from)) {
      if (!is_unscoped_enum(from)) return {};
      if (types().is(to, Builtin::bool_)) return {true, 2, 0};
      if (!types().is_integral(to)) return {};
      // [conv.prom]/4, [over.ics.rank]/4.2: with a fixed type, to it is the better promotion.
      if (const EnumRec* er = enum_rec(types().enum_of(from)); er && er->fixed) {
        if (types().canonical(*er->underlying) == types().canonical(to)) return {true, 1, 0};
        if (builtin(promoted(*types().builtin_of(*er->underlying))) == to) return {true, 1, 1};
        return {true, 2, 0};
      }
      bool promotion = builtin(promoted(*types().builtin_of(underlying_of(from)))) == to;
      return {true, promotion ? 1 : 2, 0};
    }
    if (types().is_pointer(from) && types().is_pointer(to))
    {
      if (qualification_convertible(from, to)) return {true, 0, 1};
      return pointer_convertible(from, to) ? Ics{true, 2, 0} : Ics{};
    }
    if (types().is(to, Builtin::bool_) && (types().is_arithmetic(from) || types().is_pointer(from))) return {true, 2, 0};
    if (types().is_integral(from) && types().is_integral(to)) {
      bool promotion = builtin(promoted(*types().builtin_of(from))) == to && !types().is(from, *types().builtin_of(to));
      return {true, promotion ? 1 : 2, 0};
    }
    return {};
  }

  // The best viable candidate for `args` ([over.match.best]), diagnosed at `at` when there is
  // none or more than one, or when it is deleted. `object_cv` is the implicit object's, for
  // non-static member functions.
  std::optional<std::size_t> resolve(std::span<const Candidate> cands, std::span<const Ex> args, u32 at, NameId name,
                                     std::optional<Cv> object_cv, std::optional<QualType> ctor_of = std::nullopt) {
    std::vector<std::pair<std::size_t, std::vector<Ics>>> viable;
    for (std::size_t i = 0; i < cands.size(); ++i) {
      TypeRec fr = types().rec(cands[i].type);
      if (args.size() < fr.params.size() || (args.size() > fr.params.size() && !fr.variadic)) continue;
      std::vector<Ics> seq;
      // The implicit object parameter, first: the object must not be more cv-qualified.
      if (!cands[i].no_object && object_cv) {
        if (!cv_within(*object_cv, fr.fn_cv)) continue;
        seq.push_back(binding({true, 0, 0}, false, false, cv_bits(fr.fn_cv) - cv_bits(*object_cv)));
      } else {
        seq.push_back({true, 0, 0});
      }
      bool ok = true;
      for (std::size_t k = 0; k < args.size() && ok; ++k) {
        Ics c = k < fr.params.size() ? ics(args[k], fr.params[k]) : Ics{true, 3, 0};  // the ellipsis
        ok = c.viable;
        seq.push_back(c);
      }
      if (ok) viable.push_back({i, std::move(seq)});
    }
    auto report = [&](Diag fn, Diag ctor) {
      if (ctor_of) diag(at, ctor, Types::unqualified(*ctor_of));
      else diag(at, fn, name);
    };
    if (viable.empty()) {
      report(Diag::no_viable_function, Diag::no_viable_constructor);
      return std::nullopt;
    }
    // F1 is better than F2 if none of its conversions is worse, and one is better, or else F1 is
    // not a template specialization and F2 is, or both are and F1's template is more specialized
    // ([over.match.best]/2).
    auto better = [&](std::size_t a, std::size_t b) {
      const std::vector<Ics>& x = viable[a].second;
      const std::vector<Ics>& y = viable[b].second;
      bool some = false;
      for (std::size_t k = 0; k < x.size(); ++k) {
        if (y[k] < x[k]) return false;
        some = some || x[k] < y[k];
      }
      if (some) return true;
      const Candidate& f1 = cands[viable[a].first];
      const Candidate& f2 = cands[viable[b].first];
      if (!f1.pattern && f2.pattern) return true;
      if (f1.pattern && f2.pattern) return templates().more_specialized(*f1.pattern, *f2.pattern, args.size());
      return false;
    };
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < viable.size(); ++i) {
      bool wins = true;
      for (std::size_t j = 0; j < viable.size() && wins; ++j)
        if (i != j) wins = better(i, j);
      if (wins) best = i;
    }
    if (!best) {
      report(Diag::ambiguous_call, Diag::ambiguous_constructor);
      return std::nullopt;
    }
    std::size_t c = viable[*best].first;
    if (cands[c].deleted) {
      report(Diag::deleted_function, Diag::deleted_constructor);
      return std::nullopt;
    }
    return c;
  }

  // What a call to a function returning `r` is ([expr.call]): an lvalue for an lvalue reference,
  // an xvalue for an rvalue reference, else a prvalue.
  ExprInfo result_info(QualType r) {
    if (!types().is_reference(r)) return {Types::unqualified(r), ValueCategory::prvalue, 0};
    return {types().referee(r), types().rec(r).rvalue_ref ? ValueCategory::xvalue : ValueCategory::lvalue, 0};
  }

  // ------------------------------------------------------------ constructors and references

  // Initializes a `t` by a constructor from `args` ([over.match.ctor], [dcl.init.general]/17.6):
  // the class's constructors and, where they are implicit, its default and copy constructors.
  // A trivial copy is a copy; `list` forbids narrowing ([over.match.list]); `copy_init` takes only
  // one-argument constructors.
  Ex construct(QualType t, std::vector<Ex> args, u32 begin, u32 end, bool list = false, bool copy_init = false) {
    Ex fail{NodeId::none, begin, end};
    if (std::ranges::any_of(args, [](const Ex& a) { return !a.ok(); })) return fail;
    if (types().is_dependent(t) || any_dependent(args))
      return dependent_node(args, begin, end, types().is_dependent(t) ? std::nullopt : std::optional(Types::unqualified(t)));
    if (!require_complete(t, begin)) return fail;
    QualType ut = Types::unqualified(t);
    EntityId c = types().class_of(ut);
    const ClassRec* cr = class_rec(c);
    if (!cr) return fail;
    std::vector<Candidate> cands;
    std::vector<EntityId> ents;
    bool user_copy = false;
    for (const MemberDeclRec& m : cr->members)
      if (m.is_ctor) {
        TypeRec fr = types().rec(m.type);
        if (copy_init && fr.params.size() != 1) continue;
        cands.push_back({m.entity, m.type, true, m.is_deleted});
        user_copy = user_copy || is_copy_ctor(fr, ut);
      }
    QualType void_ = builtin(Builtin::void_);
    if (!user_copy) {
      QualType p = types().lvalue_ref(types().add_cv(ut, Cv::const_));
      cands.push_back({cr->copy_ctor, types().function(void_, std::span(&p, 1), false, LangLinkage::cxx), true, cr->copy_deleted});
    }
    if (!cr->user_ctors && !copy_init) cands.push_back({EntityId::none, types().function(void_, {}, false, LangLinkage::cxx), true, false});
    auto chosen = resolve(cands, args, begin, no_name(), std::nullopt, ut);
    if (!chosen) return fail;
    const Candidate& k = cands[*chosen];
    TypeRec fr = types().rec(k.type);
    if (k.entity == EntityId::none) {
      // Trivial: a copy of the argument's object, or nothing to construct.
      if (fr.params.empty()) return value_init_aggregate(ut, begin, end);
      Ex a = args[0];
      return a.glvalue() ? convert_node(a, ast::ConversionKind::lvalue_to_rvalue, ut) : a;
    }
    std::vector<Child> cs;
    for (std::size_t i = 0; i < args.size(); ++i) {
      Ex a = i < fr.params.size() ? convert(args[i], fr.params[i], args[i].begin) : variadic_argument(args[i]);
      if (!a.ok()) return fail;
      if (list && i < fr.params.size() && narrows(rvalue(args[i]), fr.params[i])) {
        diag(args[i].begin, Diag::narrowing, value_type(args[i]), Types::unqualified(fr.params[i]));
        return fail;
      }
      cs.push_back({a.id, a.begin >= begin ? a.begin - begin : 0});
    }
    ExprInfo info{ut, ValueCategory::prvalue, 0};
    return node(ast::CtorCall{info, k.entity, 0, b_->list(cs)}, begin, end, info);
  }
  // [class.copy.ctor]/2: a constructor whose only parameter is `C&` or `cv C&`.
  bool is_copy_ctor(const TypeRec& fr, QualType c) {
    return fr.params.size() == 1 && types().is_reference(fr.params[0]) && !types().rec(fr.params[0]).rvalue_ref &&
           same_unqualified(types().referee(fr.params[0]), c);
  }
  // Value-initialization of an aggregate (no constructor to call).
  Ex value_init_aggregate(QualType t, u32 begin, u32 end) {
    if (has_member_inits(t)) return member_inits(t, true, begin, end);
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    return node(ast::ValueInit{info}, begin, end, info);
  }

  // `T(args)` and `T{…}` for a class T ([expr.type.conv]): a prvalue.
  Ex functional_cast(QualType t, std::vector<Ex> args, u32 begin, u32 end) {
    if (types().is_dependent(t) || any_dependent(args)) return construct(t, std::move(args), begin, end);
    // [expr.type.conv]/2: `void()` is a void prvalue; one argument is a cast to the type, a
    // reference included ([expr.cast]); value-initializing a reference, a function or an array is
    // ill-formed or not supported yet.
    if (args.empty() && types().is_void(t)) return void_prvalue(begin, end);
    if (args.size() == 1 && types().is_reference(t)) return bind_reference(args[0], t, begin);
    if (types().is_reference(t) || types().is_function(t) || types().is_array(t)) {
      diag(begin, types().is_reference(t) ? Diag::reference_uninitialized : Diag::not_supported, lex::Tk::l_paren);
      return {NodeId::none, begin, end};
    }
    if (args.empty()) return value_init(t, begin, end);
    // [expr.type.conv]/2: one argument is the cast-expression `(T)e`, as static_cast does it.
    if (!has_user_ctors(t) && args.size() == 1)
      return types().is_class(t) ? convert(args[0], t, begin) : static_cast_to(args[0], t, begin);
    if (!has_user_ctors(t)) {
      diag(begin, Diag::no_viable_constructor, Types::unqualified(t));  // parenthesized aggregate init is not supported
      return {NodeId::none, begin, end};
    }
    return construct(t, std::move(args), begin, end);
  }

  // A void prvalue ([expr.type.conv]/2): `(void)0`.
  Ex void_prvalue(u32 begin, u32 end) {
    QualType i = builtin(Builtin::int_);
    Ex zero = literal(i, w_->values.integer(i, 0), ValueCategory::prvalue, begin, end);
    return convert_node(zero, ast::ConversionKind::to_void, builtin(Builtin::void_));
  }

  // Temporary materialization ([conv.rval]) of the prvalue `v` as an object of type `t`.
  Ex materialize(Ex v, QualType t, bool extended) {
    if (!v.ok()) return v;
    ExprInfo info{t, ValueCategory::xvalue, 0};
    return node(ast::Materialize{info, destructor_of(t), extended ? 1u : 0u, at(v, v.begin)}, v.begin, v.end, info);
  }

  // Binds a reference of type `r` to `e` ([dcl.init.ref]): directly to an lvalue (or, for an
  // rvalue reference, an xvalue) of its type, no less qualified; a const lvalue reference or an
  // rvalue reference also to a temporary initialized from `e`, which lives as long as the
  // reference when `extend` ([class.temporary]/6).
  Ex bind_reference(Ex e, QualType r, u32 at_, bool extend = false) {
    if (!e.ok()) return e;
    if (dependent(e) || types().is_dependent(r)) {
      QualType t = types().referee(r);
      return dependent_node({e}, e.begin, e.end, types().is_dependent(t) ? std::nullopt : std::optional(t),
                            types().rec(r).rvalue_ref ? ValueCategory::xvalue : ValueCategory::lvalue);
    }
    if (is_bound_member(e) || types().is(e.type(), Builtin::overload_set)) return rvalue(e);
    TypeRec rr = types().rec(r);
    QualType t = rr.inner;
    Ex fail{NodeId::none, e.begin, e.end};
    bool direct_cat = rr.rvalue_ref ? e.cat() == ValueCategory::xvalue : e.cat() == ValueCategory::lvalue;
    if (direct_cat && same_unqualified(e.type(), t)) {
      if (cv_within(object_cv(e.type()), object_cv(t))) return extend ? extended(e) : e;
      diag(at_, Diag::bind_drops_qualifiers, e.type(), r);
      return fail;
    }
    bool const_ref = object_cv(t) == Cv::const_;
    if (!rr.rvalue_ref && !const_ref) {
      if (e.cat() == ValueCategory::lvalue && same_unqualified(e.type(), t)) diag(at_, Diag::bind_drops_qualifiers, e.type(), r);
      else diag(at_, Diag::bind_to_rvalue, t);
      return fail;
    }
    // [dcl.init.ref]/5.4.4: an rvalue reference does not bind to an lvalue.
    if (rr.rvalue_ref && e.cat() == ValueCategory::lvalue) {
      diag(at_, Diag::bind_to_lvalue, t);
      return fail;
    }
    if (e.glvalue() && same_unqualified(e.type(), t) && cv_within(object_cv(e.type()), object_cv(t)))
      return extend ? extended(e) : e;
    Ex v = convert(e, Types::unqualified(t), at_);
    if (!v.ok() || v.glvalue()) return v;
    return materialize(v, t, extend);
  }

  // A temporary a reference is bound to through its subobject (`const int& m = A().x`) lives as
  // long as the reference too ([class.temporary]/6): the `Materialize` under `.` and parentheses,
  // rebuilt extended.
  Ex extended(Ex e) {
    if (auto m = as<ast::Materialize>(e))
      return node(ast::Materialize{m->info, m->destructor, 1, m->operand}, e.begin, e.end, m->info);
    if (auto p = as<ast::Paren>(e)) {
      Ex inner = extended({p->operand.id, e.begin + p->operand.at, e.end, {}});
      return node(ast::Paren{p->info, at(inner, e.begin)}, e.begin, e.end, p->info);
    }
    if (auto m = as<ast::Member>(e); m && !m->arrow && !m->reference) {
      Ex inner = extended({m->object.id, e.begin + m->object.at, e.end, {}});
      return node(ast::Member{m->info, m->member, m->field, 0, 0, at(inner, e.begin)}, e.begin, e.end, m->info);
    }
    return e;
  }

  // ------------------------------------------------------------ names ([basic.lookup.unqual])

  // An id-expression naming `name`, at `begin`: the innermost block-scope declaration, else the
  // namespace-scope ones. The `callee` of a call that lookup does not find is a `DepName`: the
  // call decides once it has its arguments ([temp.dep.candidate]).
  Ex name_expr(NameId name, u32 begin, u32 end, bool callee = false) {
    // Block scopes, innermost first: a scope's locals and the names its using-declarations
    // brought in ([namespace.udecl]) hide those of enclosing scopes ([basic.scope.hiding]);
    // using-declarations of one scope make one set.
    for (std::size_t d = scopes_.size(); d-- > 0;) {
      for (auto p = scopes_[d].rbegin(); p != scopes_[d].rend(); ++p)
        if (p->first == name) {
          const ast::LocalRec& l = locals_[p->second];
          ExprInfo info{types().referee(l.type), ValueCategory::lvalue, 0};  // [expr.type]: a reference names its object
          return node(ast::LocalRef{info, p->second}, begin, end, info);
        }
      if (std::ranges::any_of(block_types_, [&](const BlockType& t) { return t.depth == d && t.name == name; })) {
        diag(begin, Diag::type_as_expression, name);  // [expr.prim.id]: a type is not an expression
        return {NodeId::none, begin, end};
      }
      std::vector<ast::DeclSema> used;
      for (const BlockUsing& u : block_usings_)
        if (u.depth == d && u.name == name) used.insert(used.end(), u.decls.begin(), u.decls.end());
      if (!used.empty()) return decls_expr(std::move(used), true, name, begin, end, callee, true);
    }
    // Enumerators a `using enum` brought into a block scope ([enum.udecl]).
    for (auto u = using_enums_.rbegin(); u != using_enums_.rend(); ++u)
      if (auto r = find_enumerator(u->second, name, begin, end)) return *r;
    // A non-type template parameter is a prvalue whose value depends on the argument
    // ([temp.param]/8, [temp.dep.constexpr]).
    if (const ast::TemplateParamSema* p = template_param(name)) {
      if (p->kind == ast::TemplateArgKind::type) {
        diag(begin, Diag::expected_expression);
        return {NodeId::none, begin, end};
      }
      if (auto a = template_args.at(p->depth, p->index))
        return literal(Types::unqualified(a->type), a->value, ValueCategory::prvalue, begin, end);
      ConstId v = ins().intern_value(ast::VDepParam{p->depth, p->index});
      ExprInfo info{Types::unqualified(p->type), ValueCategory::prvalue, expr_flag::dependent};
      return node(ast::Literal{info, v}, begin, end, info);
    }
    // The class's members, in a member function's body ([basic.scope.class]).
    if (current_class != EntityId::none) {
      if (auto r = class_member(current_class, name, begin, end)) return *r;
      if (!class_rec(current_class))
        if (auto m = q_->earlier_member(name)) {
          if (m->kind != ast::DeclKind::variable) {
            diag(begin, Diag::member_without_object, name);
            return {NodeId::none, begin, end};
          }
          ExprInfo info{m->type, ValueCategory::lvalue, 0};
          return node(ast::DeclRef{info, m->entity}, begin, end, info);
        }
    }
    std::vector<ast::DeclSema> found;
    for (auto p = own.rbegin(); p != own.rend(); ++p)
      if (p->name == name) found.push_back(p->sema);
    bool any = !found.empty();
    if (found.empty())
      for (DeclId d : q_->visible_decls(name, directives())) {
        any = true;
        ast::DeclSema s = q_->decl_sema(d);
        if (s.entity != EntityId::none && !is_special(s)) found.push_back(s);  // else its item diagnosed it
      }
    return decls_expr(std::move(found), any, name, begin, end, callee, true);
  }

  // A constructor or destructor: it has no name that lookup finds ([class.ctor]/1, [class.dtor]/1);
  // its definition outside the class still names the class's declaration in the skeleton.
  static bool is_special(const ast::DeclSema& d) {
    return (d.flags & (decl_flag::constructor | decl_flag::destructor)) != 0;
  }

  // The expression naming what lookup found for `name` ([expr.prim.id]): one entity, or an
  // overload set.
  // `unqualified`: named by an unqualified-id, so a call may add argument-dependent lookup
  // ([basic.lookup.argdep]/1); what was found is kept for it.
  Ex decls_expr(std::vector<ast::DeclSema> found, bool any, NameId name, u32 begin, u32 end, bool callee = false,
                bool unqualified = false) {
    std::erase_if(found, [](const ast::DeclSema& d) { return d.kind == ast::DeclKind::namespace_; });
    // A class hidden by a variable or function of the same name ([basic.scope.hiding]/2).
    if (std::ranges::any_of(found, [](const ast::DeclSema& d) { return d.kind != ast::DeclKind::class_; }))
      std::erase_if(found, [](const ast::DeclSema& d) { return d.kind == ast::DeclKind::class_; });
    if (found.empty()) {
      if (!any && callee) {
        ExprInfo info{builtin(Builtin::dependent), ValueCategory::lvalue, 0};
        return node(ast::DepName{info, name}, begin, end, info);
      }
      if (!any) diag(begin, Diag::undeclared, name);
      return {NodeId::none, begin, end};
    }
    // Several functions: an overload set, resolved by the call ([over.match]); the latest
    // declaration of each gives its type.
    const ast::DeclSema& d = found.back();
    std::vector<ast::DeclSema> set;
    for (auto o = found.rbegin(); o != found.rend(); ++o)
      if (!std::ranges::any_of(set, [&](const ast::DeclSema& x) { return x.entity == o->entity; })) set.push_back(*o);
    bool templ = std::ranges::any_of(set, [](const ast::DeclSema& x) { return (x.flags & decl_flag::template_) != 0; });
    if (set.size() > 1 || templ) {
      if (!std::ranges::all_of(set, [](const ast::DeclSema& x) { return x.kind == ast::DeclKind::function; })) {
        diag(begin, Diag::ambiguous_name, name);  // [basic.lookup.general]/1: two entities
        return {NodeId::none, begin, end};
      }
      ExprInfo info{builtin(Builtin::overload_set), ValueCategory::lvalue, 0};
      Ex r = node(ast::DeclRef{info, d.entity}, begin, end, info);
      if (unqualified) unqualified_[std::to_underlying(r.id)] = set;
      overloads_[std::to_underlying(r.id)] = std::move(set);
      return r;
    }
    if (d.kind == ast::DeclKind::function && (d.flags & decl_flag::consteval_))
      consteval_.insert(std::to_underlying(d.entity));
    if (d.kind == ast::DeclKind::function && (d.flags & decl_flag::deleted)) deleted_.insert(std::to_underlying(d.entity));
    if (is_type(d.kind)) {  // [expr.prim.id]: a type is not an expression
      diag(begin, Diag::type_as_expression, name);
      return {NodeId::none, begin, end};
    }
    if (d.kind == ast::DeclKind::enumerator) return enumerator(d.entity, d.type, begin, end);
    if (d.kind == ast::DeclKind::function && ins().names.spelling(name) == "main" &&
        w_->entities.rec(d.entity).scope == EntityId::none) {
      diag(begin, Diag::main_named);  // [basic.start.main]/3: main shall not be used
      return {NodeId::none, begin, end};
    }
    ExprInfo info{types().referee(d.type), ValueCategory::lvalue, 0};
    Ex r = node(ast::DeclRef{info, d.entity}, begin, end, info);
    if (unqualified && d.kind == ast::DeclKind::function) unqualified_[std::to_underlying(r.id)] = {d};
    declared_types_[std::to_underlying(r.id)] = d.type;
    return r;
  }

  // ------------------------------------------------------------ literals ([lex.literal])

  Ex literal(QualType t, ConstId v, ValueCategory cat, u32 begin, u32 end) {
    ExprInfo info{t, cat, 0};
    return node(ast::Literal{info, v}, begin, end, info);
  }

  // [lex.icon]: the first type in the literal's list that can represent its value.
  Ex integer_literal(const lex::Literal& lit, u32 begin, u32 end) {
    bool decimal = lit.base == 10;
    std::vector<Builtin> list;
    auto add = [&](std::initializer_list<Builtin> bs) { list.insert(list.end(), bs); };
    switch (lit.int_suffix) {
    case lex::IntSuffix::none:
      if (decimal) add({Builtin::int_, Builtin::long_, Builtin::long_long});
      else add({Builtin::int_, Builtin::unsigned_int, Builtin::long_, Builtin::unsigned_long, Builtin::long_long,
                Builtin::unsigned_long_long});
      break;
    case lex::IntSuffix::u: add({Builtin::unsigned_int, Builtin::unsigned_long, Builtin::unsigned_long_long}); break;
    case lex::IntSuffix::l:
      if (decimal) add({Builtin::long_, Builtin::long_long});
      else add({Builtin::long_, Builtin::unsigned_long, Builtin::long_long, Builtin::unsigned_long_long});
      break;
    case lex::IntSuffix::ul: add({Builtin::unsigned_long, Builtin::unsigned_long_long}); break;
    case lex::IntSuffix::ll:
      if (decimal) add({Builtin::long_long});
      else add({Builtin::long_long, Builtin::unsigned_long_long});
      break;
    case lex::IntSuffix::ull: add({Builtin::unsigned_long_long}); break;
    case lex::IntSuffix::z:
    case lex::IntSuffix::uz: add({lit.int_suffix == lex::IntSuffix::z ? Builtin::long_ : Builtin::unsigned_long}); break;
    }
    Builtin chosen = Builtin::long_long;
    bool fits = false;
    for (Builtin k : list) {
      IntShape s = shape(k);
      u64 max = s.is_signed ? (u64{1} << (s.bits - 1)) - 1 : (s.bits == 64 ? ~u64{0} : (u64{1} << s.bits) - 1);
      if (!lit.overflow && lit.value <= max) {
        chosen = k;
        fits = true;
        break;
      }
    }
    // A value no listed type holds was diagnosed by the lexer (integer_too_large), or is
    // ill-formed here; `long long` keeps analysis going.
    if (!fits) {
      if (!lit.overflow) diag(begin, Diag::integer_literal_too_large);  // [lex.icon]/3
      chosen = list.back();
    }
    QualType t = builtin(chosen);
    return literal(t, w_->values.integer(t, lit.value), ValueCategory::prvalue, begin, end);
  }

  // [lex.ccon]: an ordinary character literal has type char; a multicharacter one, int.
  Ex char_literal(const lex::Literal& l, u32 begin, u32 end) {
    using enum lex::Encoding;
    Builtin k = Builtin::char_;
    if (l.multichar) k = Builtin::int_;
    else if (l.encoding == utf8) k = Builtin::char8_t_;
    else if (l.encoding == utf16) k = Builtin::char16_t_;
    else if (l.encoding == utf32) k = Builtin::char32_t_;
    else if (l.encoding == wide) k = Builtin::wchar_t_;
    QualType t = builtin(k);
    return literal(t, w_->values.integer(t, l.value), ValueCategory::prvalue, begin, end);
  }

  // [lex.string]: an lvalue of type array of N const char, N counting the terminating null.
  Ex string_literal(const lex::Literal& l, u32 begin, u32 end) {
    if (l.encoding != lex::Encoding::ordinary && l.encoding != lex::Encoding::utf8) {
      diag(begin, Diag::not_supported, lex::Tk::string_lit);
      return {NodeId::none, begin, end};
    }
    std::string units;
    for (u32 u : l.units) units.push_back(static_cast<char>(u));
    units.push_back('\0');
    Builtin elem = l.encoding == lex::Encoding::utf8 ? Builtin::char8_t_ : Builtin::char_;
    QualType t = types().array(types().add_cv(builtin(elem), Cv::const_), units.size());
    return literal(t, w_->values.string(t, std::move(units)), ValueCategory::lvalue, begin, end);
  }

  Ex bool_literal(bool v, u32 begin, u32 end) {
    QualType t = builtin(Builtin::bool_);
    return literal(t, w_->values.integer(t, v), ValueCategory::prvalue, begin, end);
  }

  Ex nullptr_literal(u32 begin, u32 end) {
    QualType t = builtin(Builtin::nullptr_t);
    return literal(t, w_->values.null_pointer(t), ValueCategory::prvalue, begin, end);
  }

  // The value of a static data member of integral type usable in constant expressions.
  std::optional<ConstId> static_constant(const Ex& e) {
    auto d = as<ast::DeclRef>(e);
    if (!d || !types().is_integral(e.type()) || Types::is_volatile(e.type()) ||
        w_->entities.rec(d->entity).scope == EntityId::none)
      return std::nullopt;
    return q_->constant_value(d->entity);
  }

  // An expression statement's expression, a discarded-value expression ([expr.context]).
  Ex discarded(Ex e) {
    if (is_bound_member(e)) {
      diag(e.begin, Diag::bound_member_not_called, w_->entities.rec(as<ast::Member>(e)->member).name);
      return {NodeId::none, e.begin, e.end};
    }
    return e;
  }

  Ex paren(Ex e, u32 begin, u32 end) {
    if (!e.ok()) return {NodeId::none, begin, end};
    // A parenthesized member function name is that name ([expr.prim.paren]): only a call may follow.
    if (is_bound_member(e)) return {e.id, begin, end, e.info};
    // So is a parenthesized overload set, which only a call or an address may follow; it gets no
    // argument-dependent lookup ([basic.lookup.argdep]/1).
    if (types().is(e.type(), Builtin::overload_set)) {
      Ex r = node(ast::DeclRef{e.info, as<ast::DeclRef>(e)->entity}, begin, end, e.info);
      overloads_[std::to_underlying(r.id)] = overloads_.at(std::to_underlying(e.id));
      if (auto ex = explicit_args_.find(std::to_underlying(e.id)); ex != explicit_args_.end())
        explicit_args_[std::to_underlying(r.id)] = ex->second;
      return r;
    }
    return node(ast::Paren{e.info, at(e, begin)}, begin, end, e.info);
  }

  // ------------------------------------------------------------ conversions ([conv])

  Ex convert_node(Ex e, ast::ConversionKind k, QualType to, ValueCategory cat = ValueCategory::prvalue) {
    // An enumerator converted to an integer is that integer's constant: enumeration-typed
    // constants stay out of the IR's arithmetic.
    if (auto lit = as<ast::Literal>(e); lit && types().is_enum(e.type()) &&
                                        (k == ast::ConversionKind::integral || k == ast::ConversionKind::boolean)) {
      u64 bits = w_->values.rec(lit->value).bits;
      if (k == ast::ConversionKind::boolean) bits = bits != 0;
      return literal(to, w_->values.integer(to, bits), ValueCategory::prvalue, e.begin, e.end);
    }
    ExprInfo info{to, cat, 0};
    return node(ast::ImplicitConversion{info, k, EntityId::none, at(e, e.begin)}, e.begin, e.end, info);
  }

  // The conversions every operand that needs a prvalue gets ([basic.lval], [conv.lval],
  // [conv.array], [conv.func]).
  Ex rvalue(Ex e) {
    if (e.ok() && types().is(e.type(), Builtin::overload_set)) {
      diag(e.begin, Diag::overload_address, w_->entities.rec(as<ast::DeclRef>(e)->entity).name);
      return {NodeId::none, e.begin, e.end};
    }
    if (is_bound_member(e)) {
      diag(e.begin, Diag::bound_member_not_called, w_->entities.rec(as<ast::Member>(e)->member).name);
      return {NodeId::none, e.begin, e.end};
    }
    if (!e.ok() || !e.glvalue() || dependent(e)) return e;
    QualType t = e.type();
    // A static data member usable in constant expressions is read as its value, which is no
    // odr-use ([basic.def.odr]), so it needs no definition; through an object (`a.x`, the comma
    // `member` built), the object is still evaluated.
    if (auto v = static_constant(e)) return literal(Types::unqualified(t), *v, ValueCategory::prvalue, e.begin, e.end);
    if (auto c = as<ast::OrderedBinary>(e); c && c->op == ast::OrderedOp::comma && types().is_integral(t)) {
      Ex rhs{c->rhs.id, e.begin + c->rhs.at, e.end, c->info};
      if (auto v = static_constant(rhs)) {
        Ex lit = literal(Types::unqualified(t), *v, ValueCategory::prvalue, rhs.begin, rhs.end);
        ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
        return node(ast::OrderedBinary{info, ast::OrderedOp::comma, c->lhs, at(lit, e.begin)}, e.begin, e.end, info);
      }
    }
    if (types().is_array(t)) {
      QualType p = types().pointer(types().rec(t).inner);
      return convert_node(e, ast::ConversionKind::array_to_pointer, p);
    }
    if (types().is_function(t)) return convert_node(e, ast::ConversionKind::function_to_pointer, types().pointer(t));
    return convert_node(e, ast::ConversionKind::lvalue_to_rvalue, Types::unqualified(t));
  }

  // [conv.prom], after the lvalue conversions.
  Ex promote(Ex e) {
    e = rvalue(e);
    if (!e.ok() || dependent(e)) return e;
    // [conv.prom]/4: an unscoped enumeration promotes as its underlying type does.
    if (is_unscoped_enum(e.type())) {
      QualType u = underlying_of(e.type());
      return convert_node(e, ast::ConversionKind::integral, builtin(promoted(*types().builtin_of(u))));
    }
    auto b = types().builtin_of(e.type());
    if (!b || !is_integral(*b)) return e;
    Builtin p = promoted(*b);
    if (p == *b) return e;
    return convert_node(e, ast::ConversionKind::integral, builtin(p));
  }

  // Enumerations ([dcl.enum]).
  bool is_unscoped_enum(QualType t) {
    if (!types().is_enum(t)) return false;
    const EnumRec* r = enum_rec(types().enum_of(t));
    return !r || !r->scoped;
  }
  bool is_scoped_enum(QualType t) { return types().is_enum(t) && !is_unscoped_enum(t); }
  QualType underlying_of(QualType t) {
    const EnumRec* r = enum_rec(types().enum_of(t));
    return r && r->underlying ? *r->underlying : builtin(Builtin::int_);
  }

  // [conv.ptr]: an integer literal with value zero, or a prvalue of type std::nullptr_t.
  bool is_null_pointer_constant(const Ex& e) {
    if (!e.ok() || e.glvalue()) return false;
    if (types().is(e.type(), Builtin::nullptr_t)) return true;
    NodeId n = e.id;
    while (ast::kind_of(n) == ast::Nodes::index_of<ast::Paren>()) n = b_->get<ast::Paren>(n).operand.id;
    if (ast::kind_of(n) != ast::Nodes::index_of<ast::Literal>()) return false;
    // An integer-literal's type is a signed or unsigned integer type of rank int or above
    // ([lex.icon]); a character or bool literal is not an integer-literal.
    auto b = types().builtin_of(e.type());
    if (!b || !ast::is_integral(*b) || *b == Builtin::bool_ || rank_below_int(*b) || is_character(*b)) return false;
    return w_->values.rec(b_->get<ast::Literal>(n).value).bits == 0;
  }
  static bool is_character(Builtin b) {
    return b == Builtin::char_ || b == Builtin::wchar_t_ || b == Builtin::char8_t_ || b == Builtin::char16_t_ ||
           b == Builtin::char32_t_;
  }
  static bool rank_below_int(Builtin b) {
    return b == Builtin::signed_char || b == Builtin::unsigned_char || b == Builtin::short_ ||
           b == Builtin::unsigned_short;
  }

  // An implicit conversion sequence to `to` ([over.best.ics], [dcl.init.general]): copy-
  // initialization of an object of type `to` from `e`. Standard conversions only.
  Ex convert(Ex e, QualType to, u32 at_) {
    if (!e.ok()) return e;
    if (dependent(e) || types().is_dependent(to)) {
      if (types().is_reference(to)) return bind_reference(e, to, at_);
      return dependent_node({e}, e.begin, e.end, types().is_dependent(to) ? std::nullopt : std::optional(Types::unqualified(to)));
    }
    QualType target = Types::unqualified(to);
    if (types().is_array(target)) {
      if (auto r = string_init(target, e)) return *r;
      diag(at_, Diag::array_initializer);  // [dcl.init.general]/16.5: a list or a string literal
      return {NodeId::none, e.begin, e.end};
    }
    // A class object is copy-initialized from an expression of its own class: a glvalue through
    // the (trivial, implicit) copy constructor, a prvalue directly ([dcl.init.general]/17.6).
    if (types().is_reference(to)) return bind_reference(e, to, at_);
    if (types().is_class(target)) {
      if (!require_complete(target, at_)) return {NodeId::none, e.begin, e.end};
      if (initializes(target, e) && !e.glvalue()) return e;  // the prvalue initializes it directly
      const ClassRec* cr = class_rec(types().class_of(target));
      // A copy through a constructor that is not trivial, or one converting another type
      // ([class.conv.ctor]).
      if (cr && (cr->user_ctors || cr->copy_ctor != EntityId::none || cr->copy_deleted))
        return construct(target, {e}, e.begin, e.end, false, true);
      if (!initializes(target, e)) {
        diag(at_, Diag::no_conversion, e.type(), to);
        return {NodeId::none, e.begin, e.end};
      }
      return convert_node(e, ast::ConversionKind::lvalue_to_rvalue, target);
    }
    if (types().is_pointer(target) && is_null_pointer_constant(e))
      return convert_node(e, ast::ConversionKind::null_pointer, target);
    e = rvalue(e);
    QualType from = e.type();
    // A std::nullptr_t object's value is a null pointer constant too ([conv.ptr]/1).
    if (e.ok() && types().is_pointer(target) && types().is(Types::unqualified(from), Builtin::nullptr_t))
      return convert_node(e, ast::ConversionKind::null_pointer, target);
    // [dcl.enum]: an enumeration is initialized only from its own type; an unscoped one converts
    // to an integer or bool ([conv.integral], [conv.bool]).
    if (types().is_enum(target)) {
      if (e.ok() && types().canonical(Types::unqualified(from)) == types().canonical(target)) return e;
      if (e.ok()) diag(at_, Diag::no_conversion, from, to);
      return {NodeId::none, e.begin, e.end};
    }
    if (e.ok() && is_unscoped_enum(from) && types().is_arithmetic(target))
      return convert_node(e, types().is(target, Builtin::bool_) ? ast::ConversionKind::boolean : ast::ConversionKind::integral, target);
    if (types().is_void(from)) {
      diag(at_, Diag::void_value);
      return {NodeId::none, e.begin, e.end};
    }
    if (from == target) return e;
    if (types().is_arithmetic(from) && types().is_arithmetic(target)) {
      if (types().is(target, Builtin::bool_)) return convert_node(e, ast::ConversionKind::boolean, target);
      return convert_node(e, ast::ConversionKind::integral, target);
    }
    if (types().is(target, Builtin::bool_) && (types().is_pointer(from) || types().is(from, Builtin::nullptr_t))) {
      if (types().is_pointer(from)) return convert_node(e, ast::ConversionKind::boolean, target);
    }
    if (types().is_pointer(from) && types().is_pointer(target) && qualification_convertible(from, target))
      return convert_node(e, ast::ConversionKind::qualification, target);
    if (types().is_pointer(from) && types().is_pointer(target) && pointer_convertible(from, target))
      return convert_node(e, ast::ConversionKind::pointer, target);
    diag(at_, Diag::no_conversion, from, to);
    return {NodeId::none, e.begin, e.end};
  }

  // A conversion as `static_cast<to>(e)` performs it ([expr.static.cast]); a cast in functional
  // notation or a cast-expression is one when static_cast applies ([expr.type.conv]/2,
  // [expr.cast]/4). Downcasts and the const_cast and reinterpret_cast parts are not supported yet.
  Ex static_cast_to(Ex e, QualType to, u32 at_) {
    if (!e.ok()) return e;
    if (dependent(e) || types().is_dependent(to))
      return dependent_node({e}, e.begin, e.end, types().is_dependent(to) ? std::nullopt : std::optional(Types::unqualified(to)));
    QualType target = Types::unqualified(to);
    if (types().is_void(target)) return convert_node(e, ast::ConversionKind::to_void, target);  // /6
    if (types().is_reference(to) || types().is_class(target) || types().is_array(target)) return convert(e, to, at_);
    Ex v = rvalue(e);
    if (!v.ok()) return v;
    QualType from = Types::unqualified(v.type());
    // /10: an integral or enumeration value to an enumeration; /9: a scoped enumeration's to an
    // integral type.
    if (types().is_enum(target) && (types().is_integral(from) || types().is_enum(from))) {
      if (types().canonical(from) == types().canonical(target)) return v;
      // A constant (an enumerator, a literal) is folded to the enumeration's constant: the
      // interpreter has no enumeration-typed values yet (#25).
      if (auto lit = as<ast::Literal>(v))
        return literal(target, w_->values.integer(underlying_of(target), w_->values.rec(lit->value).bits), ValueCategory::prvalue,
                       v.begin, v.end);
      return convert_node(v, ast::ConversionKind::integral, target);
    }
    if (is_scoped_enum(from) && types().is_arithmetic(target))
      return convert_node(v, types().is(target, Builtin::bool_) ? ast::ConversionKind::boolean : ast::ConversionKind::integral, target);
    // /14: a pointer to cv void to a pointer to an object type at least as cv-qualified.
    if (types().is_pointer(from) && types().is_pointer(target) && types().is_void(types().pointee(from)) &&
        types().is_object(types().pointee(target)) && cv_within(object_cv(types().pointee(from)), object_cv(types().pointee(target))))
      return convert_node(v, ast::ConversionKind::pointer, target);
    return convert(v, to, at_);
  }

  // `sizeof` and `alignof` of a type ([expr.sizeof], [expr.alignof]), a `size_t` prvalue; zero
  // after a diagnostic. A class's layout is the back end's (#194).
  Ex size_of(QualType t, bool align, u32 begin, u32 end) {
    QualType st = builtin(target.size_type);
    if (types().is_dependent(t)) return dependent_node({}, begin, end, st);
    t = types().referee(t);  // [expr.sizeof]/2, [expr.alignof]/3
    std::optional<u64> n = layout_of(t, align);
    if (!n) {
      if (types().is_function(t) || !is_complete(t)) diag(begin, Diag::sizeof_incomplete, t);
      else diag(begin, Diag::not_supported, align ? lex::Tk::kw_alignof : lex::Tk::kw_sizeof);
      return {NodeId::none, begin, end};
    }
    return literal(st, w_->values.integer(st, *n), ValueCategory::prvalue, begin, end);
  }
  // A complete non-class object type's size or alignment, from the target.
  std::optional<u64> layout_of(QualType t, bool align) {
    TypeRec r = types().rec(t);
    switch (r.kind) {
    case TypeKind::builtin:
      if (!target.has(r.builtin) || r.builtin == Builtin::void_) return std::nullopt;
      return align ? target[r.builtin].align : target[r.builtin].size;
    case TypeKind::pointer: return align ? target.pointer_align : target.pointer_size;
    case TypeKind::enum_: return is_complete(t) ? layout_of(underlying_of(t), align) : std::nullopt;
    case TypeKind::array: {
      if (r.bound_value == ConstId::none) return std::nullopt;
      auto e = layout_of(r.inner, align);
      return e && !align ? std::optional<u64>(*e * r.bound) : e;
    }
    default: return std::nullopt;  // void, functions, classes (#194)
    }
  }

  // [dcl.type.decltype]/1: an unparenthesized id-expression or member access names its entity's
  // declared type; any other expression has its type, as `T&&` for an xvalue and `T&` for an
  // lvalue.
  QualType decltype_of(Ex e, bool parenthesized) {
    if (!parenthesized) {
      if (auto l = as<ast::LocalRef>(e)) return locals_[l->local].type;
      // The declared type: the declaration's that named the entity (a static data member's is
      // its type, a function's its function type), a data member's from its class.
      if (as<ast::DeclRef>(e)) {
        if (auto it = declared_types_.find(std::to_underlying(e.id)); it != declared_types_.end()) return it->second;
        return e.type();
      }
      if (auto m = as<ast::Member>(e))
        if (const ClassRec* c = class_rec(w_->entities.rec(m->member).scope))
          for (const FieldRec& f : c->fields)
            if (f.entity == m->member) return f.type;
    }
    QualType t = e.type();
    if (e.cat() == ValueCategory::xvalue) return types().rvalue_ref(t);
    if (e.cat() == ValueCategory::lvalue) return types().lvalue_ref(t);
    return t;
  }

  // [conv.ptr]/2: a pointer to an object type converts to a pointer to `void` at least as
  // cv-qualified.
  bool pointer_convertible(QualType from, QualType to) {
    QualType a = types().pointee(from), b = types().pointee(to);
    return types().is_void(b) && types().is_object(a) && cv_within(Types::cv(a), Types::cv(b));
  }

  // [conv.qual]: pointers of equal depth to the same type, where each level of `to` holds the
  // cv-qualifiers of `from`'s, and a level that adds some is preceded only by const levels.
  bool qualification_convertible(QualType from, QualType to) {
    QualType a = from, b = to;
    bool const_so_far = true;
    while (types().is_pointer(a) && types().is_pointer(b)) {
      a = types().pointee(a);
      b = types().pointee(b);
      Cv ca = object_cv(a), cb = object_cv(b);
      if (!cv_within(ca, cb) || (ca != cb && !const_so_far)) return false;
      const_so_far = const_so_far && (std::to_underlying(cb) & 1);
    }
    return !types().is_pointer(a) && !types().is_pointer(b) && same_unqualified(a, b);
  }

  // Contextual conversion to bool ([conv.general], [conv.bool]).
  Ex to_bool(Ex e, u32 at_) {
    e = rvalue(e);
    if (!e.ok()) return e;
    if (dependent(e)) return dependent_node({e}, e.begin, e.end, builtin(Builtin::bool_));
    QualType t = e.type();
    if (types().is(t, Builtin::bool_)) return e;
    if (types().is_arithmetic(t) || types().is_pointer(t) || types().is(t, Builtin::nullptr_t) || is_unscoped_enum(t))
      return convert_node(e, ast::ConversionKind::boolean, builtin(Builtin::bool_));
    diag(at_, Diag::not_contextually_bool, t);
    return {NodeId::none, e.begin, e.end};
  }

  // [expr.arith.conv] for integral operands: both are promoted and converted to the common type.
  QualType arithmetic_conversions(Ex& a, Ex& c) {
    a = promote(a);
    c = promote(c);
    Builtin k = common_integer(*types().builtin_of(a.type()), *types().builtin_of(c.type()));
    QualType t = builtin(k);
    if (a.type() != t) a = convert_node(a, ast::ConversionKind::integral, t);
    if (c.type() != t) c = convert_node(c, ast::ConversionKind::integral, t);
    return t;
  }

  // ------------------------------------------------------------ operators ([expr.unary], [expr.compound])

  bool is_integral_or_unscoped(QualType t) { return types().is_integral(t); }
  // A pointer to a completely-defined object type, which [expr.add] and [expr.pre.incr] need.
  bool is_object_pointer(QualType t) {
    return types().is_pointer(t) && types().is_object(types().pointee(t)) && is_complete(types().pointee(t));
  }

  // [class.copy.assign]/7: the implicit copy assignment of a class with a const member (or one
  // whose own is deleted) is deleted.
  bool has_const_member(QualType t) {
    TypeRec r = types().rec(t);
    if (r.kind == TypeKind::array) return has_const_member(r.inner);
    if (r.kind != TypeKind::class_) return false;
    if (const ClassRec* c = class_rec(r.entity))
      for (const FieldRec& f : c->fields)
        if (Types::is_const(types().canonical(f.type)) || has_const_member(f.type)) return true;
    return false;
  }
  bool check_class_assignable(const Ex& l, u32 at_) {
    if (!types().is_class(l.type()) || !has_const_member(l.type())) return true;
    diag(at_, Diag::const_member_assign, Types::unqualified(l.type()));
    return false;
  }

  // A modifiable lvalue ([basic.lval]): an lvalue of a non-const object type that is not an array.
  bool check_modifiable(const Ex& e, u32 at_) {
    if (!e.glvalue() || types().is_function(e.type()) || types().is_array(e.type())) {
      diag(at_, Diag::not_assignable);
      return false;
    }
    if (Types::is_const(e.type())) {
      diag(at_, Diag::assign_to_const, e.type());
      return false;
    }
    return true;
  }

  Ex unary(ast::UnaryOp op, Ex e, u32 begin, u32 end, u32 op_at) {
    using enum ast::UnaryOp;
    if (!e.ok()) return {NodeId::none, begin, end};
    if (dependent(e)) return dependent_node({e}, begin, end);
    auto make = [&](Ex operand, QualType t, ValueCategory cat) {
      ExprInfo info{t, cat, 0};
      return node(ast::Unary{info, op, at(operand, begin)}, begin, end, info);
    };
    switch (op) {
    case plus:
    case minus:
    case compl_: {
      Ex o = promote(e);
      QualType t = o.type();
      bool ok = op == compl_ ? types().is_integral(t) : types().is_arithmetic(t) || (op == plus && types().is_pointer(t));
      if (!ok) {
        diag(op_at, Diag::invalid_operand, e.type());
        return {NodeId::none, begin, end};
      }
      return make(o, t, ValueCategory::prvalue);
    }
    case not_: {
      Ex o = to_bool(e, e.begin);
      if (!o.ok()) return {NodeId::none, begin, end};
      return make(o, builtin(Builtin::bool_), ValueCategory::prvalue);
    }
    case deref: {
      Ex o = rvalue(e);
      if (!types().is_pointer(o.type()) || types().is_void(types().pointee(o.type()))) {
        diag(op_at, Diag::indirection_not_pointer, e.type());
        return {NodeId::none, begin, end};
      }
      return make(o, types().pointee(o.type()), ValueCategory::lvalue);
    }
    case address_of:
      if (!e.glvalue()) {
        diag(op_at, Diag::not_lvalue_address, e.type());
        return {NodeId::none, begin, end};
      }
      return make(e, types().pointer(e.type()), ValueCategory::prvalue);
    case pre_inc:
    case pre_dec:
    case post_inc:
    case post_dec: {
      // [expr.pre.incr], [expr.post.incr]: a modifiable lvalue of arithmetic type other than
      // bool, or a pointer to a complete object type.
      if (!check_modifiable(e, e.begin)) return {NodeId::none, begin, end};
      QualType t = e.type();
      if (types().is(t, Builtin::bool_)) {
        diag(op_at, Diag::increment_bool);
        return {NodeId::none, begin, end};
      }
      if (!types().is_arithmetic(t) && !is_object_pointer(t)) {
        diag(op_at, Diag::invalid_operand, t);
        return {NodeId::none, begin, end};
      }
      bool pre = op == pre_inc || op == pre_dec;
      return make(e, pre ? t : Types::unqualified(t), pre ? ValueCategory::lvalue : ValueCategory::prvalue);
    }
    }
    std::unreachable();
  }

  enum class BinOp : u8 {
    mul, div, rem, add, sub, shl, shr, lt, gt, le, ge, eq, ne, bit_and, bit_xor, bit_or, logical_and, logical_or, comma,
  };

  Ex binary(BinOp op, Ex l, Ex r, u32 op_at) {
    using enum BinOp;
    u32 begin = l.begin, end = r.end;
    if (!l.ok() || !r.ok()) return {NodeId::none, begin, end};
    if (dependent(l) || dependent(r)) return dependent_node({l, r}, begin, end);
    auto bin = [&](ast::BinaryOp k, Ex a, Ex c, QualType t) {
      ExprInfo info{t, ValueCategory::prvalue, 0};
      return node(ast::Binary{info, k, at(a, begin), at(c, begin)}, begin, end, info);
    };
    auto ordered = [&](ast::OrderedOp k, Ex a, Ex c, ExprInfo info) {
      return node(ast::OrderedBinary{info, k, at(a, begin), at(c, begin)}, begin, end, info);
    };
    auto invalid = [&] {
      diag(op_at, Diag::invalid_operands, l.type(), r.type());
      return Ex{NodeId::none, begin, end};
    };
    switch (op) {
    case logical_and:
    case logical_or: {
      Ex a = to_bool(l, l.begin), c = to_bool(r, r.begin);
      if (!a.ok() || !c.ok()) return {NodeId::none, begin, end};
      return ordered(op == logical_and ? ast::OrderedOp::logical_and : ast::OrderedOp::logical_or, a, c,
                     {builtin(Builtin::bool_), ValueCategory::prvalue, 0});
    }
    case comma: return ordered(ast::OrderedOp::comma, l, r, r.info);
    case shl:
    case shr: {
      Ex a = promote(l), c = promote(r);
      if (!a.ok() || !c.ok()) return {NodeId::none, begin, end};
      if (!types().is_integral(a.type()) || !types().is_integral(c.type())) return invalid();
      return ordered(op == shl ? ast::OrderedOp::shl : ast::OrderedOp::shr, a, c, {a.type(), ValueCategory::prvalue, 0});
    }
    default: break;
    }
    Ex a = rvalue(l), c = rvalue(r);
    if (!a.ok() || !c.ok()) return {NodeId::none, begin, end};
    // [expr.arith.conv]: unscoped enumerations are promoted; a scoped one is compared only with
    // its own type, by value; enumerations do not otherwise mix.
    if (types().is_enum(a.type()) || types().is_enum(c.type())) {
      bool cmp = op == lt || op == gt || op == le || op == ge || op == eq || op == ne;
      if (is_scoped_enum(a.type()) || is_scoped_enum(c.type())) {
        if (!cmp || types().canonical(Types::unqualified(a.type())) != types().canonical(Types::unqualified(c.type())))
          return invalid();
        QualType u = underlying_of(a.type());
        a = convert_node(a, ast::ConversionKind::integral, u);
        c = convert_node(c, ast::ConversionKind::integral, u);
      } else {
        if (types().is_enum(a.type()) && types().is_enum(c.type()) &&
            types().canonical(Types::unqualified(a.type())) != types().canonical(Types::unqualified(c.type())))
          return invalid();  // P2864: arithmetic between different enumerations is ill-formed
        if (types().is_enum(a.type())) a = promote(a);
        if (types().is_enum(c.type())) c = promote(c);
      }
    }
    QualType ta = a.type(), tc = c.type();
    bool arith = types().is_arithmetic(ta) && types().is_arithmetic(tc);
    bool integral = types().is_integral(ta) && types().is_integral(tc);
    static constexpr std::array<ast::BinaryOp, 19> kinds{
        ast::BinaryOp::mul, ast::BinaryOp::div, ast::BinaryOp::rem, ast::BinaryOp::add, ast::BinaryOp::sub,
        ast::BinaryOp::mul, ast::BinaryOp::mul, ast::BinaryOp::lt, ast::BinaryOp::gt, ast::BinaryOp::le,
        ast::BinaryOp::ge, ast::BinaryOp::eq, ast::BinaryOp::ne, ast::BinaryOp::bit_and, ast::BinaryOp::bit_xor,
        ast::BinaryOp::bit_or, ast::BinaryOp::mul, ast::BinaryOp::mul, ast::BinaryOp::mul};
    ast::BinaryOp k = kinds[std::to_underlying(op)];
    switch (op) {
    case mul:
    case div:
      if (!arith) return invalid();
      return bin(k, a, c, arithmetic_conversions(a, c));
    case rem:
    case bit_and:
    case bit_xor:
    case bit_or:
      if (!integral) return invalid();
      return bin(k, a, c, arithmetic_conversions(a, c));
    case add:
    case sub:
      if (arith) return bin(k, a, c, arithmetic_conversions(a, c));
      // [expr.add]: a pointer to a complete object type and an integer; or, for `-`, two
      // pointers to the same type, giving std::ptrdiff_t (long).
      if (is_object_pointer(ta) && types().is_integral(tc)) return bin(k, a, promote(c), ta);
      if (op == add && types().is_integral(ta) && is_object_pointer(tc)) return bin(k, promote(a), c, tc);
      if (op == sub && is_object_pointer(ta) && is_object_pointer(tc) &&
          Types::unqualified(types().pointee(ta)) == Types::unqualified(types().pointee(tc)))
        return bin(k, a, c, builtin(Builtin::long_));
      if (types().is_pointer(ta) && types().is_integral(tc)) {
        diag(op_at, Diag::pointer_arithmetic_void, ta);
        return {NodeId::none, begin, end};
      }
      return invalid();
    case lt:
    case gt:
    case le:
    case ge:
    case eq:
    case ne: {
      QualType b = builtin(Builtin::bool_);
      if (arith) {
        arithmetic_conversions(a, c);
        return bin(k, a, c, b);
      }
      // [expr.rel], [expr.eq]: pointers converted to their composite pointer type; for the
      // equality operators also a pointer and a null pointer constant.
      bool equality = op == eq || op == ne;
      if (types().is_pointer(ta) && types().is_pointer(tc)) {
        QualType pa = types().pointee(ta), pc = types().pointee(tc);
        if (Types::unqualified(pa) != Types::unqualified(pc)) return invalid();
        QualType composite = types().pointer(
            types().add_cv(Types::unqualified(pa), Cv(std::to_underlying(Types::cv(pa)) | std::to_underlying(Types::cv(pc)))));
        if (ta != composite) a = convert_node(a, ast::ConversionKind::qualification, composite);
        if (tc != composite) c = convert_node(c, ast::ConversionKind::qualification, composite);
        return bin(k, a, c, b);
      }
      if (equality && types().is(ta, Builtin::nullptr_t) && types().is(tc, Builtin::nullptr_t)) return bin(k, a, c, b);
      if (equality && types().is_pointer(ta) && is_null_pointer_constant(r))
        return bin(k, a, convert_node(c, ast::ConversionKind::null_pointer, ta), b);
      if (equality && types().is_pointer(tc) && is_null_pointer_constant(l))
        return bin(k, convert_node(a, ast::ConversionKind::null_pointer, tc), c, b);
      return invalid();
    }
    default: break;
    }
    std::unreachable();
  }

  enum class AsgOp : u8 { assign, mul, div, rem, add, sub, shl, shr, bit_and, bit_xor, bit_or };

  // [expr.assign]
  Ex assign(AsgOp op, Ex l, Ex r, u32 op_at) {
    u32 begin = l.begin, end = r.end;
    if (!l.ok() || !r.ok()) return {NodeId::none, begin, end};
    if (dependent(l) || dependent(r)) return dependent_node({l, r}, begin, end, std::nullopt, ValueCategory::lvalue);
    if (!check_modifiable(l, op_at) || !check_class_assignable(l, op_at)) return {NodeId::none, begin, end};
    QualType lt = l.type();
    QualType computation = Types::unqualified(lt);
    Ex c = r;
    if (op == AsgOp::assign) {
      c = convert(r, lt, r.begin);
    } else {
      Ex lv = rvalue(l);  // only to read its type; the IR loads the left operand itself
      QualType tl = lv.type();
      Ex rv = is_unscoped_enum(r.type()) ? promote(r) : rvalue(r);  // [conv.prom]
      if (!rv.ok()) return {NodeId::none, begin, end};
      QualType tr = rv.type();
      bool shift = op == AsgOp::shl || op == AsgOp::shr;
      bool integral_only = shift || op == AsgOp::rem || op == AsgOp::bit_and || op == AsgOp::bit_xor || op == AsgOp::bit_or;
      if (types().is_arithmetic(tl) && types().is_arithmetic(tr) &&
          (!integral_only || (types().is_integral(tl) && types().is_integral(tr)))) {
        if (shift) {
          computation = builtin(promoted(*types().builtin_of(tl)));
          c = promote(rv);
        } else {
          Builtin common = common_integer(promoted(*types().builtin_of(tl)), promoted(*types().builtin_of(tr)));
          computation = builtin(common);
          c = convert(rv, computation, r.begin);
        }
      } else if ((op == AsgOp::add || op == AsgOp::sub) && is_object_pointer(tl) && types().is_integral(tr)) {
        computation = tl;
        c = promote(rv);
      } else {
        diag(op_at, Diag::invalid_operands, lt, r.type());
        return {NodeId::none, begin, end};
      }
    }
    if (!c.ok()) return {NodeId::none, begin, end};
    ExprInfo info{lt, ValueCategory::lvalue, 0};
    return node(ast::Assign{info, ast::AssignOp(std::to_underlying(op)), at(l, begin), at(c, begin), computation}, begin,
                end, info);
  }

  // [expr.assign]/9: `x = {…}` assigns `T{…}`, list-initialized, to an object of type T.
  Ex assign_list(Ex l, const Braced& b, u32 op_at) {
    u32 begin = l.begin, end = b.end;
    if (l.ok() && (dependent(l) || dependent(b))) return dependent_node({l}, begin, end, std::nullopt, ValueCategory::lvalue);
    if (!l.ok() || !check_modifiable(l, op_at) || !check_class_assignable(l, op_at)) return {NodeId::none, begin, end};
    Ex c = list_init(Types::unqualified(l.type()), b);
    if (!c.ok()) return {NodeId::none, begin, end};
    ExprInfo info{l.type(), ValueCategory::lvalue, 0};
    return node(ast::Assign{info, ast::AssignOp::assign, at(l, begin), at(c, begin), Types::unqualified(l.type())},
                begin, end, info);
  }

  // [expr.cond]
  Ex conditional(Ex cond, Ex t, Ex f, u32 q_at) {
    u32 begin = cond.begin, end = f.end;
    if (!cond.ok() || !t.ok() || !f.ok()) return {NodeId::none, begin, end};
    if (dependent(cond) || dependent(t) || dependent(f)) return dependent_node({cond, t, f}, begin, end);
    Ex c = to_bool(cond, cond.begin);
    if (!c.ok()) return {NodeId::none, begin, end};
    ExprInfo info;
    if (t.glvalue() && t.cat() == f.cat() && t.type() == f.type()) {
      info = t.info;
    } else {
      Ex a = rvalue(t), b = rvalue(f);
      QualType ta = a.type(), tb = b.type();
      if (ta == tb) {
        info = {ta, ValueCategory::prvalue, 0};  // [expr.cond]/7.1: the same type, no conversion
      } else if (types().is_void(ta) && types().is_void(tb)) {
        info = {ta, ValueCategory::prvalue, 0};
      } else if (types().is_arithmetic(ta) && types().is_arithmetic(tb)) {
        info = {arithmetic_conversions(a, b), ValueCategory::prvalue, 0};
      } else if (types().is_pointer(ta) && (tb == ta || is_null_pointer_constant(f))) {
        if (tb != ta) b = convert_node(b, ast::ConversionKind::null_pointer, ta);
        info = {ta, ValueCategory::prvalue, 0};
      } else if (types().is_pointer(tb) && is_null_pointer_constant(t)) {
        a = convert_node(a, ast::ConversionKind::null_pointer, tb);
        info = {tb, ValueCategory::prvalue, 0};
      } else if (types().is_pointer(ta) && types().is_pointer(tb) &&
                 (qualification_convertible(ta, tb) || qualification_convertible(tb, ta))) {
        QualType to = qualification_convertible(ta, tb) ? tb : ta;
        if (ta != to) a = convert_node(a, ast::ConversionKind::qualification, to);
        if (tb != to) b = convert_node(b, ast::ConversionKind::qualification, to);
        info = {to, ValueCategory::prvalue, 0};
      } else {
        diag(q_at, Diag::incompatible_operands, t.type(), f.type());
        return {NodeId::none, begin, end};
      }
      t = a;
      f = b;
    }
    return node(ast::Conditional{info, at(c, begin), at(t, begin), at(f, begin)}, begin, end, info);
  }

  // [expr.call]: a call to a named function or through a pointer to function. Each argument
  // copy-initializes its parameter; arguments matching a C-style ellipsis get the default
  // argument promotions.
  Ex call(Ex callee, std::vector<Ex> args, u32 end, u32 paren_at) {
    u32 begin = callee.begin;
    if (!callee.ok() || std::ranges::any_of(args, [](const Ex& a) { return !a.ok(); })) return {NodeId::none, begin, end};
    // A member function called with a dependent argument is resolved when the template is
    // instantiated ([temp.dep.expr]).
    if (is_bound_member(callee) && any_dependent(args)) {
      std::vector<Ex> ops{callee};
      ops.insert(ops.end(), args.begin(), args.end());
      return dependent_node(ops, begin, end);
    }
    if (is_bound_member(callee)) return member_call(callee, std::move(args), end, paren_at);
    // [temp.dep.candidate]: an unqualified call with a type-dependent argument is resolved when the
    // template is instantiated, among what lookup finds at the definition and argument-dependent
    // lookup finds at the point of instantiation.
    std::optional<NameId> unqualified = unqualified_name(callee);
    std::vector<Ex> all{callee};
    all.insert(all.end(), args.begin(), args.end());
    if (unqualified && any_dependent(args)) {
      dependent_calls.push_back(callee.begin);
      return dependent_node(all, begin, end);
    }
    if (unqualified && q_->dependent_call(callee.begin)) callee = with_adl(callee, *unqualified, args);
    if (auto d = as<ast::DepName>(callee)) {
      diag(callee.begin, Diag::undeclared, d->name);
      return {NodeId::none, begin, end};
    }
    if (dependent(callee) || any_dependent(args)) return dependent_node(all, begin, end);
    if (types().is(callee.type(), Builtin::overload_set)) {
      const std::vector<ast::DeclSema>& set = overloads_.at(std::to_underlying(callee.id));
      auto ex = explicit_args_.find(std::to_underlying(callee.id));
      std::span<const TemplateArg> explicit_args;
      if (ex != explicit_args_.end()) explicit_args = ex->second;
      std::vector<ArgShape> shapes;
      for (const Ex& a : args)
        shapes.push_back({a.type(), a.cat() == ValueCategory::lvalue,
                          !types().is(a.type(), Builtin::overload_set) && !is_bound_member(a)});
      std::vector<Candidate> cands;
      std::vector<const ast::DeclSema*> decls;
      for (const ast::DeclSema& d : set) {
        bool deleted = (d.flags & decl_flag::deleted) != 0;
        if (!(d.flags & decl_flag::template_)) {
          // Explicit template arguments name only templates ([temp.arg.explicit]).
          if (ex != explicit_args_.end()) continue;
          cands.push_back({d.entity, d.type, true, deleted});
          decls.push_back(&d);
          continue;
        }
        // A function template contributes the specialization deduction gives ([temp.over]/1),
        // or nothing when deduction fails ([temp.deduct.general]).
        Templates t = templates();
        auto targs = t.deduce_call(d, shapes, explicit_args);
        if (!targs) continue;
        auto st = t.specialization_type(d, *targs);
        if (!st) continue;
        cands.push_back({specialization(d, *targs, *st), *st, true, deleted, &d});
        decls.push_back(&d);
      }
      auto chosen = resolve(cands, args, callee.begin, w_->entities.rec(set.front().entity).name, std::nullopt);
      if (!chosen) return {NodeId::none, begin, end};
      const ast::DeclSema& d = *decls[*chosen];
      const Candidate& k = cands[*chosen];
      if (d.flags & decl_flag::consteval_) consteval_.insert(std::to_underlying(k.entity));
      // A specialization called is one whose definition the program requires ([temp.inst]/8).
      if (k.pattern) demands.push_back({k.entity, ast::Loc{item_, loc_base_ + callee.begin}});
      ExprInfo ci{k.type, ValueCategory::lvalue, 0};
      callee = node(ast::DeclRef{ci, k.entity}, callee.begin, callee.end, ci);
    } else if (auto d = as<ast::DeclRef>(callee); d && deleted_.contains(std::to_underlying(d->entity))) {
      // One function: still not callable when deleted ([dcl.fct.def.delete]).
      diag(callee.begin, Diag::deleted_function, w_->entities.rec(d->entity).name);
      return {NodeId::none, begin, end};
    }
    EntityId function = EntityId::none;
    QualType ft = callee.type();
    if (auto d = as<ast::DeclRef>(callee); d && types().is_function(ft)) {
      function = d->entity;
    } else {
      callee = rvalue(callee);
      ft = callee.type();
      if (types().is_pointer(ft) && types().is_function(types().pointee(ft))) ft = types().pointee(ft);
    }
    if (!types().is_function(ft)) {
      diag(paren_at, Diag::not_callable, callee.type());
      return {NodeId::none, begin, end};
    }
    TypeRec fr = types().rec(ft);
    auto children = call_args(fr, args, begin, paren_at);
    if (!children) return {NodeId::none, begin, end};
    ExprInfo info = result_info(fr.inner);
    return node(ast::Call{info, function, at(callee, begin), b_->list(*children)}, begin, end, info);
  }

  // The name of a callee named by an unqualified-id: an undeclared name, an overload set, or a
  // function that is not a class member ([basic.lookup.argdep]/1: not parenthesized, not
  // qualified).
  std::optional<NameId> unqualified_name(const Ex& callee) {
    if (auto d = as<ast::DepName>(callee)) return d->name;
    auto r = as<ast::DeclRef>(callee);
    if (!r || !unqualified_.contains(std::to_underlying(callee.id))) return std::nullopt;
    const EntityRec& e = w_->entities.rec(r->entity);
    if (e.scope != EntityId::none && w_->entities.rec(e.scope).kind == EntityKind::class_) return std::nullopt;
    return e.name;
  }

  // The namespace associated with a type ([basic.lookup.argdep]/3): the innermost namespace
  // enclosing its class or enumeration, through pointers, arrays and references; none for a
  // fundamental type. The global namespace is `EntityId::none`.
  std::optional<EntityId> associated_namespace(QualType t) {
    TypeRec r = types().rec(t);
    while (r.kind == TypeKind::pointer || r.kind == TypeKind::array || r.kind == TypeKind::reference) r = types().rec(r.inner);
    if (r.kind != TypeKind::class_ && r.kind != TypeKind::enum_) return std::nullopt;
    EntityId s = w_->entities.rec(r.entity).scope;
    while (s != EntityId::none && w_->entities.rec(s).kind != EntityKind::namespace_) s = w_->entities.rec(s).scope;
    return s;
  }

  // The callee of a call that was dependent in the template's definition, in an instantiation:
  // the functions the definition found, with those named `name` that argument-dependent lookup
  // finds in the arguments' associated namespaces at the point of instantiation
  // ([temp.dep.candidate], [basic.lookup.argdep]/4).
  Ex with_adl(Ex callee, NameId name, std::span<const Ex> args) {
    std::vector<EntityId> namespaces;
    for (const Ex& a : args)
      if (auto ns = associated_namespace(a.type()); ns && !std::ranges::contains(namespaces, *ns)) namespaces.push_back(*ns);
    if (namespaces.empty()) return callee;
    std::vector<ast::DeclSema> set;
    if (auto it = unqualified_.find(std::to_underlying(callee.id)); it != unqualified_.end()) set = it->second;
    for (EntityId ns : namespaces)
      for (const ast::DeclSema& d : q_->members_at_instantiation(ns, name))
        if (d.kind == ast::DeclKind::function && !std::ranges::any_of(set, [&](const ast::DeclSema& x) { return x.entity == d.entity; }))
          set.push_back(d);
    if (set.empty()) return callee;
    ExprInfo info{builtin(Builtin::overload_set), ValueCategory::lvalue, 0};
    Ex r = node(ast::DeclRef{info, set.front().entity}, callee.begin, callee.end, info);
    if (auto ex = explicit_args_.find(std::to_underlying(callee.id)); ex != explicit_args_.end())
      explicit_args_[std::to_underlying(r.id)] = ex->second;
    overloads_[std::to_underlying(r.id)] = std::move(set);
    return r;
  }

  // The arguments of a call to a function of type `fr`, each copy-initializing its parameter.
  std::optional<std::vector<Child>> call_args(const TypeRec& fr, std::span<const Ex> args, u32 begin, u32 paren_at) {
    std::vector<QualType> params = fr.params;  // copied: converting may intern new types
    if (args.size() < params.size()) {
      diag(paren_at, Diag::too_few_arguments, u64(params.size()), u64(args.size()));
      return std::nullopt;
    }
    if (args.size() > params.size() && !fr.variadic) {
      diag(args[params.size()].begin, Diag::too_many_arguments, u64(params.size()), u64(args.size()));
      return std::nullopt;
    }
    std::vector<Child> children;
    for (std::size_t i = 0; i < args.size(); ++i) {
      Ex a = i < params.size() ? convert(args[i], params[i], args[i].begin) : variadic_argument(args[i]);
      if (!a.ok()) return std::nullopt;
      children.push_back(at(a, begin));
    }
    return children;
  }

  // [expr.call]/12: an argument matched by the ellipsis gets the lvalue conversions and the
  // integral promotions; std::nullptr_t becomes void*; void is no value.
  Ex variadic_argument(Ex e) {
    e = promote(e);
    if (!e.ok()) return e;
    if (types().is_void(e.type())) {
      diag(e.begin, Diag::void_value);
      return {NodeId::none, e.begin, e.end};
    }
    if (types().is(e.type(), Builtin::nullptr_t))
      return convert_node(e, ast::ConversionKind::null_pointer, types().pointer(builtin(Builtin::void_)));
    return e;
  }

  // [expr.sub]: `E1[E2]` is `*(E1 + E2)`. The α subset takes the pointer on the left only.
  Ex subscript(Ex base, Ex index, u32 end, u32 at_) {
    u32 begin = base.begin;
    if (!base.ok() || !index.ok()) return {NodeId::none, begin, end};
    if (dependent(base) || dependent(index)) return dependent_node({base, index}, begin, end, std::nullopt, ValueCategory::lvalue);
    Ex a = rvalue(base), c = is_unscoped_enum(index.type()) ? promote(index) : rvalue(index);
    if (!is_object_pointer(a.type()) || !types().is_integral(c.type())) {
      diag(at_, Diag::invalid_operands, base.type(), index.type());
      return {NodeId::none, begin, end};
    }
    ExprInfo info{types().pointee(a.type()), ValueCategory::lvalue, 0};
    return node(ast::Subscript{info, at(a, begin), at(promote(c), begin)}, begin, end, info);
  }

private:
  World* w_;
  ast::Builder* b_;
  Questions* q_;
  std::vector<engine::Diagnostic>* diags_;
  ast::ItemId item_;
  u32 loc_base_;
  std::vector<ast::LocalRec> locals_;
  std::vector<std::optional<ConstId>> local_values_;  // by local
  std::set<u32> consteval_;                           // consteval functions named so far
  std::map<u32, std::vector<Ex>> list_elems_;         // by draft InitList
  struct Bound {
    std::optional<Ex> object;
    std::vector<const MemberDeclRec*> cands;
  };
  std::map<u32, Bound> bound_;                        // by draft bound-member Member
  std::set<u32> deleted_;                             // deleted functions named so far
  std::vector<std::pair<std::size_t, EntityId>> using_enums_;  // (scope depth, enumeration)
  struct BlockUsing {
    std::size_t depth;
    NameId name;
    std::vector<ast::DeclSema> decls;
  };
  struct BlockAlias {
    std::size_t depth;
    NameId name;
    EntityId ns;
  };
  std::vector<BlockUsing> block_usings_;
  std::vector<std::pair<std::size_t, EntityId>> block_directives_;  // (scope depth, namespace)
  std::vector<BlockAlias> block_aliases_;
  struct BlockType {
    std::size_t depth;
    NameId name;
    QualType type;
  };
  std::vector<BlockType> block_types_;
  std::map<u32, QualType> declared_types_;  // by DeclRef node: the declared type of what it names
  std::map<u32, std::vector<ast::DeclSema>> overloads_;  // by draft overload-set DeclRef
  std::map<u32, std::vector<TemplateArg>> explicit_args_;  // by draft overload-set DeclRef
  std::map<u32, std::vector<ast::DeclSema>> unqualified_;  // by draft DeclRef named by an unqualified-id
  std::vector<std::vector<std::pair<NameId, u32>>> scopes_;
  std::vector<std::size_t> guarded_;
  QualType return_type_{};
};

}  // namespace ycxx::sema
