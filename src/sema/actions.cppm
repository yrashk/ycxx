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

// A pack a pattern names ([temp.variadic]): a template parameter pack by depth and index, or a
// function parameter pack by name (`fn`).
struct PackRef {
  u16 depth = 0;
  u16 index = 0;
  NameId fn{};
  bool operator==(const PackRef&) const = default;
};
// A pack expansion of a definition, by the offset of its pattern's first token: the offset just
// past its pattern, and the packs its pattern names. An instantiation analyses the pattern once
// per element (ast.md §6.4).
struct Expansion {
  u32 begin = 0;
  u32 end = 0;
  std::vector<PackRef> packs;
};

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
  // The type of variable template specialization `v` at the current point when it is not the
  // template's with the arguments substituted: an explicit specialization's, or a partial
  // specialization's that it is instantiated from ([temp.expl.spec], [temp.spec.partial]).
  virtual std::optional<QualType> specialization_type(EntityId /*v*/) { return std::nullopt; }
  // Whether normal form `c` is satisfied here ([temp.constr.sat]); none while it is dependent.
  virtual std::optional<bool> satisfied(ConstId /*c*/) { return true; }
  // Whether checking normal form `c`'s satisfaction raised a hard error (an atom not of type
  // bool, not constant, or not supported): the expression that asked is then invalid.
  virtual bool satisfaction_failed(ConstId /*c*/) { return false; }
  // A class template's static data member whose initializer is value-dependent; and a use of a
  // specialization's static data member, which instantiates its initializer ([temp.inst]/3).
  // The declaration of template `t` here: its head and, for an alias template, its type.
  virtual std::optional<ast::DeclSema> template_decl(EntityId /*t*/) { return std::nullopt; }
  virtual bool value_dependent_member(EntityId /*v*/) { return false; }
  // False when its initializer was in error, which was reported: what uses it says nothing more.
  virtual bool use_static_member(EntityId /*v*/) { return true; }
  // Whether class `c`, complete or not, was declared with the class-key `union`.
  virtual bool is_union_class(EntityId /*c*/) { return false; }
  // In an instantiation: the pack expansions the definition of what it re-analyses recorded.
  virtual const std::vector<Expansion>* pattern_expansions() { return nullptr; }
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
namespace param_flag {
inline constexpr u16 pack_element = 1;  // an element of a function parameter pack ([temp.variadic]/8)
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
inline constexpr u16 specialization = 8192;  // an explicit specialization ([temp.expl.spec])
inline constexpr u16 partial = 16384;  // a partial specialization ([temp.spec.partial]), with `template_`
inline constexpr u16 friend_ = 32768;  // a friend declaration ([class.friend]): `entity` is the friend
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
    engine::Diagnostic x{engine::Loc{std::to_underlying(item_), loc_base_ + at}, diag_id(d), {arg(args)...}};
    // The elements of an expansion say an equal diagnostic once (ast.md §6.4).
    if (!frames_.empty() && std::ranges::contains(*diags_, x)) return;
    diags_->push_back(std::move(x));
  }
  std::size_t diag_count() const { return diags_->size(); }
  // Drops the diagnostics after the first `n`: a part analysed again where it was diagnosed already.
  void truncate_diags(std::size_t n) { diags_->resize(n); }
  // Drops the diagnostics after the first `n` but those that `survives_substitution` keeps: a
  // substitution failure makes something false, not ill-formed.
  void drop_failures(std::size_t n) {
    auto kept = std::ranges::remove_if(diags_->begin() + n, diags_->end(),
                                       [](const engine::Diagnostic& d) { return !survives_substitution(d.id); });
    diags_->erase(kept.begin(), kept.end());
  }

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
      if (auto a = bound_arg(*p)) return a->type;
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
        for (const auto& [n, t, te] : cr->types)
          if (n == name) return t;
      } else if (auto m = q_->earlier_member(name); m && (m->kind == ast::DeclKind::enum_ || m->kind == ast::DeclKind::type_alias)) {
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
    if (!found && !elaborated && aliases) return builtin_type_name(name);
    if (!found || !is_type(found->kind)) return std::nullopt;
    if (found->kind == ast::DeclKind::type_alias && !aliases) return std::nullopt;
    // A class template's name needs its arguments outside the template ([temp.names]).
    if (found->flags & decl_flag::template_) return std::nullopt;
    return found->type;
  }
  static bool is_type(ast::DeclKind k) {
    return k == ast::DeclKind::class_ || k == ast::DeclKind::enum_ || k == ast::DeclKind::type_alias;
  }
  // A typedef-name the implementation declares where nothing else does (docs/EXTENSIONS.md):
  // `__builtin_va_list`, the type of the SDK's `__darwin_va_list` and libycxx's `va_list`.
  std::optional<QualType> builtin_type_name(NameId name) {
    if (ins().names.spelling(name) != "__builtin_va_list") return std::nullopt;
    switch (target.va_list) {
    case ast::VaList::char_pointer: return types().pointer(builtin(Builtin::char_));
    case ast::VaList::sysv_x86_64: return std::nullopt;  // no `__va_list_tag` yet: not a type
    }
    std::unreachable();
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

  // Whether `name` is a member of the class whose member is being analysed, which hides what
  // the enclosing namespaces declare ([basic.scope.class], [basic.lookup.unqual]).
  bool names_member(NameId name) {
    if (current_class == EntityId::none) return false;
    if (const ClassRec* r = class_rec(current_class))
      return std::ranges::any_of(r->fields, [&](const FieldRec& f) { return f.name == name; }) ||
             std::ranges::any_of(r->members, [&](const MemberDeclRec& m) { return m.name == name; });
    return q_->earlier_member(name).has_value();
  }

  // The class template unqualified lookup finds for `name`, which a `<` after it begins the
  // arguments of ([temp.names]/3); none if it finds anything else.
  std::optional<ast::DeclSema> class_template(NameId name) { return named_template(name, ast::DeclKind::class_); }
  // A template-name that names a type with its arguments ([temp.names]): a class template, or an
  // alias template ([temp.alias]).
  std::optional<ast::DeclSema> type_template(NameId name) {
    if (auto c = class_template(name)) return c;
    return named_template(name, ast::DeclKind::type_alias);
  }
  std::optional<ast::DeclSema> namespace_type_template(EntityId ns, NameId name) {
    if (auto c = namespace_class_template(ns, name)) return c;
    return namespace_template(ns, name, ast::DeclKind::type_alias);
  }
  // The variable template unqualified lookup finds for `name` ([temp.pre]/1), likewise.
  std::optional<ast::DeclSema> variable_template(NameId name) { return named_template(name, ast::DeclKind::variable); }
  // The template of kind `kind` unqualified lookup finds for `name`, with the default arguments of
  // every earlier declaration of it ([temp.param]/12).
  std::optional<ast::DeclSema> named_template(NameId name, ast::DeclKind kind) {
    if (names_member(name)) return std::nullopt;
    for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s)
      for (auto p = s->rbegin(); p != s->rend(); ++p)
        if (p->first == name) return std::nullopt;
    if (template_param(name)) return std::nullopt;
    std::optional<ast::DeclSema> found;
    // Block-scope using-declarations, innermost first ([namespace.udecl]).
    for (auto u = block_usings_.rbegin(); u != block_usings_.rend() && !found; ++u)
      if (u->name == name)
        for (const ast::DeclSema& d : u->decls)
          if (d.kind == kind && (d.flags & decl_flag::template_)) found = d;
    for (auto p = own.rbegin(); p != own.rend() && !found; ++p)
      if (p->name == name) found = p->sema;
    std::vector<ast::DeclSema> decls;
    for (DeclId d : q_->visible_decls(name, directives())) {
      ast::DeclSema s = q_->decl_sema(d);
      if (s.entity != EntityId::none && !is_special(s)) decls.push_back(s);
    }
    if (!found && !decls.empty()) found = decls.back();
    if (!found || found->kind != kind || !(found->flags & decl_flag::template_)) return std::nullopt;
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
    return namespace_template(ns, name, ast::DeclKind::class_);
  }
  // The template of kind `kind` named `name` that is a member of namespace `ns`.
  std::optional<ast::DeclSema> namespace_template(EntityId ns, NameId name, ast::DeclKind kind) {
    for (const ast::DeclSema& d : q_->namespace_members(ns, name))
      if (d.kind == kind && (d.flags & decl_flag::template_)) return d;
    return std::nullopt;
  }

  // The specialization of class template `templ` for `args` ([temp.spec.general]): its entity is
  // the template and the canonical arguments (§B1.5). With dependent arguments it is a dependent
  // type ([temp.dep.type]).
  EntityId class_specialization(EntityId templ, std::span<const TemplateArg> args) {
    return templates().specialize_class(templ, args);
  }

  // `X<args>` naming a class template specialization ([temp.names], [temp.arg]), or the type an
  // alias template specialization stands for: its type with the arguments substituted
  // ([temp.alias]/2).
  std::optional<QualType> class_template_id(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, u32 at) {
    auto full = template_arguments(tmpl, args, at);
    if (!full) return std::nullopt;
    if (tmpl.kind == ast::DeclKind::type_alias) {
      std::vector<ast::TemplateParamSema> params = ins().seqs.elements(tmpl.template_params);
      auto t = templates().substitute(tmpl.type, Bindings::of(params.empty() ? 0 : params[0].depth, *full));
      // Past the nesting limit, which was diagnosed, it says nothing more ([temp.inst]/16).
      if (!t && !q_->instantiation_gave_up()) diag(at, Diag::template_args_mismatch, w_->entities.rec(tmpl.entity).name);
      return t;
    }
    return types().class_type(class_specialization(tmpl.entity, *full));
  }

  // `v<args>` naming a variable template specialization ([temp.pre], [temp.inst]/7): an lvalue of
  // the specialization, of the template's type with the arguments substituted. Its entity is the
  // template and the canonical arguments (§B1.5), as a function template specialization's is.
  Ex variable_template_id(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, u32 begin, u32 end) {
    auto full = template_arguments(tmpl, args, begin);
    if (!full) return {NodeId::none, begin, end};
    std::vector<ast::TemplateParamSema> params = ins().seqs.elements(tmpl.template_params);
    auto t = templates().substitute(tmpl.type, Bindings::of(params.empty() ? 0 : params[0].depth, *full));
    const EntityRec& r = w_->entities.rec(tmpl.entity);
    if (!t) {
      diag(begin, Diag::template_args_mismatch, r.name);
      return {NodeId::none, begin, end};
    }
    EntityId spec = variable_specialization(tmpl.entity, *full);
    if (auto st = q_->specialization_type(spec)) t = *st;
    demands.push_back({spec, ast::Loc{item_, loc_base_ + begin}});
    ExprInfo info{types().referee(*t), ValueCategory::lvalue, 0};
    return node(ast::DeclRef{info, spec}, begin, end, info);
  }
  // The specialization of variable template `templ` for `args` ([temp.spec.general]).
  EntityId variable_specialization(EntityId templ, std::span<const TemplateArg> args) {
    const EntityRec& t = w_->entities.rec(templ);
    EntityRec r{EntityKind::variable, t.name, {}, t.lang, t.linkage, t.first, t.scope};
    r.templ = templ;
    r.args.assign(args.begin(), args.end());
    return w_->entities.entity(std::move(r));
  }

  // The arguments of a template-id for `tmpl` ([temp.arg]): each of its parameter's kind, values
  // converted to the parameter's type, defaults for the rest; none, diagnosed, if they do not fit.
  std::optional<std::vector<TemplateArg>> template_arguments(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, u32 at) {
    std::vector<ast::TemplateParamSema> params = ins().seqs.elements(tmpl.template_params);
    NameId name = w_->entities.rec(tmpl.entity).name;
    Templates t = templates();
    // An expansion only for a pack parameter: for others it is not supported yet (#88).
    std::size_t first_pack = std::ranges::find_if(params, [](const ast::TemplateParamSema& p) { return p.pack != 0; }) - params.begin();
    for (std::size_t i = 0; i < args.size() && i < first_pack; ++i)
      if (t.is_expansion(args[i])) {
        diag(at, Diag::not_supported, lex::Tk::ellipsis);
        return std::nullopt;
      }
    std::vector<std::optional<TemplateArg>> out(params.size());
    if (!t.explicit_into(params, args, out)) {
      diag(at, Diag::template_args_mismatch, name);
      return std::nullopt;
    }
    auto full = t.complete(tmpl, params, out);
    if (!full) diag(at, Diag::template_args_mismatch, name);
    return full;
  }

  // `using enum E;` in a block ([enum.udecl]): E's enumerators are found in this scope.
  void using_enum(QualType t, u32 at_) {
    if (!types().is_enum(t)) {
      diag(at_, Diag::not_a_type, no_name());
      return;
    }
    using_enums_.push_back({scopes_.size() - 1, types().enum_of(t)});
  }

  // ------------------------------------------------------------ concepts ([temp.concept], [temp.constr])

  // [temp.func.order]/6.2: equivalent template-heads, and function parameters of the same types
  // position by position (in terms of each one's own parameters, which are positional).
  bool equivalent_for_constraints(const ast::DeclSema& f1, const ast::DeclSema& f2) {
    std::vector<ast::TemplateParamSema> p1 = ins().seqs.elements(f1.template_params), p2 = ins().seqs.elements(f2.template_params);
    if (p1.size() != p2.size()) return false;
    for (std::size_t i = 0; i < p1.size(); ++i)
      if (p1[i].kind != p2[i].kind || p1[i].pack != p2[i].pack) return false;
    TypeRec t1 = types().rec(f1.type), t2 = types().rec(f2.type);
    if (t1.params.size() != t2.params.size()) return false;
    for (std::size_t i = 0; i < t1.params.size(); ++i)
      if (types().canonical(t1.params[i]) != types().canonical(t2.params[i])) return false;
    return true;
  }

  Questions& questions() { return *q_; }
  u32 loc_base() const { return loc_base_; }
  // Whether this is a templated context: a template's definition or one of its instantiations.
  bool templated() const { return !template_scopes.empty() || instantiating(); }
  // The concept unqualified lookup finds for `name`.
  std::optional<ast::DeclSema> concept_named(NameId name) { return named_template(name, ast::DeclKind::concept_); }
  // The type-constraints of the template-head being read, in normal form ([temp.constr.decl]/3).
  std::vector<ConstId> head_constraints;
  // An atomic constraint ([temp.constr.atomic]): the expression at [begin, end) of this part, whose
  // parameter mapping takes the innermost head's parameters among `names` (the identifiers it
  // contains) to themselves, the others to none.
  ConstId atom(u32 begin, u32 end, std::span<const NameId> names) {
    const bool fn_params = std::ranges::any_of(names, [&](NameId n) { return local_named(n); });
    u16 depth = 0;
    std::vector<ConstId> mapping;
    if (!template_scopes.empty() && !template_scopes.back().empty()) {
      const std::vector<ast::TemplateParamSema>& head = template_scopes.back();
      depth = head[0].depth;
      Templates t = templates();
      for (const ast::TemplateParamSema& p : head) {
        if (!fn_params && !std::ranges::contains(names, p.name)) {
          mapping.push_back(ConstId::none);
          continue;
        }
        // Bound in an instantiation (a nested-requirement of a requires-expression being
        // re-analysed): its argument; otherwise the parameter itself.
        auto bound = template_args.at(p.depth, p.index);
        TemplateArg a = bound                                     ? *bound
                        : p.kind == ast::TemplateArgKind::type ? t.type_arg(p.type)
                                                               : t.value_arg(p.type, ins().intern_value(ast::VDepParam{p.depth, p.index}));
        mapping.push_back(t.mapping_value(a));
      }
    }
    std::vector<u32> calls;
    for (u32 c : dependent_calls)
      if (c >= begin && c < end) calls.push_back(c - begin);
    return ins().intern_value(ast::VAtom{item_, loc_base_ + begin, loc_base_ + end, depth, ins().intern_seq(std::span<const ConstId>(mapping)),
                                         ins().intern_seq(std::span<const u32>(calls)), fn_params});
  }
  // `C<args>` in normal form: C's with the arguments substituted into its atoms' mappings
  // ([temp.constr.normal]/1.4); none, diagnosed, when they do not fit or a mapping cannot be formed.
  std::optional<ConstId> concept_constraint(const ast::DeclSema& c, std::span<const TemplateArg> args, u32 at) {
    auto full = template_arguments(c, args, at);
    if (!full) return std::nullopt;
    std::vector<ast::TemplateParamSema> params = ins().seqs.elements(c.template_params);
    return templates().substitute_constraint(c.constraints, Bindings::of(params.empty() ? 0 : params[0].depth, *full));
  }
  // A concept-id as an expression ([temp.names]/8): whether it is satisfied, a `bool` prvalue;
  // value-dependent while its arguments are.
  Ex concept_id_expr(const ast::DeclSema& c, std::span<const TemplateArg> args, u32 begin, u32 end) {
    QualType b = builtin(Builtin::bool_);
    if (std::ranges::any_of(args, [&](const TemplateArg& a) { return dependent(a); })) return dependent_node({}, begin, end, b);
    auto nf = concept_constraint(c, args, begin);
    if (!nf) return {NodeId::none, begin, end};
    auto sat = q_->satisfied(*nf);
    if (!sat) return dependent_node({}, begin, end, b);
    if (q_->satisfaction_failed(*nf)) return {NodeId::none, begin, end};
    return bool_literal(*sat, begin, end);
  }

  // ------------------------------------------------------------ type-trait built-ins (docs/EXTENSIONS.md)

  // The built-in `name` names, if it is one ycxx implements.
  const lex::TypeTraitInfo* type_trait(NameId name) { return lex::type_trait(ins().names.spelling(name)); }
  QualType innermost(QualType t) {
    while (types().is_array(t)) t = types().canonical(types().rec(t).inner);
    return t;
  }
  // Whether a class type is a union: by its class-key, which its first declaration's tokens say.
  bool is_union_type(QualType t) {
    if (!types().is_class(t)) return false;
    if (const ClassRec* r = class_rec(types().class_of(t))) return r->union_;
    return q_->is_union_class(types().class_of(t));
  }
  // `__trait(T)` that transforms a type ([meta.trans]): the type it gives.
  std::optional<QualType> trait_type(lex::TypeTrait t, QualType arg, u32 at) {
    auto r = templates().transform(t, arg);
    if (!r) diag(at, Diag::trait_not_applicable, arg);
    return r;
  }
  // `__trait(T…)` that tests types ([meta.unary], [meta.rel]): a `bool` prvalue, value-dependent
  // while a type is dependent.
  Ex trait_expr(lex::TypeTrait t, std::span<const QualType> args, u32 begin, u32 end) {
    using enum lex::TypeTrait;
    QualType b = builtin(Builtin::bool_);
    if (std::ranges::any_of(args, [&](QualType a) { return types().is_dependent(a); })) return dependent_node({}, begin, end, b);
    QualType x = args[0], c = types().canonical(x);
    auto cls = [&]() -> const ClassRec* {
      if (!types().is_class(c)) return nullptr;
      if (!require_complete(c, begin)) return nullptr;
      return class_rec(types().class_of(c));
    };
    bool v = false;
    switch (t) {
    case is_same: v = c == types().canonical(args[1]); break;
    case is_void: v = types().is_void(c); break;
    case is_integral: v = types().is_integral(c); break;
    case is_floating_point: v = types().is_floating(c); break;
    case is_arithmetic: v = types().is_integral(c) || types().is_floating(c); break;
    case is_enum: v = types().is_enum(c); break;
    case is_scoped_enum:
      if (types().is_enum(c))
        if (const EnumRec* e = enum_rec(types().enum_of(c))) v = e->scoped;
      break;
    // The class-key says whether a class is a union, complete or not ([class.pre]).
    case is_class: v = types().is_class(c) && !is_union_type(c); break;
    case is_union: v = is_union_type(c); break;
    case is_pointer: v = types().is_pointer(c); break;
    // ycxx has no pointers to members yet, nor virtual functions, abstract or final classes: these
    // are false of every type it has.
    case is_member_pointer:
    case is_member_object_pointer:
    case is_member_function_pointer:
    case is_polymorphic:
    case is_abstract:
    case is_final:
    case has_virtual_destructor: v = false; break;
    case is_reference: v = types().is_reference(c); break;
    case is_lvalue_reference: v = types().is_reference(c) && !types().rec(c).rvalue_ref; break;
    case is_rvalue_reference: v = types().is_reference(c) && types().rec(c).rvalue_ref; break;
    case is_array: v = types().is_array(c); break;
    case is_bounded_array: v = types().is_array(c) && types().rec(c).bound_value != ConstId::none; break;
    case is_unbounded_array: v = types().is_array(c) && types().rec(c).bound_value == ConstId::none; break;
    case is_function: v = types().is_function(c); break;
    case is_object: v = types().is_object(c); break;
    // An array's cv-qualifiers are its elements' ([basic.type.qualifier]/3).
    case is_const: v = Types::is_const(innermost(c)); break;
    case is_volatile: v = Types::is_volatile(innermost(c)); break;
    case is_empty:
      // [meta.unary.prop]: a complete class, not a union, with no non-static data members.
      if (is_union_type(c)) v = false;
      else if (const ClassRec* r = cls()) v = r->fields.empty();
      else if (types().is_class(c)) return {NodeId::none, begin, end};
      break;
    case is_aggregate:
      if (types().is_class(c) && !cls()) return {NodeId::none, begin, end};
      v = this->is_aggregate(c);
      break;
    default: break;
    }
    return bool_literal(v, begin, end);
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
      if (auto a = bound_arg(*p)) {
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
  // A type member `n` of class `c` ([class.member.lookup], types): from its record where it is
  // complete, a specialization instantiated for it; or, in the class being defined or
  // instantiated, among the members declared so far ([basic.scope.class]).
  std::optional<QualType> member_type(EntityId c, NameId n) {
    if (const ClassRec* cr = class_rec(c)) {
      for (const auto& [name, t, te] : cr->types)
        if (name == n) return t;
      return std::nullopt;
    }
    if (c == current_class)
      if (auto m = q_->earlier_member(n); m && (m->kind == ast::DeclKind::enum_ || m->kind == ast::DeclKind::type_alias))
        return m->type;
    return std::nullopt;
  }

  std::optional<Scope> scope_member(const Scope& s, NameId n) {
    // A member of the class whose member-specification this is (the current instantiation in a
    // template) is found where it is declared ([temp.dep.type]/1); a member of another dependent
    // type is looked up when the template is instantiated ([temp.dep.type]/7): a dependent type.
    if (s.type && types().is_class(*s.type) && (!types().is_dependent(*s.type) || types().class_of(*s.type) == current_class))
      if (auto t = member_type(types().class_of(*s.type), n)) return Scope{std::nullopt, *t};
    if (s.type && types().is_dependent(*s.type)) return Scope{std::nullopt, types().dep_name(*s.type, n)};
    if (s.ns) {
      for (const ast::DeclSema& d : q_->namespace_members(*s.ns, n)) {
        if (d.kind == ast::DeclKind::namespace_) return Scope{d.entity, std::nullopt};
        if (is_type(d.kind)) return Scope{std::nullopt, d.type};
      }
      return std::nullopt;
    }
    return std::nullopt;
  }
  // Whether `S::n` names a function or variable, which hides a type of that name in its scope
  // ([basic.scope.hiding]/2): `::stat(path, &buf)` calls the function.
  bool scope_names_object(const Scope& s, NameId n) {
    if (s.ns)
      return std::ranges::any_of(q_->namespace_members(*s.ns, n), [](const ast::DeclSema& d) {
        return d.kind == ast::DeclKind::function || d.kind == ast::DeclKind::variable;
      });
    if (s.type && types().is_class(*s.type))
      if (const ClassRec* c = class_rec(types().class_of(*s.type)))
        return std::ranges::any_of(c->members, [&](const MemberDeclRec& m) { return m.name == n; });
    return false;
  }
  // Whether the type `S::n`, named at `at`, is accessible here: a nested type of a class
  // ([class.access]/4); diagnosed if not.
  bool type_member_access(const Scope& s, NameId n, u32 at) {
    if (!s.type || !types().is_class(*s.type) || types().is_dependent(*s.type)) return true;
    EntityId c = types().class_of(*s.type);
    if (const ClassRec* cr = class_rec(c))
      for (const NestedType& t : cr->types)
        if (t.name == n) {
          if (defer_type_access && !accessible(c, cr->access_of(t.entity))) {
            deferred_access_.push_back({c, t.entity, n, at});
            return true;
          }
          return check_access(c, t.entity, n, at);
        }
    return true;
  }
  // In a namespace-scope declaration, the types its decl-specifiers and declarator name are
  // checked once what it declares is known: a member's definition is in its class's scope, and
  // a friend's declaration may name the class's private types ([class.access]/6–7). `declared`
  // is the function or variable declared, none when it is not known.
  bool defer_type_access = false;
  void settle_type_access(EntityId declared) {
    defer_type_access = false;
    EntityId saved = current_function;
    if (declared != EntityId::none) current_function = declared;
    for (const DeferredAccess& d : deferred_access_) check_access(d.cls, d.member, d.name, d.at);
    current_function = saved;
    deferred_access_.clear();
  }
  // The function `N::name` of type `t` declared in namespace `ns`, if any ([namespace.memdef]/3:
  // a qualified friend refers to an earlier declaration).
  std::optional<EntityId> namespace_function(EntityId ns, NameId name, QualType t) {
    for (const ast::DeclSema& d : q_->namespace_members(ns, name))
      if (d.kind == ast::DeclKind::function && types().canonical(d.type) == types().canonical(t)) return d.entity;
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
      if (auto r = class_member(types().class_of(*s.type), n, begin, end, name_at)) return *r;
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
      for (const auto& [n, nt, te] : cr->types)
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
  // Whether `object.name` names a member function template ([temp.names]/3): the object's class
  // has one of that name.
  bool names_member_template(const Ex& object, bool arrow, NameId name) {
    if (!object.ok() || dependent(object)) return false;
    QualType t = arrow ? types().pointee(object.type()) : object.type();
    if (!types().is_class(t)) return false;
    const ClassRec* c = class_rec(types().class_of(t));
    return c && std::ranges::any_of(c->members, [&](const MemberDeclRec& m) { return m.name == name && m.is_template; });
  }
  // Explicit template arguments for member `e`'s function templates ([temp.arg.explicit]).
  void explicit_template_args(const Ex& e, std::vector<TemplateArg> args) {
    if (e.ok() && is_bound_member(e)) explicit_args_[std::to_underlying(e.id)] = std::move(args);
  }
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
    if (found->member && found->member->is_function) return bound_member(c, name, object, arrow, begin, end, name_at);
    if (!check_access(c, found->member ? found->member->entity : found->field->entity, name, name_at)) return fail;
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
      for (const FieldRec& f : c->fields) {
        r.push_back({types().add_cv(f.type, Types::cv(t)), f.name, f.init, f.dmi});
        if (c->union_) break;  // [dcl.init.aggr]/16: a union's list initializes its first member
      }
    }
    return r;
  }
  bool is_union(QualType t) {
    const ClassRec* c = types().is_class(t) ? class_rec(types().class_of(t)) : nullptr;
    return c && c->union_;
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
        if (j == elems.size() && is_union(t) && !elided) {
          diag(c.begin, Diag::not_supported, lex::Tk::period);  // a union's member other than its first
          return fail;
        }
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
  // `TT<args>` of template template parameter `p` ([temp.names]): bound, the specialization of its
  // argument; otherwise a dependent type.
  std::optional<QualType> template_param_id(const ast::TemplateParamSema& p, std::span<const TemplateArg> args, u32 at) {
    // The arguments fit the parameter's own head, whose defaults complete them, whatever template
    // it is bound to ([temp.arg]/4, [temp.arg.template]); a pack parameter's arguments grouped, as
    // Itanium mangles them (`T_IclE`, `T_IcJEE`).
    std::vector<ast::TemplateParamSema> head = ins().seqs.elements(p.params);
    Templates t = templates();
    std::vector<std::optional<TemplateArg>> out(head.size());
    std::optional<std::vector<TemplateArg>> full;
    if (t.explicit_into(head, args, out)) full = t.complete(ast::DeclSema{}, head, out);
    if (!full) {
      diag(at, Diag::template_args_mismatch, p.name);
      return std::nullopt;
    }
    if (auto a = bound_arg(p); a && a->kind == ast::TemplateArgKind::template_) {
      if (a->templ != EntityId::none) {
        auto d = q_->template_decl(a->templ);
        if (!d) return std::nullopt;
        return class_template_id(*d, t.flatten(*full), at);
      }
      TypeRec r = types().rec(a->type);
      return types().dep_spec(r.depth, r.index, r.pack, *full);
    }
    return types().dep_spec(p.depth, p.index, p.pack != 0, *full);
  }
  // The template template parameter `name` names here, if it does.
  const ast::TemplateParamSema* template_template_param(NameId name) const {
    const ast::TemplateParamSema* p = template_param(name);
    return p && p->kind == ast::TemplateArgKind::template_ ? p : nullptr;
  }
  // Whether `name` names a template whose template-id is a type: a class or alias template, or a
  // template template parameter.
  bool names_type_template(NameId name) {
    return template_template_param(name) || type_template(name) || builtin_template(name);
  }
  // `__make_integer_seq` (docs/EXTENSIONS.md), unless something declared hides it.
  bool builtin_template(NameId name) {
    if (!lex::builtin_template(ins().names.spelling(name))) return false;
    return !template_param(name) && q_->visible_decls(name, directives()).empty();
  }
  // `__make_integer_seq<Seq, T, N>`: `Seq<T, 0, 1, …, N-1>` for a template Seq, an integral type T
  // other than bool and a value N of type T that is not negative; while an argument is dependent, a
  // dependent type.
  std::optional<QualType> builtin_template_id(NameId name, std::span<const TemplateArg> args, u32 at) {
    bool ok = args.size() == 3 && args[0].kind == ast::TemplateArgKind::template_ &&
              args[1].kind == ast::TemplateArgKind::type && args[2].kind == ast::TemplateArgKind::value;
    auto t = ok ? templates().builtin_template(ast::BuiltinTemplate::make_integer_seq, args) : std::nullopt;
    if (!t) diag(at, Diag::template_args_mismatch, name);
    return t;
  }

  // A template-name as a template argument ([temp.arg.template]/1): a class or alias template, in
  // namespace `ns` or found by unqualified lookup, or a template template parameter (its argument
  // when bound).
  std::optional<TemplateArg> template_name_arg(std::optional<EntityId> ns, NameId name) {
    if (!ns)
      if (const ast::TemplateParamSema* p = template_param(name)) {
        if (p->kind != ast::TemplateArgKind::template_) return std::nullopt;
        if (auto a = bound_arg(*p)) return a;
        return TemplateArg{ast::TemplateArgKind::template_, p->type, ConstId::none};
      }
    // In a class template, its injected-class-name is the template as a template argument and the
    // class (the current instantiation, or the specialization) as a type one ([temp.local]/1).
    if (!ns && current_class != EntityId::none) {
      const EntityRec& c = w_->entities.rec(current_class);
      if (c.templ != EntityId::none && c.name == name && !local_named(name))
        return TemplateArg{ast::TemplateArgKind::template_, types().class_type(current_class), ConstId::none, {}, c.templ};
    }
    auto t = ns ? namespace_type_template(*ns, name) : type_template(name);
    if (!t) return std::nullopt;
    return TemplateArg{ast::TemplateArgKind::template_, QualType{}, ConstId::none, {}, t->entity};
  }
  // The argument a template parameter named here stands for: its argument, or for a pack the
  // element of the innermost expansion naming it ([temp.variadic]/5). A pack outside every
  // expansion is unexpanded: none, as an unbound parameter is, and noted for the expansion that
  // must enclose it.
  std::optional<TemplateArg> bound_arg(const ast::TemplateParamSema& p) {
    if (p.pack) note_pack({p.depth, p.index, {}});
    auto a = template_args.at(p.depth, p.index);
    if (!a || a->kind != ast::TemplateArgKind::pack) return a;
    for (auto f = frames_.rbegin(); f != frames_.rend(); ++f)
      if (std::ranges::contains(f->packs, PackRef{p.depth, p.index, {}})) {
        std::vector<TemplateArg> es = ins().seqs.elements(a->elems);
        return f->index < es.size() ? std::optional(es[f->index]) : std::nullopt;
      }
    return std::nullopt;
  }

  // ------------------------------------------------------------ packs ([temp.variadic])

  // The packs the operand being read names, for the expansion that may follow it: one list for
  // each operand being read, innermost last.
  void begin_pattern() { pattern_packs_.emplace_back(); }
  // After the operand: the packs it names. With `...` they are recorded as an expansion of the
  // definition ([temp.variadic]/5); without, they are the enclosing operand's too.
  // A template argument's expansion is substituted structurally, and not recorded (`record`);
  // its packs may still be expanded by the declarator it is in, so an unexpanded one is not
  // diagnosed there. An expression's packs that no enclosing pattern takes are unexpanded
  // ([temp.variadic]/5).
  std::vector<PackRef> end_pattern(bool expanded, u32 begin, u32 end, bool record = true) {
    std::vector<PackRef> packs = std::move(pattern_packs_.back());
    pattern_packs_.pop_back();
    if (expanded) {
      if (record) expansions.push_back({begin, end, packs});
      return packs;
    }
    if (record && pattern_packs_.empty() && frames_.empty() && !packs.empty()) diag(begin, Diag::unexpanded_pack, pack_name(packs[0]));
    for (const PackRef& r : packs) note_pack(r);
    return packs;
  }
  NameId pack_name(const PackRef& r) {
    if (r.fn != NameId{}) return r.fn;
    for (const auto& scope : template_scopes)
      for (const ast::TemplateParamSema& p : scope)
        if (p.depth == r.depth && p.index == r.index) return p.name;
    return no_name();
  }
  void note_pack(PackRef r) {
    if (!pattern_packs_.empty() && !std::ranges::contains(pattern_packs_.back(), r)) pattern_packs_.back().push_back(r);
  }
  // Whether this is an instantiation of a definition with expansions: where they are is known.
  bool expansion_at_any() { return q_->pattern_expansions() != nullptr; }
  // The expansion of the definition at `begin`, in an instantiation.
  const Expansion* expansion_at(u32 begin) {
    if (const std::vector<Expansion>* es = q_->pattern_expansions())
      for (const Expansion& e : *es)
        if (e.begin == begin) return &e;
    return nullptr;
  }
  // How many elements an expansion has here: the length of the packs it names, all equal
  // ([temp.variadic]/7); none while one of them is not bound.
  std::optional<std::size_t> expansion_length(const Expansion& e, u32 at) {
    std::optional<std::size_t> n;
    for (const PackRef& r : e.packs) {
      std::size_t m = 0;
      if (r.fn != NameId{}) {
        auto it = std::ranges::find(param_packs_, r.fn, &ParamPack::name);
        if (it == param_packs_.end()) return std::nullopt;
        m = it->locals.size();
      } else {
        auto a = template_args.at(r.depth, r.index);
        if (!a || a->kind != ast::TemplateArgKind::pack) return std::nullopt;
        m = ins().seqs.size(a->elems);
      }
      if (n && *n != m) {
        diag(at, Diag::pack_length_mismatch);
        return std::nullopt;
      }
      n = m;
    }
    return n;
  }
  // Whether this analysis is an instantiation's: some template parameter is bound.
  bool instantiating() const {
    return std::ranges::any_of(template_args.levels, [](const auto& l) { return std::ranges::any_of(l, [](const auto& a) { return a.has_value(); }); });
  }
  void push_frame(const std::vector<PackRef>& packs, u32 index) { frames_.push_back({packs, index}); }
  // While a template argument that is itself a pack expansion is read, the enclosing expansions'
  // elements are not in effect: its packs are expanded whole ([temp.variadic]/5).
  auto hide_frames() { return std::exchange(frames_, {}); }
  void restore_frames(auto f) { frames_ = std::move(f); }
  void pop_frame() { frames_.pop_back(); }
  bool in_expansion() const { return !frames_.empty(); }
  // A function parameter pack of the function being instantiated: its name, and its parameters'
  // locals, one per element ([temp.variadic]/8).
  void declare_param_pack(NameId name) { param_packs_.push_back({name, {}}); }
  void declare_pack_element(NameId name, QualType type) {
    u32 l = declare_local(no_name(), type, local_flag::parameter, 0, false);
    locals_[l].name = name;
    auto it = std::ranges::find(param_packs_, name, &ParamPack::name);
    if (it == param_packs_.end()) {
      param_packs_.push_back({name, {}});
      it = param_packs_.end() - 1;
    }
    it->locals.push_back(l);
  }
  // The local a function parameter pack's name stands for here: its element of the innermost
  // expansion naming it; none outside every expansion.
  std::optional<u32> pack_element(NameId name, bool& is_pack) {
    auto it = std::ranges::find(param_packs_, name, &ParamPack::name);
    is_pack = it != param_packs_.end();
    if (!is_pack) return std::nullopt;
    note_pack({0, 0, name});
    for (auto f = frames_.rbegin(); f != frames_.rend(); ++f)
      if (std::ranges::contains(f->packs, PackRef{0, 0, name}))
        return f->index < it->locals.size() ? std::optional(it->locals[f->index]) : std::nullopt;
    return std::nullopt;
  }
  // `sizeof...(name)` ([expr.sizeof]/5): a template parameter pack's or a function parameter
  // pack's length, value-dependent until it is bound.
  Ex sizeof_pack(NameId name, u32 begin, u32 end) {
    QualType size_t_ = builtin(Builtin::unsigned_long);
    auto it = std::ranges::find(param_packs_, name, &ParamPack::name);
    if (it != param_packs_.end()) return literal(size_t_, w_->values.integer(size_t_, it->locals.size()), ValueCategory::prvalue, begin, end);
    const ast::TemplateParamSema* p = template_param(name);
    if (!p || !p->pack) {
      if (is_function_pack_pattern(name)) return dependent_node({}, begin, end, size_t_);
      diag(begin, Diag::not_a_pack, name);
      return {NodeId::none, begin, end};
    }
    auto a = template_args.at(p->depth, p->index);
    if (a && a->kind == ast::TemplateArgKind::pack)
      return literal(size_t_, w_->values.integer(size_t_, ins().seqs.size(a->elems)), ValueCategory::prvalue, begin, end);
    ConstId v = ins().intern_value(ast::VDepSizeofPack{p->depth, p->index});
    ExprInfo info{size_t_, ValueCategory::prvalue, expr_flag::dependent};
    return node(ast::Literal{info, v}, begin, end, info);
  }
  // Whether `name` is a local in scope here.
  bool local_named(NameId name) const {
    return std::ranges::any_of(scopes_, [&](const auto& sc) { return std::ranges::any_of(sc, [&](const auto& p) { return p.first == name; }); });
  }
  // Whether `name` is a function parameter pack of the definition being analysed (its type a pack
  // expansion), in phase one.
  bool is_function_pack_pattern(NameId name) {
    for (std::size_t d = scopes_.size(); d-- > 0;)
      for (auto p = scopes_[d].rbegin(); p != scopes_[d].rend(); ++p)
        if (p->first == name) return types().kind(locals_[p->second].type) == TypeKind::pack_expansion;
    return false;
  }
  // The pack expansions this definition has (phase one), for its instantiations.
  std::vector<Expansion> expansions;
  // Whether a type or argument names a parameter pack, as a pack expansion's pattern must
  // ([temp.variadic]/5).
  bool names_pack(QualType t) { return names_pack(templates().type_arg(t)); }
  bool names_pack(const TemplateArg& a) {
    std::vector<std::pair<u16, u16>> refs;
    templates().params_in(a, refs, false);
    for (auto [d, i] : refs)
      for (const auto& scope : template_scopes)
        for (const ast::TemplateParamSema& p : scope)
          if (p.depth == d && p.index == i && p.pack) return true;
    return false;
  }
  // `args` with the expansions whose packs are bound here expanded ([temp.variadic]/8); none when
  // there is none such.
  std::optional<std::vector<TemplateArg>> expand_bound(std::span<const TemplateArg> args) {
    Templates t = templates();
    bool any = false;
    std::vector<TemplateArg> out;
    for (const TemplateArg& a : args) {
      auto n = t.is_expansion(a) ? t.expansion_length(t.pattern_of(a), template_args) : std::nullopt;
      if (!n || !*n) {
        out.push_back(a);
        continue;
      }
      auto xs = t.substitute_expansion(a, template_args);
      if (!xs) return std::nullopt;
      out.insert(out.end(), xs->begin(), xs->end());
      any = true;
    }
    if (!any) return std::nullopt;
    return out;
  }
  // What the cell demands of the program (§B1.4), in program order.
  std::vector<Demand> demands;

  Templates templates() {
    Templates t(*w_, classes());
    t.member_type = [this](EntityId c, NameId n) { return member_type(c, n); };
    t.template_decl = [this](EntityId tmpl) { return q_->template_decl(tmpl); };
    return t;
  }
  bool dependent(const Ex& e) {
    return e.ok() && ((e.info.flags & expr_flag::dependent) || types().is_dependent(e.type()));
  }
  bool dependent(const TemplateArg& a) {
    return w_->arg_dependent(a);
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
    // A constructor template's specializations are constructors ([temp.mem]/1).
    EntityKind kind = (tmpl.flags & decl_flag::constructor) ? EntityKind::constructor : EntityKind::function;
    EntityRec r{kind, t.name, type, t.lang, t.linkage, t.first, t.scope};
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
    if (template_param(name) || names_member(name)) return false;
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
    if (auto v = variable_template(name)) return variable_template_id(*v, args, begin, end);
    if (auto c = concept_named(name)) return concept_id_expr(*c, args, begin, end);
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
  // The function whose body is being analysed, none outside one: a friend of a class may name its
  // private members ([class.friend]/2).
  EntityId current_function = EntityId::none;

  // [class.access]/1–2: whether a member of class `c` with access `a` can be named here. A public
  // one anywhere; a private or protected one (no base classes yet) in a member of `c` or of a
  // class nested in it ([class.access.nest]), or in a friend of `c`: a function, or a class whose
  // members are then all friends ([class.friend]/2).
  bool accessible(EntityId c, ast::Access a) {
    if (a == ast::Access::public_ || a == ast::Access::none) return true;
    const ClassRec* cr = class_rec(c);
    if (!cr) return true;
    auto befriended = [&](EntityId e) { return std::ranges::find(cr->friends, e) != cr->friends.end(); };
    if (current_function != EntityId::none && befriended(current_function)) return true;
    for (EntityId k = current_class; k != EntityId::none; k = w_->entities.rec(k).scope) {
      const EntityRec& r = w_->entities.rec(k);
      if (r.kind != EntityKind::class_ && r.kind != EntityKind::class_template) break;
      if (k == c || befriended(k)) return true;
    }
    return false;
  }
  // Whether member `m` of class `c`, named `name` at `at`, is accessible here; diagnosed if not.
  bool check_access(EntityId c, EntityId m, NameId name, u32 at) {
    const ClassRec* cr = class_rec(c);
    if (!cr) return true;
    ast::Access a = cr->access_of(m);
    if (accessible(c, a)) return true;
    diag(at, a == ast::Access::protected_ ? Diag::protected_member : Diag::private_member, name, types().class_type(c));
    return false;
  }

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
  Ex bound_member(EntityId c, NameId name, std::optional<Ex> object, bool arrow, u32 begin, u32 end, u32 name_at) {
    auto cands = member_functions(c, name);
    ExprInfo info{builtin(Builtin::bound_member), ValueCategory::prvalue, 0};
    Ex r = node(ast::Member{info, cands.front()->entity, ~u32{0}, arrow ? 1u : 0u, 0, object ? at(*object, begin) : Child{}},
                begin, end, info);
    bound_[std::to_underlying(r.id)] = {object, std::move(cands), c, name_at};
    return r;
  }
  bool is_bound_member(const Ex& e) { return e.ok() && types().is(e.type(), Builtin::bound_member); }

  // The implicit member access `name` inside a member function of `c` ([class.mfct.non.static]),
  // or a qualified name `C::name`: none if `c` has no such member.
  std::optional<Ex> class_member(EntityId c, NameId name, u32 begin, u32 end, u32 name_at) {
    auto f = find_member(c, name);
    if (!f) {
      // The enumerators of an unscoped enumeration nested in the class are its members.
      if (const ClassRec* cr = class_rec(c))
        for (const auto& [n, t, te] : cr->types)
          if (is_unscoped_enum(t))
            if (auto r = find_enumerator(types().enum_of(t), name, begin, end)) {
              if (!check_access(c, te, name, name_at)) return Ex{NodeId::none, begin, end};
              return r;
            }
      return std::nullopt;
    }
    Ex fail{NodeId::none, begin, end};
    bool has_this = this_type && current_class == c;
    if (!(f->member && f->member->is_function) && !check_access(c, f->field ? f->field->entity : f->member->entity, name, name_at))
      return fail;
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
    // ([expr.prim.id.qual]): `S::f` can have its address taken or be called. A member function
    // template is a set to deduce from, as overloads are.
    if (!m.is_function || (m.is_static && !m.is_template && member_functions(c, name).size() == 1)) {
      if (m.is_function && !check_access(c, m.entity, name, name_at)) return fail;
      if (!m.is_function) return static_member(c, m.entity, m.type, begin, end);
      ExprInfo info{m.type, ValueCategory::lvalue, 0};
      return node(ast::DeclRef{info, m.entity}, begin, end, info);
    }
    std::optional<Ex> object;
    if (has_this) object = this_expr(begin, begin);
    return bound_member(c, name, object, true, begin, end, name_at);
  }

  // A static data member of class `c`. Of the current instantiation of a class template, one whose
  // initializer is value-dependent is value-dependent ([temp.dep.constexpr]/2.3); of a
  // specialization, naming it instantiates its initializer ([temp.inst]/3).
  Ex static_member(EntityId c, EntityId v, QualType t, u32 begin, u32 end) {
    bool dep = types().is_dependent(types().class_type(c)) && q_->value_dependent_member(v);
    if (!dep && !types().is_dependent(types().class_type(c)) && !q_->use_static_member(v)) return {NodeId::none, begin, end};
    ExprInfo info{t, ValueCategory::lvalue, dep ? expr_flag::dependent : u8(0)};
    return node(ast::DeclRef{info, v}, begin, end, info);
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
    if (auto r = class_member(types().class_of(*t), name, begin, end, name_at)) return *r;
    diag(name_at, Diag::no_member, name, *t);
    return fail;
  }

  // A call of a member function ([expr.call], [over.match.funcs]): a static one is an ordinary
  // call; a non-static one passes its object, which must be at least as cv-qualified as the
  // function ([class.this]).
  Ex member_call(Ex callee, std::vector<Ex> args, u32 end, u32 paren_at) {
    u32 begin = callee.begin;
    Ex fail{NodeId::none, begin, end};
    if (auto b = std::ranges::find_if(args, [&](const Ex& a) { return is_braced(a); }); b != args.end()) {
      diag(b->begin, Diag::not_supported, lex::Tk::l_brace);  // a braced list for a member function's parameter
      return fail;
    }
    auto m = *as<ast::Member>(callee);
    const Bound& bound = bound_.at(std::to_underlying(callee.id));
    std::optional<Ex> object = bound.object;
    // The object's cv-qualifiers, which a non-static member function must have ([class.this]).
    std::optional<Cv> object_cv;
    if (object) object_cv = Types::cv(m.arrow ? types().pointee(object->type()) : object->type());
    // The candidates: each member function, and of each member function template the
    // specialization deduction gives ([temp.over]/1, [temp.mem]); explicit template arguments name
    // only templates ([temp.arg.explicit]).
    auto ex = explicit_args_.find(std::to_underlying(callee.id));
    std::span<const TemplateArg> explicit_args;
    if (ex != explicit_args_.end()) explicit_args = ex->second;
    std::vector<ArgShape> shapes;
    for (const Ex& a : args)
      shapes.push_back({a.type(), a.cat() == ValueCategory::lvalue, !types().is(a.type(), Builtin::overload_set) && !is_bound_member(a)});
    std::vector<Candidate> cands;
    std::vector<const MemberDeclRec*> recs;
    std::deque<ast::DeclSema> patterns;
    for (const MemberDeclRec* c : bound.cands) {
      if (!c->is_template) {
        if (ex != explicit_args_.end()) continue;
        cands.push_back({c->entity, c->type, c->is_static, c->is_deleted});
        recs.push_back(c);
        continue;
      }
      auto tmpl = q_->template_decl(c->entity);
      if (!tmpl) continue;
      const ast::DeclSema& td = patterns.emplace_back(*tmpl);
      Templates t = templates();
      auto targs = t.deduce_call(td, shapes, explicit_args);
      if (!targs) continue;
      if (td.constraints != ConstId::none) {
        std::vector<ast::TemplateParamSema> ps = ins().seqs.elements(td.template_params);
        auto nf = t.substitute_constraint(td.constraints, Bindings::of(ps.empty() ? 0 : ps[0].depth, *targs));
        auto sat = nf ? q_->satisfied(*nf) : std::optional<bool>(false);
        if (sat && !*sat) continue;
      }
      auto st = t.specialization_type(td, *targs);
      if (!st) continue;
      cands.push_back({specialization(td, *targs, *st), *st, c->is_static, c->is_deleted, &td});
      recs.push_back(c);
    }
    NameId name = bound.cands.front()->name;
    if (cands.empty()) {
      diag(begin, Diag::no_viable_function, name);
      return fail;
    }
    // [class.this]: a non-static member function needs the object's cv-qualifiers.
    if (object_cv && std::ranges::none_of(cands, [&](const Candidate& c) {
          return c.no_object || cv_within(*object_cv, types().rec(c.type).fn_cv);
        })) {
      diag(begin, Diag::const_member_call, name);
      return fail;
    }
    auto chosen = resolve(cands, args, begin, name, object_cv);
    if (!chosen) return fail;
    const MemberDeclRec* d = recs[*chosen];
    const Candidate& k = cands[*chosen];
    if (!check_access(bound.cls, d->entity, d->name, bound.name_at)) return fail;
    // A specialization called is one whose definition the program requires ([temp.inst]/8).
    if (k.pattern) demands.push_back({k.entity, ast::Loc{item_, loc_base_ + begin}});
    TypeRec fr = types().rec(k.type);
    auto children = call_args(fr, args, begin, paren_at);
    if (!children) return fail;
    ExprInfo info = result_info(fr.inner);
    if (d->is_static) {
      // The object expression is evaluated, then discarded ([expr.ref]).
      Ex call = node(ast::Call{info, k.entity, at(callee, begin), b_->list(*children)}, begin, end, info);
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
    return node(ast::MemberCall{info, k.entity, at(o, begin), b_->list(*children)}, begin, end, info);
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
      // [conv.prom]/3: without one, to the type its values need.
      const EnumRec* er = enum_rec(types().enum_of(from));
      QualType p = er && er->promotion ? *er->promotion : builtin(promoted(*types().builtin_of(underlying_of(from))));
      bool promotion = types().canonical(p) == types().canonical(to);
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
      if (f1.pattern && f2.pattern) {
        Templates t = templates();
        auto [a12, a21] = t.ordering(*f1.pattern, *f2.pattern, args.size());
        if (a12 != a21) return a12;
        // [temp.func.order]/6: equally specialized templates with equivalent heads and parameters:
        // the more constrained ([temp.constr.order]).
        if (a12 && equivalent_for_constraints(*f1.pattern, *f2.pattern)) {
          ConstId c1 = f1.pattern->constraints, c2 = f2.pattern->constraints;
          return t.subsumes(c1, c2) && !t.subsumes(c2, c1);
        }
        return false;
      }
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
    // [dcl.init.general]/17.6.1: a prvalue of the class itself initializes the object directly.
    if (!list && args.size() == 1 && !args[0].glvalue() && initializes(ut, args[0])) return args[0];
    std::vector<Candidate> cands;
    std::vector<EntityId> ents;
    bool user_copy = false;
    std::deque<ast::DeclSema> patterns;
    std::vector<ArgShape> shapes;
    for (const Ex& a : args)
      shapes.push_back({a.type(), a.cat() == ValueCategory::lvalue, !types().is(a.type(), Builtin::overload_set) && !is_bound_member(a)});
    for (const MemberDeclRec& m : cr->members)
      if (m.is_ctor && m.is_template) {
        // A constructor template: the specialization deduction gives ([temp.over]/1), never a
        // copy constructor ([class.copy.ctor]/5).
        auto tmpl = q_->template_decl(m.entity);
        if (!tmpl) continue;
        const ast::DeclSema& td = patterns.emplace_back(*tmpl);
        Templates t = templates();
        auto targs = t.deduce_call(td, shapes, {});
        if (!targs) continue;
        auto st = t.specialization_type(td, *targs);
        if (!st || (copy_init && types().rec(*st).params.size() != 1)) continue;
        cands.push_back({specialization(td, *targs, *st), *st, true, m.is_deleted, &td});
      } else if (m.is_ctor) {
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
    if (k.entity != EntityId::none && !check_access(c, k.pattern ? k.pattern->entity : k.entity, w_->entities.rec(c).name, begin)) return fail;
    // A specialization called is one whose definition the program requires ([temp.inst]/8).
    if (k.pattern) demands.push_back({k.entity, ast::Loc{item_, loc_base_ + begin}});
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
      // A parameter of class type: a temporary the caller initializes and destroys ([expr.call]/7).
      if (i < fr.params.size() && types().is_class(fr.params[i]) && !a.glvalue())
        a = materialize(a, Types::unqualified(fr.params[i]), false, true);
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
  Ex materialize(Ex v, QualType t, bool extended, bool argument = false) {
    if (!v.ok()) return v;
    ExprInfo info{t, ValueCategory::xvalue, 0};
    return node(ast::Materialize{info, destructor_of(t), extended ? 1u : argument ? 2u : 0u, at(v, v.begin)}, v.begin, v.end, info);
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
          // A function parameter pack of the definition: its uses are expanded ([temp.variadic]).
          if (types().kind(l.type) == TypeKind::pack_expansion) {
            if (pattern_packs_.empty()) {
              diag(begin, Diag::unexpanded_pack, name);  // [temp.variadic]/5: outside every expansion
              return {NodeId::none, begin, end};
            }
            note_pack({0, 0, name});
            return dependent_node({}, begin, end);
          }
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
    // A function parameter pack of an instantiation, unless a block declares the name: its element
    // ([temp.variadic]/8).
    bool is_pack = false;
    if (auto l = pack_element(name, is_pack)) {
      ExprInfo info{types().referee(locals_[*l].type), ValueCategory::lvalue, 0};
      return node(ast::LocalRef{info, *l}, begin, end, info);
    }
    if (is_pack) {
      diag(begin, Diag::unexpanded_pack, name);
      return {NodeId::none, begin, end};
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
      if (p->pack && pattern_packs_.empty() && frames_.empty()) {
        diag(begin, Diag::unexpanded_pack, name);  // [temp.variadic]/5: outside every expansion
        return {NodeId::none, begin, end};
      }
      if (auto a = bound_arg(*p))
        return literal(Types::unqualified(a->type), a->value, ValueCategory::prvalue, begin, end);
      ConstId v = ins().intern_value(ast::VDepParam{p->depth, p->index});
      ExprInfo info{Types::unqualified(p->type), ValueCategory::prvalue, expr_flag::dependent};
      return node(ast::Literal{info, v}, begin, end, info);
    }
    // The class's members, in a member function's body ([basic.scope.class]).
    if (current_class != EntityId::none) {
      if (auto r = class_member(current_class, name, begin, end, begin)) return *r;
      if (!class_rec(current_class))
        if (auto m = q_->earlier_member(name)) {
          if (m->kind != ast::DeclKind::variable) {
            diag(begin, Diag::member_without_object, name);
            return {NodeId::none, begin, end};
          }
          return static_member(current_class, m->entity, m->type, begin, end);
        }
    }
    std::vector<ast::DeclSema> found;
    for (auto p = own.rbegin(); p != own.rend(); ++p)
      if (p->name == name) found.push_back(p->sema);
    // An earlier declarator of the member's own declaration (`static constexpr int a = 1, b = a;`).
    if (found.size() == 1 && current_class != EntityId::none && found[0].kind == ast::DeclKind::variable &&
        found[0].storage == ast::Storage::static_)
      return static_member(current_class, found[0].entity, found[0].type, begin, end);
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

  // A name lookup found nothing for ([basic.lookup.unqual]). A GNU built-in function the headers
  // use and ycxx lacks (`lex::lacking_builtins`) says that ycxx does not have it yet.
  void undeclared(u32 at_, NameId name) {
    diag(at_, lex::lacking_builtin(ins().names.spelling(name)) ? Diag::builtin_unsupported : Diag::undeclared, name);
  }
  // Whether `name` is a GNU built-in function ycxx does not have yet (`lex::lacking_builtins`,
  // docs/EXTENSIONS.md) that no declaration in scope gives. Its call is diagnosed without parsing
  // its arguments, which may be types (`__builtin_offsetof(T, m)`). Any other `__builtin_` name
  // is an ordinary one.
  bool unsupported_builtin(NameId name) {
    if (!lex::lacking_builtin(ins().names.spelling(name))) return false;
    for (const auto& scope : scopes_)
      if (std::ranges::any_of(scope, [&](const auto& p) { return p.first == name; })) return false;
    if (std::ranges::any_of(own, [&](const auto& p) { return p.name == name; })) return false;
    if (std::ranges::any_of(block_usings_, [&](const BlockUsing& u) { return u.name == name; })) return false;
    if (names_member(name)) return false;
    for (DeclId d : q_->visible_decls(name, directives())) {
      (void)d;
      return false;
    }
    return true;
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
      if (!any) undeclared(begin, name);
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
    // A discarded prvalue of class type is materialized ([expr.context]/2), so a call's result
    // has an object, destroyed with the full-expression.
    if (e.ok() && !e.glvalue() && !dependent(e) && types().is_class(e.type()) && !as<ast::Materialize>(e))
      return materialize(e, Types::unqualified(e.type()), false);
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
    // [conv.prom]/3: an unscoped enumeration without a fixed type promotes to the type its values
    // need; /4: one with a fixed type promotes as its underlying type does.
    if (is_unscoped_enum(e.type())) {
      if (const EnumRec* r = enum_rec(types().enum_of(e.type())); r && r->promotion)
        return convert_node(e, ast::ConversionKind::integral, *r->promotion);
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
  // after a diagnostic.
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
  // A complete object type's size or alignment: from the target, and for a class by
  // `ast::record_layout` over its members' (#194), the rule the back end lays it out by.
  std::optional<u64> layout_of(QualType t, bool align) {
    auto p = place_of(t);
    if (!p) return std::nullopt;
    return align ? p->align : p->size;
  }
  // Size and alignment together, once per class (`class_places_`): each member's are needed.
  std::optional<ast::FieldPlace> place_of(QualType t) {
    TypeRec r = types().rec(t);
    switch (r.kind) {
    case TypeKind::builtin:
      if (!target.has(r.builtin) || r.builtin == Builtin::void_) return std::nullopt;
      return ast::FieldPlace{target[r.builtin].size, target[r.builtin].align};
    case TypeKind::pointer:
    case TypeKind::reference: return ast::FieldPlace{target.pointer_size, target.pointer_align};  // a reference member's
    case TypeKind::enum_: return is_complete(t) ? place_of(underlying_of(t)) : std::nullopt;
    case TypeKind::array: {
      if (r.bound_value == ConstId::none) return std::nullopt;
      auto e = place_of(r.inner);
      if (!e) return std::nullopt;
      return ast::FieldPlace{e->size * r.bound, e->align};
    }
    case TypeKind::class_: {
      EntityId ce = types().class_of(t);
      if (auto it = class_places_.find(std::to_underlying(ce)); it != class_places_.end()) return it->second;
      const ClassRec* c = is_complete(t) ? class_rec(ce) : nullptr;
      if (!c) return std::nullopt;
      std::vector<ast::FieldPlace> places;
      for (const FieldRec& f : c->fields) {
        auto m = place_of(f.type);
        if (!m) return std::nullopt;
        places.push_back(*m);
      }
      ast::RecordLayout l = ast::record_layout(places, c->union_);
      return class_places_[std::to_underlying(ce)] = ast::FieldPlace{l.size, l.align};
    }
    default: return std::nullopt;  // void, functions
    }
  }
  std::map<u32, ast::FieldPlace> class_places_;  // complete classes' sizes and alignments

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

  // [over.match.oper]: an operator with an operand of class or enumeration type calls an operator
  // function when one is viable: a member `operator@` of the left operand's class, else the
  // non-member ones that unqualified lookup and argument-dependent lookup find
  // ([over.match.oper]/3). None viable: the built-in operator. (A member and a non-member are not
  // yet ranked against each other, nor against the built-in candidates.)
  std::optional<Ex> overloaded(std::string_view op, std::vector<Ex> operands, u32 begin, u32 end, u32 op_at) {
    auto user_type = [&](const Ex& e) {
      QualType t = types().referee(e.type());
      return types().is_class(t) || types().is_enum(t);
    };
    if (std::ranges::none_of(operands, user_type) || any_dependent(operands)) return std::nullopt;
    NameId name = ins().names.intern(std::string("operator") + std::string(op));
    std::size_t n = diag_count();
    QualType lt = types().referee(operands[0].type());
    if (types().is_class(lt))
      if (const ClassRec* c = class_rec(types().class_of(lt));
          c && std::ranges::any_of(c->members, [&](const MemberDeclRec& m) { return m.name == name && m.is_function && !m.is_static; })) {
        Ex m = member(operands[0], false, name, operands[0].end, op_at, op_at);
        Ex r = m.ok() ? call(m, std::vector<Ex>(operands.begin() + 1, operands.end()), end, op_at) : m;
        if (r.ok()) return Ex{r.id, begin, end, r.info};
        truncate_diags(n);
      }
    std::vector<ast::DeclSema> set;
    auto add = [&](const ast::DeclSema& d) {
      if (d.kind == ast::DeclKind::function && d.entity != EntityId::none &&
          !std::ranges::any_of(set, [&](const ast::DeclSema& x) { return x.entity == d.entity; }))
        set.push_back(d);
    };
    for (const auto& o : own)
      if (o.name == name) add(o.sema);
    for (DeclId d : q_->visible_decls(name, directives())) add(q_->decl_sema(d));
    for (const Ex& o : operands)
      if (auto ns = associated_namespace(o.type()))
        for (const ast::DeclSema& d : q_->namespace_members(*ns, name)) add(d);
    if (set.empty()) return std::nullopt;
    ExprInfo info{builtin(Builtin::overload_set), ValueCategory::lvalue, 0};
    Ex callee = node(ast::DeclRef{info, set.front().entity}, begin, begin, info);
    overloads_[std::to_underlying(callee.id)] = std::move(set);
    Ex r = call(callee, std::move(operands), end, op_at);
    if (!r.ok()) {
      // A deleted or ambiguous choice makes the expression ill-formed ([over.match.oper]/3,
      // [over.match.best]); only when no candidate is viable does the built-in operator apply.
      for (std::size_t i = n; i < diags_->size(); ++i)
        if ((*diags_)[i].id == diag_id(Diag::deleted_function) || (*diags_)[i].id == diag_id(Diag::ambiguous_call))
          return Ex{NodeId::none, begin, end};
      truncate_diags(n);
      return std::nullopt;
    }
    return Ex{r.id, begin, end, r.info};
  }

  Ex unary(ast::UnaryOp op, Ex e, u32 begin, u32 end, u32 op_at) {
    using enum ast::UnaryOp;
    if (!e.ok()) return {NodeId::none, begin, end};
    if (dependent(e)) return dependent_node({e}, begin, end);
    static constexpr std::string_view unary_spellings[] = {"+", "-", "!", "~", "*", "&", "++", "--", "", ""};
    if (std::string_view sp = unary_spellings[std::to_underlying(op)]; !sp.empty())
      if (auto o = overloaded(sp, {e}, begin, end, op_at)) return *o;
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
    static constexpr std::string_view binary_spellings[] = {"*", "/", "%", "+", "-", "<<", ">>", "<", ">", "<=", ">=",
                                                            "==", "!=", "&", "^", "|", "&&", "||", ","};
    if (auto o = overloaded(binary_spellings[std::to_underlying(op)], {l, r}, begin, end, op_at)) return *o;
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
    // `=` is a member only ([over.assign]); `operator_function_ok` rejects non-member ones.
    static constexpr std::string_view assign_spellings[] = {"=", "*=", "/=", "%=", "+=", "-=", "<<=", ">>=", "&=", "^=", "|="};
    if (auto o = overloaded(assign_spellings[std::to_underlying(op)], {l, r}, begin, end, op_at)) return *o;
    if (!check_modifiable(l, op_at) || !check_class_assignable(l, op_at)) return {NodeId::none, begin, end};
    QualType lt = l.type();
    QualType computation = Types::unqualified(lt);
    Ex c = r;
    if (op == AsgOp::assign) {
      c = convert(r, lt, r.begin);
      // A class result is a temporary the copy reads, destroyed with the full-expression
      // ([expr.assign]/9, [class.temporary]/4).
      if (c.ok() && types().is_class(lt) && !c.glvalue() && !dependent(c) && (as<ast::Call>(c) || as<ast::MemberCall>(c)))
        c = materialize(c, Types::unqualified(lt), false);
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
      undeclared(callee.begin, d->name);
      return {NodeId::none, begin, end};
    }
    if (dependent(callee) || any_dependent(args)) return dependent_node(all, begin, end);
    // A braced list as an argument is copy-list-initialization of its parameter
    // ([dcl.init.list]/1); overload resolution with one ([over.ics.list]) is not supported yet, so
    // the callee must be one function.
    auto braced = std::ranges::find_if(args, [&](const Ex& a) { return is_braced(a); });
    if (braced != args.end() && types().is(callee.type(), Builtin::overload_set)) {
      const std::vector<ast::DeclSema>& set = overloads_.at(std::to_underlying(callee.id));
      if (set.size() != 1 || (set[0].flags & decl_flag::template_)) {
        diag(braced->begin, Diag::not_supported, lex::Tk::l_brace);
        return {NodeId::none, begin, end};
      }
      ExprInfo ci{set[0].type, ValueCategory::lvalue, 0};
      callee = node(ast::DeclRef{ci, set[0].entity}, callee.begin, callee.end, ci);
    }
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
        // Its associated constraints are satisfied, or it is not viable ([temp.deduct.general]/5,
        // [over.match.viable]/3).
        if (d.constraints != ConstId::none) {
          std::vector<ast::TemplateParamSema> ps = ins().seqs.elements(d.template_params);
          auto nf = t.substitute_constraint(d.constraints, Bindings::of(ps.empty() ? 0 : ps[0].depth, *targs));
          auto sat = nf ? q_->satisfied(*nf) : std::optional<bool>(false);
          if (sat && !*sat) continue;
        }
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

  // A braced-init-list as a call's argument: a placeholder, list-initializing its parameter once
  // the callee is known (`call_args`).
  Ex braced_argument(std::unique_ptr<Braced> b) {
    ExprInfo info{builtin(Builtin::void_), ValueCategory::prvalue, 0};
    Ex r = node(ast::ValueInit{info}, b->begin, b->end, info);
    braced_args_[std::to_underlying(r.id)] = std::move(b);
    return r;
  }
  bool is_braced(const Ex& e) const { return e.ok() && braced_args_.contains(std::to_underlying(e.id)); }

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
      if (is_braced(args[i]) && i >= params.size()) {
        diag(args[i].begin, Diag::not_supported, lex::Tk::l_brace);  // [expr.call]/12: no list for an ellipsis
        return std::nullopt;
      }
      Ex a = is_braced(args[i])      ? list_init(Types::unqualified(params[i]), *braced_args_.at(std::to_underlying(args[i].id)))
             : i < params.size() ? convert(args[i], params[i], args[i].begin)
                                 : variadic_argument(args[i]);
      if (!a.ok()) return std::nullopt;
      // A parameter of class type is an object the caller initializes from the argument
      // ([expr.call]/7): a temporary, which the caller destroys (Itanium C++ ABI §3.1.2).
      if (i < params.size() && types().is_class(params[i]) && !dependent(a) && !a.glvalue())
        a = materialize(a, Types::unqualified(params[i]), false, true);
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
  // Packs ([temp.variadic]): the expansions being instantiated, innermost last, each with the
  // element its pattern is read for; the packs each operand being read names; and the function
  // parameter packs of the function being instantiated.
  struct Frame {
    std::vector<PackRef> packs;
    u32 index;
  };
  std::vector<Frame> frames_;
  std::vector<std::vector<PackRef>> pattern_packs_;
  struct ParamPack {
    NameId name;
    std::vector<u32> locals;
  };
  std::vector<ParamPack> param_packs_;
  std::vector<std::optional<ConstId>> local_values_;  // by local
  std::set<u32> consteval_;                           // consteval functions named so far
  std::map<u32, std::vector<Ex>> list_elems_;         // by draft InitList
  struct DeferredAccess {
    EntityId cls, member;
    NameId name;
    u32 at;
  };
  std::vector<DeferredAccess> deferred_access_;
  struct Bound {
    std::optional<Ex> object;
    std::vector<const MemberDeclRec*> cands;
    EntityId cls = EntityId::none;  // whose members they are, for access ([class.access]/4)
    u32 name_at = 0;
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
  std::map<u32, std::unique_ptr<Braced>> braced_args_;  // by placeholder node: a braced list argument
  std::map<u32, QualType> declared_types_;  // by DeclRef node: the declared type of what it names
  std::map<u32, std::vector<ast::DeclSema>> overloads_;  // by draft overload-set DeclRef
  std::map<u32, std::vector<TemplateArg>> explicit_args_;  // by draft overload-set DeclRef
  std::map<u32, std::vector<ast::DeclSema>> unqualified_;  // by draft DeclRef named by an unqualified-id
  std::vector<std::vector<std::pair<NameId, u32>>> scopes_;
  std::vector<std::size_t> guarded_;
  QualType return_type_{};
};

}  // namespace ycxx::sema
