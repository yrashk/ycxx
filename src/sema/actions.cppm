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

// What instantiating a generic lambda's function call operator needs of its definition:
// the closure, the operator (a template), its head, its parameters' names, where its body
// begins, and the enclosing function's constants it names without capturing them; and the
// enclosing function's declaration and arguments when that is an instantiation.
struct GenericClosure {
  EntityId closure = EntityId::none;
  EntityId call = EntityId::none;
  QualType call_type{};
  std::vector<ast::TemplateParamSema> head;
  std::vector<NameId> params;
  std::vector<NameId> captures;  // the closure's fields' names, `this` for a captured `this`
  u32 body_begin = 0;
  u16 flags = 0;
  std::vector<ast::LocalRec> consts;
  std::vector<ConstId> const_values;
  std::vector<Expansion> expansions;  // the pack expansions of its body, which its specializations expand
  // Where it is defined: the template-heads around it and their arguments, and the block-scope
  // type aliases its body can name.
  std::vector<std::vector<ast::TemplateParamSema>> scopes;
  Bindings args;
  std::vector<std::pair<NameId, QualType>> aliases;
  bool erroneous = false;  // its definition was diagnosed: its specializations say nothing more
};

// The enclosing function's template arguments and entity where a generic lambda is defined, for
// instantiating it in the same part.
struct GenericBinding {
  Bindings args;
  EntityId function = EntityId::none;
};

// A generic lambda's operator specialization's type, the return type deduced; or none, and
// whether its instantiation diagnosed why.
struct GenericResult {
  std::optional<QualType> type;
  bool diagnosed = false;
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
  // A use of a global allocation function that may lack a written declaration in the unit.
  virtual void use_allocation_function(EntityId) {}
  // Check the selected implicitly declared copy assignment against its subobjects.
  virtual CopyAssignment require_copy_assignment(EntityId) { return CopyAssignment::unsupported; }
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
  // The direct bases of class `c` while it is being defined (its base-clause), none otherwise.
  virtual std::vector<EntityId> defining_bases(EntityId) { return {}; }
  // Namespaces ([basic.namespace]), the global one being `EntityId::none`: the namespace (or
  // alias) unqualified lookup finds for `name`, and qualified lookup of `name` in namespace `ns`
  // ([namespace.qual]).
  virtual std::optional<EntityId> namespace_named(NameId name, std::span<const EntityId> directives = {}) = 0;
  virtual std::vector<ast::DeclSema> namespace_members(EntityId ns, NameId name) = 0;
  // [basic.lookup.argdep]: functions in one associated namespace's inline namespace set, and
  // a class's record if complete there. Ask at the lexical point, and for a dependent call
  // also at the point of instantiation ([temp.dep.candidate]).
  virtual std::vector<ast::DeclSema> adl_functions(EntityId ns, NameId name, bool at_instantiation) = 0;
  virtual const ClassRec* adl_class(EntityId c, bool at_instantiation) = 0;
  // Earlier friend declarations in a class whose member list is still being read.
  virtual std::vector<DeclId> earlier_adl_friends(EntityId, NameId) { return {}; }
  // In an instantiation, whether the call at `offset` was dependent in the template definition.
  virtual bool dependent_call(u32 /*offset*/) { return false; }
  // Whether instantiation has given up at its nesting limit, which was diagnosed ([temp.inst]/16):
  // what fails because of it says nothing more.
  virtual bool instantiation_gave_up() { return false; }
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
  // A static local declared ([stmt.dcl]/3), for constant evaluation's questions about it
  // (`global`): its type, and its constant if constant-initialized.
  virtual void static_local(EntityId /*v*/, QualType /*type*/, std::optional<ConstId> /*value*/) {}
  // Whether class `c`, complete or not, was declared with the class-key `union`.
  virtual bool is_union_class(EntityId /*c*/) { return false; }
  // The function type of `f`, whose declared return type holds a placeholder, with the return
  // type its definition deduced: a definition before the current point, or a function template
  // specialization's instantiated here ([dcl.spec.auto.general]/11, [temp.inst]/8). None if
  // there is none, or it is in error.
  virtual std::optional<QualType> deduced_type(EntityId /*f*/) { return std::nullopt; }
  // The type of `spec`, a specialization of a generic lambda's function call operator, with the
  // return type its instantiation deduced; the specialization is instantiated here
  // ([temp.inst]/8). `known` is its definition when it is in the part being analysed, which is
  // not committed yet, with the enclosing function's arguments there. None after an error.
  virtual GenericResult generic_instance(EntityId /*spec*/, const GenericClosure* /*known*/, const GenericBinding&) { return {}; }
  // A generic lambda's definition, as its body ends: the part it is in may instantiate it from
  // elsewhere (an instantiation of another lambda's operator) before the part is committed.
  virtual void generic_defined(const GenericClosure&) {}
  // Publishes a closure type's record, made by the analysis of the part its lambda is in
  // (docs/design/closures.md); the published record.
  virtual const ClassRec& publish_class(EntityId c, ClassRec r) = 0;
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
inline constexpr u8 immediate_address = 2;  // checked after immediate invocations are folded
inline constexpr u8 unrepresented_value = 4;  // this expression has no usable ConstId for a template argument
}

// Flags of `LocalRec::flags` and `DeclSema::flags`.
namespace local_flag {
inline constexpr u16 parameter = 1;
inline constexpr u16 default_argument_parameter = 8;
inline constexpr u16 value_dependent = 16;
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

// Flags of `ClosureDef::flags`.
namespace closure_flag {
inline constexpr u16 mutable_ = 1;      // its function call operator is not const
inline constexpr u16 captureless = 2;   // no lambda-capture: it converts to a pointer to function
inline constexpr u16 pattern = 4;       // in a template's or a generic lambda's definition: not code
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
  bool has_error_since(std::size_t begin) const {
    return std::ranges::any_of(std::span(*diags_).subspan(begin), [](const engine::Diagnostic& d) {
      auto kind = sema_diag_of(d.id);
      return kind && message_of(*kind).severity == lex::Severity::error;
    });
  }
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
  std::optional<QualType> class_name(NameId name, bool elaborated = false, bool aliases = true,
                                    std::optional<u32> type_at = std::nullopt) {
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
      // A type nested in the class whose scope this is ([basic.scope.class]).
      if (const ClassRec* cr = class_rec(current_class); cr && cr->defined) {
        if (!elaborated && current_function != EntityId::none && !scopes_.empty() && !templated() &&
            c.kind == EntityKind::class_ && c.templ == EntityId::none && (c.name == name || !cr->bases.empty())) {
          auto hit = class_body_type(*cr, name, type_at);
          if (hit.found) return hit.type;
        }
        if (auto r = nested_member(current_class, name, true)) return r->first.type;
      } else if (auto m = q_->earlier_member(name); m && (m->kind == ast::DeclKind::enum_ || m->kind == ast::DeclKind::type_alias)) {
        return m->type;
      } else {
        // In the class being defined, a type of its (complete) bases.
        for (EntityId b : q_->defining_bases(current_class))
          if (auto r = nested_member(b, name)) return r->first.type;
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

  struct ClassBodyType {
    bool found = false;
    std::optional<QualType> type;
  };

  // [class.pre], [class.member.lookup]: an ordinary class's injected name stops the search on
  // that branch. In a nonvirtual graph, distinct base branches cannot dominate one another.
  ClassBodyType class_body_type(const ClassRec& root, NameId name, std::optional<u32> type_at) {
    struct Edge {
      EntityId derived;
      ast::Access access;
    };
    struct Hit {
      std::optional<QualType> type;
      EntityId owner;
      ast::Access access;
      bool injected;
      std::vector<Edge> path;
    };
    std::vector<Hit> hits;
    std::vector<Edge> path;
    auto collect = [&](this auto&& visit, EntityId cls, const ClassRec& cr) -> void {
      const EntityRec& c = w_->entities.rec(cls);
      if (c.kind == EntityKind::class_ && c.templ == EntityId::none && c.name == name) {
        hits.push_back({types().class_type(cls), cls, ast::Access::public_, true, path});
        return;
      }
      if (auto member = direct_member(cls, cr, name)) {
        auto nested = std::ranges::find(cr.types, name, &NestedType::name);
        if (member->nested_type != EntityId::none && member->enumerator == EntityId::none && nested != cr.types.end())
          hits.push_back({nested->type, cls, cr.access_of(nested->entity), false, path});
        else
          hits.push_back({std::nullopt, cls, ast::Access::public_, false, path});
        return;
      }
      for (const BaseRec& b : cr.bases) {
        if (const ClassRec* base = class_rec(b.cls)) {
          if (type_at) path.push_back({cls, b.access});
          visit(b.cls, *base);
          if (type_at) path.pop_back();
        }
      }
    };
    collect(current_class, root);
    if (hits.empty()) return {};
    auto first = std::ranges::find_if(hits, [](const Hit& h) { return h.type.has_value(); });
    if (first == hits.end()) return {true, std::nullopt};
    // Apply the lookup-set and access checks to names with an injected-class-name candidate.
    // Ordinary nested member access is handled by its existing lookup paths.
    if (type_at && std::ranges::any_of(hits, &Hit::injected)) {
      QualType selected = types().canonical(*first->type);
      if (std::ranges::any_of(hits, [&](const Hit& h) { return !h.type || types().canonical(*h.type) != selected; })) {
        diag(*type_at, Diag::ambiguous_member, name, types().class_type(current_class));
      } else if (!std::ranges::any_of(hits, [&](const Hit& h) {
                   return accessible(h.owner, h.access, current_class) && std::ranges::all_of(h.path, [&](const Edge& edge) {
                     return accessible(edge.derived, edge.access);
                   });
                 })) {
        diag(*type_at, Diag::inaccessible_base, *first->type, types().class_type(current_class));
      }
    }
    return {true, first->type};
  }
  // A typedef-name the implementation declares where nothing else does (docs/EXTENSIONS.md):
  // `__builtin_va_list`, the type used by the target's C headers and libycxx's `va_list`.
  std::optional<QualType> builtin_type_name(NameId name) {
    if (ins().names.spelling(name) != "__builtin_va_list") return std::nullopt;
    return w_->builtin_va_list();
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
  // A generic lambda can still be in its definition while its enclosing function is instantiated.
  bool in_template_definition() const {
    return std::ranges::any_of(template_scopes | std::views::join, [&](const ast::TemplateParamSema& p) {
      return !template_args.at(p.depth, p.index);
    });
  }
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
    if (t == is_same) {
      if (auto v = templates().is_same_value(args[0], args[1])) return literal(b, *v, ValueCategory::prvalue, begin, end);
      ExprInfo info{b, ValueCategory::prvalue, expr_flag::dependent | expr_flag::unrepresented_value};
      return node(ast::Dependent{info, b_->list({})}, begin, end, info);
    }
    if (std::ranges::any_of(args, [&](QualType a) { return types().is_dependent(a); })) return dependent_node({}, begin, end, b);
    QualType x = args[0], c = types().canonical(x);
    auto cls = [&]() -> const ClassRec* {
      if (!types().is_class(c)) return nullptr;
      if (!require_complete(c, begin)) return nullptr;
      return class_rec(types().class_of(c));
    };
    bool v = false;
    switch (t) {
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
    case is_member_pointer: v = types().is_member_pointer(c); break;
    case is_member_object_pointer: v = types().is_member_pointer(c) && !types().is_function(types().rec(c).inner); break;
    case is_member_function_pointer: v = types().is_member_pointer(c) && types().is_function(types().rec(c).inner); break;
    case is_polymorphic:
    case has_virtual_destructor:
      if (types().is_class(c) && !is_union_type(c)) {
        const ClassRec* r = cls();
        if (!r) return {NodeId::none, begin, end};
        v = t == is_polymorphic ? r->polymorphic : r->virtual_destructor;
      }
      break;
    case is_abstract:
    case is_final: v = false; break;
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
      else if (const ClassRec* r = cls()) v = !r->polymorphic && r->fields.empty();
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
  // A type nested in complete class `c` or else in its bases ([class.member.lookup]), with the
  // class that declares it.
  std::optional<std::pair<NestedType, EntityId>> nested_member(EntityId c, NameId n, bool unqualified = false) {
    const ClassRec* cr = class_rec(c);
    if (!cr) return std::nullopt;
    for (const NestedType& t : cr->types)
      if (t.name == n) return std::pair{t, c};
    for (const BaseRec& b : cr->bases) {
      if (unqualified && b.dependent_in_pattern) continue;
      if (auto r = nested_member(b.cls, n)) return r;
    }
    return std::nullopt;
  }
  std::optional<QualType> member_type(EntityId c, NameId n) {
    if (class_rec(c)) {
      if (auto r = nested_member(c, n)) return r->first.type;
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
      auto members = q_->namespace_members(*s.ns, n);
      for (const ast::DeclSema& d : members) {
        if (d.kind == ast::DeclKind::namespace_) return Scope{d.entity, std::nullopt};
        if (is_type(d.kind)) return Scope{std::nullopt, d.type};
      }
      if (*s.ns == EntityId::none && members.empty())
        if (auto t = builtin_type_name(n)) return Scope{std::nullopt, *t};
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
    EntityId naming = types().class_of(*s.type);
    auto r = nested_member(naming, n);
    if (!r) return true;
    auto [t, c] = *r;
    if (defer_type_access && (!accessible(c, class_rec(c)->access_of(t.entity), naming) || !accessible_naming_base(naming, c))) {
      deferred_access_.push_back({c, t.entity, n, at});
      return true;
    }
    return check_access(c, t.entity, n, at, naming);
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
  Ex scope_expr(const Scope& s, NameId n, u32 begin, u32 name_at, u32 end, bool bare_address = false) {
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
      if (auto r = class_member(types().class_of(*s.type), n, begin, end, name_at, bare_address)) return *r;
      diag(name_at, Diag::no_member, n, *s.type);
      return {NodeId::none, begin, end};
    }
    if (namespace_lambda && namespace_lambda->name == n && namespace_lambda->scope == *s.ns)
      return undeduced_namespace_lambda(n, name_at, end);
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
    if (auto r = nested_member(types().class_of(*t), name)) return r->first.type;
    return std::nullopt;
  }

  // [dcl.init.string]/1: the code units, terminating null included, of string literal `e` when it
  // can initialize an array of `elem`: an ordinary literal one of char, signed char or unsigned
  // char, a UTF-8 literal one of char8_t, char or unsigned char, and UTF-16 and UTF-32 literals
  // arrays of their respective element types.
  std::optional<std::vector<u32>> string_units(QualType elem, Ex e) {
    if (!e.ok() || !types().is_array(e.type())) return std::nullopt;
    auto lit = as<ast::Literal>(e);
    auto src = types().builtin_of(Types::unqualified(types().rec(e.type()).inner));
    auto dst = types().builtin_of(Types::unqualified(elem));
    if (!lit || !src || !dst) return std::nullopt;
    using enum Builtin;
    bool fits = *src == *dst || (*src == char_ && (*dst == signed_char || *dst == unsigned_char)) ||
                (*src == char8_t_ && (*dst == char_ || *dst == unsigned_char));
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
    for (u32 u : *units)
      b.clauses.push_back({literal(elem, w_->values.integer(elem, u), ValueCategory::prvalue, e.begin, e.end),
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

  bool require_supported_virtual_layout(QualType t, u32 at) {
    if (!classes().has_virtual_layouts()) return true;
    const QualType element = innermost(t);
    // Layout can be the first use that requires a class specialization to be complete.
    const ClassRec* record = types().is_class(element) ? class_rec(types().class_of(element)) : nullptr;
    if (!record || !record->layout_requires_virtual_support) return true;
    diag(at, Diag::virtual_layout, t);
    return false;
  }

  bool require_supported_virtual_signature(const TypeRec& f, u32 at) {
    return require_supported_virtual_layout(f.inner, at) && std::ranges::all_of(f.params, [&](QualType p) {
      return require_supported_virtual_layout(p, at);
    });
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
  // Explicit template arguments for the function templates `e` names, a member or an overload set
  // ([temp.arg.explicit]).
  void explicit_template_args(const Ex& e, std::vector<TemplateArg> args) {
    if (e.ok() && (is_bound_member(e) || types().is(e.type(), Builtin::overload_set)))
      explicit_args_[std::to_underlying(e.id)] = std::move(args);
  }
  bool class_has_member_template(EntityId c, NameId name) {
    const ClassRec* r = class_rec(c);
    return r && std::ranges::any_of(r->members, [&](const MemberDeclRec& m) { return m.name == name && m.is_template; });
  }
  // Whether `S::name` names a function template ([temp.names]/3): a member template of class S,
  // or a function template of namespace S.
  bool scope_names_template(const Scope& sc, NameId name) {
    if (sc.ns) return namespace_template(*sc.ns, name, ast::DeclKind::function).has_value();
    return sc.type && types().is_class(*sc.type) && class_has_member_template(types().class_of(*sc.type), name);
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
      if (!object.ok()) return fail;
    }
    if (!require_complete(ct, op_at)) return fail;
    EntityId c = types().class_of(ct);
    auto found = find_member(c, name);
    if (!found) {
      diag(name_at, Diag::no_member, name, Types::unqualified(ct));
      return fail;
    }
    if (found->ambiguous) {
      diag(name_at, Diag::ambiguous_member, name, Types::unqualified(ct));
      return fail;
    }
    if (found->nested_type != EntityId::none) {
      if (found->enumerator == EntityId::none) {
        diag(name_at, Diag::type_as_expression, name);
        return fail;
      }
      if (!check_access(found->owner, found->nested_type, name, name_at, c)) return fail;
      Ex value = enumerator(found->enumerator, types().enum_type(found->nested_type), name_at, end);
      if (!value.ok()) return fail;
      Ex discarded = convert_node(object, ast::ConversionKind::to_void, builtin(Builtin::void_));
      return node(ast::OrderedBinary{value.info, ast::OrderedOp::comma, at(discarded, begin), at(value, begin)},
                  begin, end, value.info);
    }
    EntityId naming = c;
    // A member of a base: named through the base class subobject ([class.member.lookup]); a
    // static member needs no object, so not the conversion either.
    if (found->owner != c && found->member && found->member->is_static) {
      c = found->owner;
    } else if (found->owner != c) {
      object = to_base(object, found->owner, name_at);
      if (!object.ok()) return fail;
      ct = arrow ? types().pointee(object.type()) : object.type();
      c = found->owner;
    }
    if (found->member && found->member->is_function) return bound_member(c, name, object, arrow, begin, end, name_at, naming);
    if (!check_access(c, found->member ? found->member->entity : found->field->entity, name, name_at, naming)) return fail;
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
    if (unevaluated == 0 && !(checking_virtual_body && c == current_class) &&
        !require_supported_virtual_layout(ct, name_at)) return fail;
    // [expr.ref]/7.2: the member's type, with the object's cv-qualifiers added; the result is an
    // lvalue if the object is (or is reached through a pointer), else an xvalue.
    // A reference member gives the object it refers to, an lvalue.
    bool ref = types().is_reference(f.type);
    QualType t = ref ? types().referee(f.type) : types().add_cv(f.type, Types::cv(ct));
    if (f.bit_width && Types::is_volatile(t) && unevaluated == 0) {
      diag(name_at, Diag::volatile_bit_field);
      return fail;
    }
    ValueCategory cat = ref || arrow || object.cat() == ValueCategory::lvalue ? ValueCategory::lvalue : ValueCategory::xvalue;
    ExprInfo info{t, cat, 0, f.bit_width};
    return node(ast::Member{info, f.entity, index, arrow ? 1u : 0u, ref ? 1u : 0u, at(object, begin)}, begin, end, info);
  }

  // ------------------------------------------------------------ initialization ([dcl.init])

  // An aggregate ([dcl.init.aggr]): an array or a class with the required properties.
  bool is_aggregate(QualType t) {
    if (types().is_array(t)) return true;
    if (!types().is_class(t)) return false;
    const ClassRec* c = class_rec(types().class_of(t));
    // [dcl.init.aggr]/1: no user-declared constructors or virtual functions; a closure type is none
    // ([expr.prim.lambda.closure]/1).
    return !c || (!c->user_ctors && !c->closure && !c->polymorphic);
  }
  bool has_user_ctors(QualType t) {
    const ClassRec* c = types().is_class(t) ? class_rec(types().class_of(t)) : nullptr;
    return c && (c->user_ctors || c->closure);
  }

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
      // [dcl.init.aggr]/2: the direct bases in declaration order, then the members.
      for (const BaseRec& b : c->bases) r.push_back({types().add_cv(types().class_type(b.cls), Types::cv(t)), no_name()});
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
    if (const ClassRec* c = class_rec(r.entity)) {
      for (const BaseRec& b : c->bases)
        if (has_member_inits(types().class_type(b.cls))) return true;
      for (const FieldRec& f : c->fields)
        if (f.init != ConstId::none || f.dmi || has_member_inits(f.type)) return true;
    }
    return false;
  }

  // Value-initialization ([dcl.init.general]) of an object of type `t`. An aggregate whose
  // members have default member initializers gets them (the implicit default constructor's).
  Ex value_init(QualType t, u32 begin, u32 end) {
    if (types().is_dependent(t)) return dependent_node({}, begin, end);
    if (!require_supported_virtual_layout(t, begin)) return {NodeId::none, begin, end};
    if (types().is_member_pointer(t)) {
      diag(begin, Diag::member_pointer_values);
      return {NodeId::none, begin, end};
    }
    if (has_user_ctors(t)) return construct(t, {}, begin, end);  // the default constructor
    if (has_member_inits(t)) return member_inits(t, true, begin, end);
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    return node(ast::ValueInit{info}, begin, end, info);
  }
  // Default-initialization ([dcl.init.general]/7): the default member initializers, and no
  // initialization for the rest.
  // Whether default-initializing a `t` calls a deleted default constructor: a capturing
  // closure's ([expr.prim.lambda.closure]/13).
  bool default_deleted(QualType t) {
    if (!types().is_class(t)) return types().is_array(t) && default_deleted(types().rec(t).inner);
    const ClassRec* r = class_rec(types().class_of(t));
    return r && r->default_deleted;
  }
  Ex default_init(QualType t, u32 begin, u32 end) {
    if (types().is_dependent(t)) return dependent_node({}, begin, end);
    if (!require_supported_virtual_layout(t, begin)) return {NodeId::none, begin, end};
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
    if (!require_complete(t, b.begin) || !require_supported_virtual_layout(t, b.begin)) return fail;
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
      return list_element(t, b.clauses[0].expr);
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
    if (!e.ok() || address_set(e)) return convert(e, t, e.begin);
    Ex from = rvalue(e);
    Ex r = convert(e, t, e.begin);
    QualType target = types().is_reference(t) ? types().referee(t) : t;
    if (r.ok() && narrows(from, target)) {
      diag(e.begin, Diag::narrowing, from.type(), Types::unqualified(target));
      return {NodeId::none, e.begin, e.end};
    }
    return r;
  }

  // [dcl.init.list]/7: an integral conversion to a type that cannot represent every value of the
  // source type, unless the source is a constant whose value fits; a pointer to bool; the
  // conversions of /7.1–7.3 between floating and other types.
  bool narrows(const Ex& from, QualType to) {
    if (!from.ok() || dependent(from) || types().is_dependent(to)) return false;
    auto a = types().builtin_of(from.type()), b = types().builtin_of(to);
    if (types().is_pointer(from.type()) && b == Builtin::bool_) return true;
    // Check an unscoped enumeration's narrowing against its underlying integer type.
    if (b && ast::is_floating(*b) && is_unscoped_enum(from.type())) {
      QualType u = underlying_of(from.type());
      return narrows_floating(convert_node(from, ast::ConversionKind::integral, u), *types().builtin_of(u), *b);
    }
    if (a && b && (ast::is_floating(*a) || ast::is_floating(*b))) return narrows_floating(from, *a, *b);
    if (!a || !b || !ast::is_integral(*a) || !ast::is_integral(*b) || *a == *b) return false;
    IntShape sa = shape(w_->target, *a), sb = shape(w_->target, *b);
    if (*b == Builtin::bool_) sb = {1, false};
    if (*a == Builtin::bool_) sa = {1, false};
    if (ast::represents(w_->target, *b, sa.bits, sa.is_signed) || (*b == Builtin::bool_ && *a == Builtin::bool_)) return false;
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

  // [dcl.init.list]/7.1–7.3 for a conversion from `a` to `b`, one of them floating.
  bool narrows_floating(const Ex& from, Builtin a, Builtin b) {
    if (ast::is_integral(b)) return true;  // /7.1: floating to integer (bool is an integer type)
    auto v = constant_of ? constant_of(from) : std::nullopt;
    if (ast::is_floating(a)) {
      // /7.2: to a type of lesser rank, unless the source is constant and either its converted
      // value is finite and did not overflow, or the values before and after are not finite.
      if (ast::float_rank_compare(w_->target, b, a) >= 0) return false;
      if (!v || !ast::computes(w_->target[b].format) || w_->values.rec(*v).kind != ValueKind::floating) return true;
      double x = w_->values.rec(*v).real;
      double y = w_->values.rec(w_->values.floating(builtin(b), x)).real;
      return std::isfinite(x) != std::isfinite(y);
    }
    // /7.3: an integer to a floating type, unless the source is constant and its value converts
    // back unchanged: its significant bits fit the type's precision (the exponent range of every
    // supported format covers 128-bit integers). Integer types are at most 64 bits wide here: a
    // 128-bit one would need the constant's high word.
    if (!v || w_->values.rec(*v).kind != ValueKind::integer) return true;
    IntShape sa = shape(w_->target, a);
    if (a == Builtin::bool_) sa = {1, false};
    u64 raw = w_->values.rec(*v).bits & (sa.bits >= 64 ? ~u64{0} : (u64{1} << sa.bits) - 1);
    if (sa.is_signed && sa.bits < 64 && ((raw >> (sa.bits - 1)) & 1)) raw |= ~((u64{1} << sa.bits) - 1);
    u64 magnitude = sa.is_signed && std::int64_t(raw) < 0 ? u64(0) - raw : raw;
    if (magnitude == 0) return false;
    u32 significant = u32(std::bit_width(magnitude) - std::countr_zero(magnitude));
    return significant > ast::shape_of(w_->target[b].format).precision;
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
    std::erase_if(barriers_, [&](const auto& b) { return b.first >= inner; });
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
  // A static block-scope variable's entity ([stmt.dcl]/3): local to the function, or the lambda,
  // being defined, numbered among its statics of that name as Itanium discriminates them
  // (§5.1.6), with no linkage.
  void static_local(u32 local) {
    ast::LocalRec& l = locals_[local];
    EntityId scope = lambdas_.empty() ? current_function : lambdas_.back().call;
    u32 ordinal = u32(std::ranges::count_if(statics_, [&](const auto& s) { return s.first == scope && s.second == l.name; }));
    statics_.push_back({scope, l.name});
    EntityRec r{EntityKind::variable, l.name, l.type, LangLinkage::cxx, ast::Linkage::none, {}, scope};
    r.head = ast::mix(ast::seed("ycxx.sema.static-local"), u64(ordinal));
    r.ordinal = ordinal;
    l.entity = w_->entities.entity(std::move(r));
  }
  void static_value(u32 local, std::optional<ConstId> value) { q_->static_local(locals_[local].entity, locals_[local].type, value); }
  // An array of unknown bound gets its bound from its initializer ([dcl.array]/8).
  void set_local_type(u32 local, QualType t) { locals_[local].type = t; }
  // A local usable in constant expressions ([expr.const]): constexpr, or const integral with a
  // constant initializer; its value.
  void set_local_value(u32 local, ConstId v) { local_values_[local] = v; }
  void set_local_value_dependent(u32 local) { locals_[local].flags |= local_flag::value_dependent; }
  std::span<const std::optional<ConstId>> local_values() const { return local_values_; }
  const ast::LocalRec& local(u32 i) const { return locals_[i]; }
  // [dcl.type.decltype]/1: a binding gives its referenced type; its reference slot is internal.
  QualType local_declared_type(const ast::LocalRec& local) {
    return local.flags & ast::local_flag::structured_binding ? types().referee(local.type) : local.type;
  }
  std::span<const ast::LocalRec> locals() const { return locals_; }
  // After the scope of a requires-expression ends, its parameters leave no runtime locals.
  void discard_local_declarations(std::size_t first) {
    locals_.resize(first);
    local_values_.resize(first);
  }
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
  // The name of a template parameter a placeholder parameter invents ([dcl.fct]/22): a keyword,
  // which no lookup finds.
  NameId invented_name() { return ins().names.intern("auto"); }

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
  bool expansion_at_any() { return pattern_expansions() != nullptr; }
  // A generic lambda's specialization reads the expansions its definition recorded.
  const std::vector<Expansion>* expansions_override = nullptr;
  const std::vector<Expansion>* pattern_expansions() { return expansions_override ? expansions_override : q_->pattern_expansions(); }
  // The expansion of the definition at `begin`, in an instantiation.
  const Expansion* expansion_at(u32 begin) {
    if (const std::vector<Expansion>* es = pattern_expansions())
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
  void declare_param_pack(NameId name) { param_packs_.push_back({name, {}, u32(lambdas_.size())}); }
  void declare_pack_element(NameId name, QualType type) {
    u32 l = declare_local(no_name(), type, local_flag::parameter, 0, false);
    locals_[l].name = name;
    auto it = std::ranges::find(param_packs_, name, &ParamPack::name);
    if (it == param_packs_.end()) {
      param_packs_.push_back({name, {}, u32(lambdas_.size())});
      it = param_packs_.end() - 1;
    }
    it->locals.push_back(l);
  }
  // The local a function parameter pack's name stands for here: its element of the innermost
  // expansion naming it; none outside every expansion.
  std::optional<u32> pack_element(NameId name, bool& is_pack, u32* ctx = nullptr) {
    auto it = std::ranges::find(param_packs_, name, &ParamPack::name);
    is_pack = it != param_packs_.end();
    if (!is_pack) return std::nullopt;
    if (ctx) *ctx = it->ctx;
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
  bool type_dependent(const Braced& b) {
    return std::ranges::any_of(b.clauses, [&](const Braced::Clause& c) {
      return c.list ? type_dependent(*c.list) : c.expr.ok() && types().is_dependent(c.expr.type());
    });
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
    // A specialization for a type with internal linkage, such as a closure type local to a
    // function that is not inline, is local to the unit too ([basic.link]/4).
    ast::Linkage linkage = std::ranges::any_of(args, [&](const TemplateArg& a) { return internal_arg(a); }) ? ast::Linkage::internal
                                                                                                           : t.linkage;
    // A constructor template's specializations are constructors ([temp.mem]/1).
    EntityKind kind = (tmpl.flags & decl_flag::constructor) ? EntityKind::constructor : EntityKind::function;
    EntityRec r{kind, t.name, type, t.lang, linkage, t.first, t.scope};
    r.templ = tmpl.entity;
    r.args.assign(args.begin(), args.end());
    return w_->entities.entity(std::move(r));
  }
  bool internal_arg(const TemplateArg& a) {
    if (a.kind == ast::TemplateArgKind::pack)
      return std::ranges::any_of(ins().seqs.elements(a.elems), [&](const TemplateArg& e) { return internal_arg(e); });
    return a.kind == ast::TemplateArgKind::type && internal_type(a.type);
  }
  // Whether a type names a class with internal linkage.
  bool internal_type(QualType t) {
    TypeRec r = types().rec(t);
    switch (r.kind) {
    case TypeKind::class_: {
      const EntityRec& c = w_->entities.rec(r.entity);
      return c.linkage == ast::Linkage::internal || std::ranges::any_of(c.args, [&](const TemplateArg& a) { return internal_arg(a); });
    }
    case TypeKind::pointer:
    case TypeKind::reference:
    case TypeKind::array: return internal_type(r.inner);
    case TypeKind::member_pointer: return internal_type(r.member_owner) || internal_type(r.inner);
    case TypeKind::function:
      return internal_type(r.inner) || std::ranges::any_of(r.params, [&](QualType p) { return internal_type(p); });
    default: return false;
    }
  }

  // Whether unqualified lookup of `name` finds a function template here, so that a `<` after it
  // begins a template argument list ([temp.names]/3).
  bool is_template_name(NameId name) {
    for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s)
      for (auto p = s->rbegin(); p != s->rend(); ++p)
        if (p->first == name) return false;
    if (template_param(name)) return false;
    // A member template of the class whose member is being analysed ([temp.mem]).
    if (names_member(name)) return current_class != EntityId::none && class_has_member_template(current_class, name);
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
    if (!types().is(r.type(), Builtin::overload_set) && !is_bound_member(r)) {
      diag(begin, Diag::not_a_template, name);
      return {NodeId::none, begin, end};
    }
    explicit_args_[std::to_underlying(r.id)] = std::move(args);
    return r;
  }


  // ------------------------------------------------------------ lambda-expressions ([expr.prim.lambda])
  //
  // docs/design/closures.md. A lambda's body is analysed here, inside the part it is in, with a
  // frame per enclosing lambda: the body's own locals are the function call operator's, and a
  // name that resolves to a local of an enclosing function, or to an init-capture, goes through
  // the captures of every lambda between ([expr.prim.lambda.capture]/7).

  // What the parser read of a capture ([expr.prim.lambda.capture]): for a simple capture, its
  // entity named in the enclosing context (`source`); for an init-capture, its initializer.
  struct CaptureSpec {
    enum class Kind : u8 { copy, ref, this_, star_this, init_copy, init_ref };
    Kind kind = Kind::copy;
    NameId name{};
    u32 at = 0;
    Ex init;
    std::optional<std::pair<u32, u32>> source;  // (context, key) of a simple capture's entity
    bool pack = false;  // `xs...`: each element of a function parameter pack ([expr.prim.lambda.capture]/17)
  };
  struct LambdaParam {
    NameId name{};
    QualType type{};
    u32 at = 0;
  };
  struct LambdaSpec {
    u32 begin = 0;
    u8 default_capture = 0;  // 1 `=`, 2 `&`
    bool has_capture = false;  // a lambda-capture, even an empty capture-default
    std::vector<CaptureSpec> captures;
    std::vector<LambdaParam> params;
    bool variadic = false;
    bool mutable_ = false;
    std::optional<QualType> trailing;
    // A generic lambda's template-head, explicit and invented ([expr.prim.lambda.closure]/7): the
    // parser pushed it onto `template_scopes`, and pops it after the body.
    std::vector<ast::TemplateParamSema> head;
  };

  // A lambda whose body encloses the point: its closure so far and the enclosing context's state.
  struct Capture {
    u32 ctx;
    u32 key;
    u32 field;
  };
  struct LambdaFrame {
    EntityId closure = EntityId::none;
    EntityId call = EntityId::none;
    QualType call_type{};
    QualType this_type{};  // the operator's `this`: a pointer to the closure, const unless mutable
    EntityId scope = EntityId::none;
    Fp128 sig{};
    u32 ordinal = 0;
    u32 begin = 0;
    std::size_t scope_base = 0;  // the scopes from this depth on are the lambda's
    u8 default_capture = 0;
    bool has_capture = false;
    bool mutable_ = false;
    std::vector<FieldRec> fields;
    std::vector<Ex> inits;  // by field, in the enclosing context
    std::vector<Capture> captures;
    std::vector<std::pair<NameId, u32>> init_captures;  // their names and fields
    std::map<u32, ConstId> const_fields;  // the fields of captured constants, with their values
    std::optional<u32> this_field;
    bool star_this = false;
    std::vector<ast::TemplateParamSema> head;  // a generic lambda's
    std::size_t diags_at = 0;  // the diagnostics before its body
    std::vector<std::vector<ast::TemplateParamSema>> outer_scopes;  // a specialization's enclosing state
    Bindings outer_args;
    // The enclosing context's state.
    std::vector<ast::LocalRec> outer_locals;
    std::vector<std::optional<ConstId>> outer_values;
    QualType return_type{};
    QualType declared_return{};
    bool return_deduced = false;
    bool return_seen = false;
    u32 no_return_deduction = 0;
    u32 loop_depth = 0;
  };
  // A local's scope entry that is an init-capture: the field of the lambda whose scope it is in.
  static constexpr u32 capture_bit = 0x80000000u;

  // The context a scope depth belongs to: 0 the function's own, n the body of the n-th enclosing
  // lambda, outermost first.
  u32 context_of(std::size_t depth) const {
    u32 c = 0;
    for (const LambdaFrame& f : lambdas_)
      if (f.scope_base <= depth) ++c;
    return c;
  }
  // A simple capture's entity, named in the current context ([expr.prim.lambda.capture]/2): the
  // context and key its name resolves to, none if it is not a local or an init-capture.
  std::optional<std::pair<u32, u32>> capture_source(NameId name) {
    for (std::size_t d = scopes_.size(); d-- > 0;)
      for (auto p = scopes_[d].rbegin(); p != scopes_[d].rend(); ++p)
        if (p->first == name) return std::pair(context_of(d), p->second);
    return std::nullopt;
  }

  // The expression, in context `ctx`, naming what `key` is there: a local of that context's
  // table, or an init-capture of the lambda whose body it is.
  Ex source_expr(u32 ctx, u32 key, u32 begin, u32 end) {
    if (key & capture_bit) return field_expr(ctx - 1, key & ~capture_bit, begin, end);
    const ast::LocalRec& l = ctx == lambdas_.size() ? locals_[key] : lambdas_[ctx].outer_locals[key];
    u8 flags = l.flags & local_flag::value_dependent ? expr_flag::dependent : 0;
    ExprInfo info{types().referee(l.type), ValueCategory::lvalue, flags};
    return node(ast::LocalRef{info, key}, begin, end, info);
  }
  // The type of what `source_expr` names, as an expression has it.
  QualType source_type(u32 ctx, u32 key) {
    if (key & capture_bit) return types().referee(lambdas_[ctx - 1].fields[key & ~capture_bit].type);
    const ast::LocalRec& l = ctx == lambdas_.size() ? locals_[key] : lambdas_[ctx].outer_locals[key];
    return types().referee(l.type);
  }
  // Field `field` of the closure of lambda `f`, named in that lambda's body: `this->field`, an
  // lvalue, const unless the lambda is mutable or the member a reference ([expr.prim.lambda.closure]/12).
  Ex field_expr(u32 f, u32 field, u32 begin, u32 end) {
    const LambdaFrame& fr = lambdas_[f];
    const FieldRec& fd = fr.fields[field];
    bool ref = types().is_reference(fd.type);
    QualType t = ref ? types().referee(fd.type) : fr.mutable_ ? fd.type : types().add_cv(fd.type, Cv::const_);
    ExprInfo ti{fr.this_type, ValueCategory::prvalue, 0};
    Ex self = node(ast::This{ti}, begin, begin, ti);
    ExprInfo info{t, ValueCategory::lvalue, 0};
    Ex m = node(ast::Member{info, fd.entity, field, 1, ref ? 1u : 0u, at(self, begin)}, begin, end, info);
    // A captured constant read as a value is its value ([expr.const]: not an odr-use, #355).
    if (auto c = fr.const_fields.find(field); c != fr.const_fields.end()) const_captures_[std::to_underlying(m.id)] = c->second;
    return m;
  }

  // `name`, resolved at context `ctx` to `key`, used in the current context: captured by each
  // lambda from `ctx` inwards, or, for a constant of an enclosing function, its value: naming it
  // is not an odr-use ([basic.def.odr]/5, [expr.prim.lambda.capture]/8).
  Ex captured(u32 ctx, u32 key, NameId name, u32 begin, u32 end) {
    const ast::LocalRec* local = nullptr;
    if (!(key & capture_bit)) local = ctx == lambdas_.size() ? &locals_[key] : &lambdas_[ctx].outer_locals[key];
    if (local && (local->flags & ast::local_flag::structured_binding) && types().has_placeholder(local->type)) {
      diag(begin, Diag::auto_used_in_initializer, name);
      return {NodeId::none, begin, end};
    }
    if (ctx == lambdas_.size()) return source_expr(ctx, key, begin, end);
    // A function parameter pack of a template's definition: its uses are expanded, and captured,
    // in each instantiation ([temp.variadic], [expr.prim.lambda.capture]/17).
    if (local && types().kind(local->type) == TypeKind::pack_expansion) {
      if (pattern_packs_.empty()) {
        diag(begin, Diag::unexpanded_pack, name);
        return {NodeId::none, begin, end};
      }
      note_pack({0, 0, name});
      return dependent_node({}, begin, end);
    }
    // A static local is no local entity, so it is named, never captured
    // ([expr.prim.lambda.capture]/7, [basic.pre]/7).
    if (local && local->entity != EntityId::none) {
      ExprInfo info{types().referee(local->type), ValueCategory::lvalue, 0};
      return node(ast::DeclRef{info, local->entity}, begin, end, info);
    }
    // In an unevaluated operand naming it is no odr-use: no capture ([expr.prim.lambda.capture]/8),
    // only its type, and for `decltype` its declared type.
    if (unevaluated > 0) {
      QualType declared = local ? local_declared_type(*local) : lambdas_[ctx - 1].fields[key & ~capture_bit].type;
      ExprInfo info{types().referee(declared), ValueCategory::lvalue, 0};
      Ex e = node(ast::Dependent{info, b_->list(std::vector<Child>{})}, begin, end, info);
      declared_types_[std::to_underlying(e.id)] = declared;
      return e;
    }
    // A constant the innermost lambda cannot capture (no capture-default, not captured
    // explicitly) is named for its value, which is no odr-use ([basic.def.odr]/5); one it can is
    // captured, so that it is an lvalue where one is needed.
    if (!(key & capture_bit)) {
      const LambdaFrame& owner = lambdas_[ctx];
      const ast::LocalRec& l = owner.outer_locals[key];
      const LambdaFrame& inner = lambdas_.back();
      bool capturable = inner.default_capture != 0 ||
                        std::ranges::any_of(inner.captures, [&](const Capture& c) { return c.ctx == ctx && c.key == key; });
      if (!capturable && owner.outer_values[key] && !types().is_reference(l.type) && types().is_scalar(l.type))
        return literal(Types::unqualified(l.type), *owner.outer_values[key], ValueCategory::prvalue, begin, end);
    }
    std::optional<u32> field;
    for (u32 j = ctx; j < lambdas_.size(); ++j) {
      field = capture_in(j, ctx, key, field, name, begin);
      if (!field) return {NodeId::none, begin, end};
    }
    return field_expr(u32(lambdas_.size() - 1), *field, begin, end);
  }
  // Lambda `j` (whose enclosing context is `j`) capturing what (`ctx`, `key`) names: its field,
  // made now if its capture-default allows ([expr.prim.lambda.capture]/8). `inner` is the field
  // of lambda `j - 1` that names it in context `j`, when `j > ctx`.
  std::optional<u32> capture_in(u32 j, u32 ctx, u32 key, std::optional<u32> inner, NameId name, u32 at_) {
    LambdaFrame& f = lambdas_[j];
    for (const Capture& c : f.captures)
      if (c.ctx == ctx && c.key == key) return c.field;
    if (f.default_capture == 0) {
      diag(at_, Diag::not_captured, name);
      return std::nullopt;
    }
    Ex src = j == ctx ? source_expr(ctx, key, f.begin, f.begin) : field_expr(j - 1, *inner, f.begin, f.begin);
    return add_capture(f, ctx, key, name, f.default_capture == 2, src);
  }
  // A field of lambda `f`'s closure for an entity captured by copy or by reference, initialized
  // from `src` in the enclosing context ([expr.prim.lambda.capture]/10, /12, /15).
  std::optional<u32> add_capture(LambdaFrame& f, u32 ctx, u32 key, NameId name, bool by_ref, Ex src) {
    // The entity's own type, not the type `src` has through an enclosing lambda's const operator:
    // a copy keeps the entity's cv-qualifiers ([expr.prim.lambda.capture]/10).
    QualType ent = source_type(ctx, key);
    QualType t = by_ref ? types().lvalue_ref(ent) : ent;
    // Array captures that need element construction or access checks are handled in #353
    // ([expr.prim.lambda.capture]/15).
    if (!by_ref && types().is_array(t) && (classes().copy_of(t).first || destructor_of(t) != EntityId::none ||
                                         classes().defaulted_copy_of(t) != CopyState::ordinary)) {
      diag(src.begin, Diag::not_supported, lex::Tk::l_square);
      return std::nullopt;
    }
    u32 field = u32(f.fields.size());
    EntityId e = w_->entities.entity({EntityKind::data_member, name, t, LangLinkage::cxx, ast::Linkage::none, {}, f.closure});
    f.fields.push_back({name, t, e, ConstId::none, false});
    if (!(key & capture_bit)) {
      const std::vector<std::optional<ConstId>>& values = ctx == lambdas_.size() ? local_values_ : lambdas_[ctx].outer_values;
      if (key < values.size() && values[key] && types().is_scalar(ent)) f.const_fields[field] = *values[key];
    }
    // An array is copied element by element ([expr.prim.lambda.capture]/15): its member from the
    // array's value, as a class object is copied.
    f.inits.push_back(by_ref                ? bind_reference(src, t, src.begin)
                      : types().is_array(t) ? convert_node(src, ast::ConversionKind::lvalue_to_rvalue, t)
                                            : convert(src, Types::unqualified(t), src.begin));
    f.captures.push_back({ctx, key, field});
    return field;
  }
  // `this` in a lambda's body: the enclosing object's, captured by each lambda that does not
  // capture it yet ([expr.prim.lambda.capture]/8, [expr.prim.this]): its pointer field, or the
  // address of its `*this` copy.
  Ex captured_this(u32 begin, u32 end) {
    if (unevaluated > 0) {  // its type only, uncaptured
      ExprInfo info{*this_type, ValueCategory::prvalue, 0};
      return node(ast::This{info}, begin, end, info);
    }
    for (std::size_t j = 0; j < lambdas_.size(); ++j) {
      LambdaFrame& f = lambdas_[j];
      if (f.this_field) continue;
      if (f.default_capture == 0) {
        diag(begin, Diag::this_not_captured);
        return {NodeId::none, begin, end};
      }
      Ex src = j == 0 ? Ex{} : this_in(u32(j - 1), f.begin);
      if (j == 0) {
        ExprInfo info{*this_type, ValueCategory::prvalue, 0};
        src = node(ast::This{info}, f.begin, f.begin, info);
      }
      add_this(f, src, false);
    }
    return this_in(u32(lambdas_.size() - 1), begin);
  }
  // `this` as lambda `f`'s body sees it, given its closure captures it.
  Ex this_in(u32 f, u32 begin) {
    const LambdaFrame& fr = lambdas_[f];
    Ex m = field_expr(f, *fr.this_field, begin, begin);
    if (fr.star_this) {
      ExprInfo pi{types().pointer(m.type()), ValueCategory::prvalue, 0};
      return node(ast::Unary{pi, ast::UnaryOp::address_of, at(m, begin)}, begin, begin, pi);
    }
    // The pointer member's value: `this` is a prvalue of the enclosing class's pointer type.
    Ex v = rvalue(m);
    return {v.id, begin, begin, v.info};
  }
  // `this` (a pointer prvalue in the enclosing context) captured by lambda `f`: as the pointer,
  // or for `*this` the object, copied ([expr.prim.lambda.capture]/10–12).
  void add_this(LambdaFrame& f, Ex src, bool star) {
    u32 field = u32(f.fields.size());
    // `*this` copies the object as it is: in a const member function, a const one (/10).
    QualType t = star ? types().pointee(src.type()) : Types::unqualified(src.type());
    EntityId e = w_->entities.entity({EntityKind::data_member, ins().names.intern("this"), t, LangLinkage::cxx,
                                      ast::Linkage::none, {}, f.closure});
    f.fields.push_back({ins().names.intern("this"), t, e, ConstId::none, false});
    if (star) {
      ExprInfo oi{types().pointee(src.type()), ValueCategory::lvalue, 0};
      Ex obj = node(ast::Unary{oi, ast::UnaryOp::deref, at(src, src.begin)}, src.begin, src.end, oi);
      f.inits.push_back(convert(obj, t, src.begin));
    } else {
      f.inits.push_back(src);
    }
    f.this_field = field;
    f.star_this = star;
  }

  // Begins a lambda's body: its closure type and function call operator, its explicit captures
  // (whose initializers were analysed in the enclosing context), and the scope of its parameters
  // and init-captures. False after a diagnosed error, with no frame.
  bool open_lambda(LambdaSpec spec) {
    // The enclosing entity: the function, or the enclosing lambda's operator().
    EntityId scope = lambdas_.empty() ? current_function : lambdas_.back().call;
    const bool in_initializer = scope == EntityId::none && namespace_lambda && namespace_lambda->begin == spec.begin;
    if (in_initializer) {
      if (spec.default_capture || spec.has_capture || !spec.head.empty()) {
        diag(spec.begin, spec.default_capture ? Diag::namespace_lambda_capture_default
                         : spec.has_capture ? Diag::namespace_lambda_init_capture
                                            : Diag::namespace_lambda_generic);
        return false;
      }
      scope = namespace_lambda->owner;
    }
    if (scope == EntityId::none) {
      diag(spec.begin, Diag::not_supported, lex::Tk::l_square);  // a lambda outside a function body
      return false;
    }
    std::vector<QualType> ps;
    for (const LambdaParam& p : spec.params) ps.push_back(p.type);
    QualType declared = spec.trailing ? *spec.trailing : types().placeholder(false);
    QualType call_type = types().function(declared, ps, spec.variadic, LangLinkage::cxx, spec.mutable_ ? Cv::none : Cv::const_);
    // The closure's identity: its enclosing entity, signature and ordinal among the closures of
    // that entity and signature completed before it (docs/design/closures.md, decision 2).
    Fp128 sig = ast::seed("ycxx.sema.closure");
    for (QualType p : types().rec(call_type).params) sig = ast::mix(sig, types().fp(types().canonical(p)));
    sig = ast::mix(sig, u64(spec.variadic));
    // The <lambda-sig> declares the explicit template parameters only (`Ty`, `Tp`), so they and
    // the invented ones of `auto` parameters make different signatures.
    for (const ast::TemplateParamSema& h : spec.head)
      if (h.name != invented_name()) sig = ast::mix(sig, (u64(std::to_underlying(h.kind)) + 1) * 4 + u64(h.pack));
    u32 ordinal = 0;
    for (const auto& [sc, sg] : completed_closures_)
      if (sc == scope && sg == sig) ++ordinal;
    ast::Linkage linkage = in_initializer ? ast::Linkage::internal : closure_linkage(scope);
    // Its record's type, which names it (the <lambda-sig>), has a generic lambda's parameters at
    // depth 0, as the innermost template's are mangled.
    EntityRec cr{EntityKind::class_, no_name(), rebased(call_type, spec.head), LangLinkage::cxx, linkage, {}, scope};
    cr.head = ast::mix(sig, u64(ordinal));
    cr.ordinal = ordinal;
    for (const ast::TemplateParamSema& h : spec.head)
      if (h.name != invented_name()) cr.closure_head.push_back(h);
    LambdaFrame f;
    f.closure = w_->entities.entity(std::move(cr));
    f.call = w_->entities.entity({EntityKind::function, ins().names.intern("operator()"), call_type, LangLinkage::cxx, linkage, {}, f.closure});
    f.call_type = call_type;
    f.sig = sig;
    f.scope = scope;
    f.begin = spec.begin;
    f.default_capture = spec.default_capture;
    f.has_capture = spec.has_capture;
    f.mutable_ = spec.mutable_;
    f.ordinal = ordinal;
    f.head = spec.head;
    f.diags_at = diag_count();
    QualType ct = types().class_type(f.closure);
    f.this_type = types().pointer(spec.mutable_ ? ct : types().add_cv(ct, Cv::const_));
    // [expr.prim.lambda.capture]/5: nor a template parameter's.
    for (const CaptureSpec& c : spec.captures)
      if (c.name != no_name() && std::ranges::any_of(spec.head, [&](const ast::TemplateParamSema& h) { return h.name == c.name; })) {
        diag(c.at, Diag::redeclared_local, c.name);
        return false;
      }
    // [expr.prim.lambda.capture]/5: a simple capture's name is not a parameter's.
    for (const LambdaParam& p : spec.params)
      if (p.name != no_name() && std::ranges::any_of(spec.captures, [&](const CaptureSpec& c) { return c.name == p.name; })) {
        diag(p.at, Diag::redeclared_local, p.name);
        return false;
      }
    for (CaptureSpec& c : spec.captures) {
      using K = CaptureSpec::Kind;
      if (c.pack) {
        // A pack capture: each element of an instantiation's function parameter pack; in a
        // template's definition, nothing yet.
        auto it = std::ranges::find(param_packs_, c.name, &ParamPack::name);
        if (it == param_packs_.end()) {
          auto src = capture_source(c.name);
          if (!src || src->second & capture_bit || types().kind(source_type(src->first, src->second)) != TypeKind::pack_expansion) {
            diag(c.at, Diag::cannot_capture, c.name);
            return false;
          }
          continue;
        }
        for (u32 l : it->locals) {
          Ex src = captured(it->ctx, l, c.name, c.at, c.at + 1);
          if (!src.ok() || !add_capture(f, it->ctx, l, c.name, c.kind == K::ref, src)) return false;
        }
        continue;
      }
      if (!c.init.ok() && c.kind != K::this_ && c.kind != K::star_this) return false;
      bool twice = std::ranges::any_of(f.fields, [&](const FieldRec& x) { return x.name == c.name && c.name != no_name(); });
      if (twice || ((c.kind == K::this_ || c.kind == K::star_this) && f.this_field)) {
        diag(c.at, Diag::capture_twice, c.name);
        return false;
      }
      switch (c.kind) {
      case K::copy:
      case K::ref:
        // A variable with static storage duration is not captured ([expr.prim.lambda.capture]/5).
        if (!(c.source->second & capture_bit) &&
            (c.source->first == lambdas_.size() ? locals_[c.source->second] : lambdas_[c.source->first].outer_locals[c.source->second])
                    .entity != EntityId::none) {
          diag(c.at, Diag::cannot_capture, c.name);
          return false;
        }
        if (!add_capture(f, c.source->first, c.source->second, c.name, c.kind == K::ref, c.init)) return false;
        break;
      case K::this_:
      case K::star_this: {
        Ex t = this_expr(c.at, c.at);
        if (!t.ok()) return false;
        add_this(f, t, c.kind == K::star_this);
        break;
      }
      case K::init_copy:
      case K::init_ref: {
        // As `auto name = init;` or `auto& name = init;` ([expr.prim.lambda.capture]/6).
        QualType p = types().placeholder(false);
        if (c.kind == K::init_ref) p = types().lvalue_ref(p);
        auto t = deduce_placeholder(p, c.init, c.init.begin);
        if (!t) return false;
        u32 field = u32(f.fields.size());
        EntityId e = w_->entities.entity({EntityKind::data_member, c.name, *t, LangLinkage::cxx, ast::Linkage::none, {}, f.closure});
        f.fields.push_back({c.name, *t, e, ConstId::none, false});
        f.inits.push_back(types().is_reference(*t) ? bind_reference(c.init, *t, c.init.begin) : convert(c.init, *t, c.init.begin));
        f.init_captures.push_back({c.name, field});
        break;
      }
      }
    }
    // The enclosing context's state, until the body ends.
    f.outer_locals = std::exchange(locals_, {});
    f.outer_values = std::exchange(local_values_, {});
    f.return_type = return_type_;
    f.declared_return = declared_return_;
    f.return_deduced = return_deduced_;
    f.return_seen = return_seen_;
    f.no_return_deduction = std::exchange(no_return_deduction, 0);
    f.loop_depth = std::exchange(loop_depth, 0);
    set_return_type(declared);
    f.scope_base = scopes_.size();
    lambdas_.push_back(std::move(f));
    push_scope();
    guard_scope();
    for (const LambdaParam& p : spec.params) declare_local(p.name, p.type, local_flag::parameter, p.at, false);
    for (const auto& [n, field] : lambdas_.back().init_captures) scopes_.back().push_back({n, capture_bit | field});
    return true;
  }
  // The linkage of a closure in `scope` (docs/design/closures.md, decision 4): external in a
  // function whose definition may appear in several units, internal elsewhere.
  ast::Linkage closure_linkage(EntityId scope) {
    if (!lambdas_.empty()) return w_->entities.rec(lambdas_.back().closure).linkage;
    const EntityRec& r = w_->entities.rec(scope);
    return r.linkage == ast::Linkage::external && (function_inline || r.templ != EntityId::none) ? ast::Linkage::external
                                                                                                 : ast::Linkage::internal;
  }
  // Whether the function whose body is analysed is inline ([dcl.inline]): its closures' members
  // are then defined in every unit that defines it. Set by the body's analysis.
  bool function_inline = false;
  // Inside an unevaluated operand (`sizeof`, `decltype`, [expr.context]/1).
  u32 unevaluated = 0;
  bool checking_virtual_body = false;

  // Ends the body of the innermost lambda (`body`, from `body_begin`, the lambda's tokens ending
  // at `end`): its return type deduced, its closure's record published, and the lambda-expression,
  // a prvalue of the closure type, in the enclosing context.
  Ex close_lambda(Ex body, u32 body_begin, u32 end) {
    LambdaFrame& f = lambdas_.back();
    // No return statement deduces `void` ([dcl.type.auto.deduct]/2), unless a template's
    // definition left the type to its instantiations.
    const bool generic = !f.head.empty();
    if (body.ok() && deduces_return() && !return_deduced_ && !return_seen_ && !in_pattern())
      if (!deduce_return(nullptr, end > 0 ? end - 1 : 0)) body = {};
    // A generic lambda's operator keeps its declared type: each specialization deduces its own.
    QualType call_type = f.call_type;
    if (generic) {
    } else if (return_deduced_) {
      call_type = with_return(f.call_type, return_type_);
    } else if (deduces_return()) {
      call_type = with_return(f.call_type, builtin(Builtin::dependent));
    }
    std::vector<ast::LocalRec> locals = std::move(locals_);
    std::size_t nparams = types().rec(call_type).params.size();
    pop_scope();
    LambdaFrame fr = std::move(lambdas_.back());
    lambdas_.pop_back();
    locals_ = std::move(fr.outer_locals);
    local_values_ = std::move(fr.outer_values);
    return_type_ = fr.return_type;
    declared_return_ = fr.declared_return;
    return_deduced_ = fr.return_deduced;
    return_seen_ = fr.return_seen;
    no_return_deduction = fr.no_return_deduction;
    loop_depth = fr.loop_depth;
    completed_closures_.push_back({fr.scope, fr.sig});
    closure_calls_[std::to_underlying(fr.call)] = call_type;
    if (!body.ok() || !require_supported_virtual_signature(types().rec(call_type), fr.begin))
      return {NodeId::none, fr.begin, end};
    // The closure type ([expr.prim.lambda.closure]): its captures' fields, its function call
    // operator, and for a lambda without a lambda-capture the static invoker its conversion to a
    // pointer to function returns (/8–9).
    ClassRec rec;
    rec.defined = true;
    rec.closure = true;
    rec.default_deleted = fr.has_capture;
    rec.fields = fr.fields;
    for (FieldRec& fd : rec.fields) fd.name = no_name();  // named by its lambda's body only
    rec.members.push_back({ins().names.intern("operator()"), fr.call, call_type, true, false, false, false, false, generic, fr.head, {}});
    // A generic lambda's conversion is a template (/10), not yet (#339).
    bool captureless = !fr.has_capture && !generic && !types().is_dependent(call_type);
    EntityId invoker = EntityId::none;
    NodeId invoker_body = NodeId::none;
    if (captureless) {
      TypeRec cr = types().rec(call_type);
      QualType it = types().function(cr.inner, cr.params, cr.variadic, LangLinkage::cxx);
      bool simple = std::ranges::none_of(cr.params, [&](QualType p) { return types().is_class(p); }) && !cr.variadic;
      if (simple) {
        invoker = w_->entities.entity({EntityKind::function, ins().names.intern("__invoke"), it, LangLinkage::cxx,
                                       w_->entities.rec(fr.closure).linkage, {}, fr.closure});
        rec.members.push_back({ins().names.intern("__invoke"), invoker, it, true, true, false, false, false, false, {}, {}});
        invoker_body = invoker_tree(fr, call_type, nparams, body_begin);
      }
    }
    q_->publish_class(fr.closure, std::move(rec));
    u16 flags = u16((fr.mutable_ ? closure_flag::mutable_ : 0) | (captureless ? closure_flag::captureless : 0) |
                    (generic || in_pattern() ? closure_flag::pattern : 0));
    // The enclosing function's constants a generic body names stand as their values when it is
    // instantiated, where that function's locals are gone.
    // The enclosing context's locals visible here, innermost first and one per name: constants
    // with their values, the others for unevaluated operands; and its type aliases.
    std::vector<ast::LocalRec> consts;
    std::vector<ConstId> values;
    std::vector<std::pair<NameId, QualType>> aliases;
    if (generic) {
      std::set<u32> seen;
      for (std::size_t d = scopes_.size(); d-- > 0 && context_of(d) == lambdas_.size();)
        for (auto p = scopes_[d].rbegin(); p != scopes_[d].rend(); ++p) {
          if ((p->second & capture_bit) || p->first == no_name() || !seen.insert(std::to_underlying(p->first)).second) continue;
          consts.push_back(locals_[p->second]);
          consts.back().decl = NodeId::none;
          values.push_back(p->second < local_values_.size() && local_values_[p->second] ? *local_values_[p->second] : ConstId::none);
        }
      for (const BlockType& t : block_types_)
        if (t.depth < scopes_.size()) aliases.push_back({t.name, t.type});
    }
    if (generic) {
      std::vector<NameId> names;
      for (std::size_t i = 0; i < nparams && i < locals.size(); ++i) names.push_back(locals[i].name);
      std::vector<NameId> captures;
      for (const FieldRec& fd : fr.fields) captures.push_back(fd.name);
      std::vector<Expansion> exps;
      for (const Expansion& x : expansions)
        if (x.begin >= body_begin && x.begin < end) exps.push_back(x);
      std::vector<std::vector<ast::TemplateParamSema>> outer(template_scopes.begin(), template_scopes.end() - (template_scopes.empty() ? 0 : 1));
      generic_closures_[std::to_underlying(fr.call)] = {fr.closure, fr.call, call_type, fr.head, names, captures, body_begin, flags, consts,
                                                         values, exps, outer, template_args, aliases, diag_count() > fr.diags_at};
      q_->generic_defined(generic_closures_[std::to_underlying(fr.call)]);
    }
    std::vector<NameId> capture_names;
    for (const FieldRec& fd : fr.fields) capture_names.push_back(fd.name);
    closure_locals.push_back(locals);
    closures.push_back({fr.closure, fr.call, call_type, body.id, b_->seq(std::span<const ast::LocalRec>(locals)), invoker,
                        EntityId::none, invoker_body, NodeId::none, fr.begin, body_begin, end, flags,
                        b_->seq(std::span<const ast::TemplateParamSema>(fr.head)), b_->seq(std::span<const ast::LocalRec>(consts)),
                        b_->seq(std::span<const ConstId>(values)), b_->seq(std::span<const NameId>(capture_names))});
    std::vector<Child> inits;
    for (std::size_t i = 0; i < fr.inits.size(); ++i) {
      const Ex& x = fr.inits[i];
      if (!x.ok()) return {NodeId::none, fr.begin, end};
      Ex mi = node(ast::MemberInit{u32(i), fr.fields[i].type, at(x, x.begin)}, x.begin, x.end);
      inits.push_back(at(mi, fr.begin));
    }
    ExprInfo info{types().class_type(fr.closure), ValueCategory::prvalue, 0};
    return node(ast::Lambda{info, fr.closure, b_->list(inits)}, fr.begin, end, info);
  }
  // The static invoker's body ([expr.prim.lambda.closure]/9): the function call operator of a
  // closure object called with its own parameters, `{ return Closure{}(params...); }`; its locals
  // are the operator's parameters, the first `nparams` of the operator's locals.
  NodeId invoker_tree(const LambdaFrame& fr, QualType call_type, std::size_t nparams, u32 at_) {
    TypeRec cr = types().rec(call_type);
    QualType ct = types().class_type(fr.closure);
    ExprInfo li{ct, ValueCategory::prvalue, 0};
    Ex closure = node(ast::Lambda{li, fr.closure, b_->list(std::vector<Child>{})}, at_, at_, li);
    Ex object = materialize(closure, ct, false);
    ExprInfo pi{types().pointer(ct), ValueCategory::prvalue, 0};
    Ex self = node(ast::Unary{pi, ast::UnaryOp::address_of, at(object, at_)}, at_, at_, pi);
    std::vector<Child> args;
    for (std::size_t i = 0; i < nparams; ++i) {
      QualType p = cr.params[i];
      ExprInfo ai{types().referee(p), ValueCategory::lvalue, 0};
      Ex a = node(ast::LocalRef{ai, u32(i)}, at_, at_, ai);
      if (!types().is_reference(p)) a = rvalue(a);
      args.push_back(at(a, at_));
    }
    ExprInfo ri = result_info(cr.inner);
    Ex call = node(ast::MemberCall{ri, fr.call, at(self, at_), b_->list(args)}, at_, at_, ri);
    if (types().is_void(cr.inner)) {
      Ex st = node(ast::ExprStmt{at(call, at_)}, at_, at_);
      Ex ret = node(ast::Return{at(Ex{}, at_)}, at_, at_);
      return node(ast::Compound{b_->list(std::vector<Child>{at(st, at_), at(ret, at_)})}, at_, at_).id;
    }
    Ex ret = node(ast::Return{at(call, at_)}, at_, at_);
    return node(ast::Compound{b_->list(std::vector<Child>{at(ret, at_)})}, at_, at_).id;
  }
  bool has_generic_lambdas() const { return !generic_closures_.empty(); }
  // Whether this is a template's definition (or a generic lambda's), not an instantiation.
  bool in_pattern() const { return templated() && !instantiating(); }
  // `t` with a generic lambda's template parameters (`head`, at its depth) at depth 0, as they
  // are mangled inside its closure type's name (Itanium C++ ABI §5.1.8).
  QualType rebased(QualType t, std::span<const ast::TemplateParamSema> head) {
    if (head.empty() || head[0].depth == 0) return t;
    std::vector<TemplateArg> to;
    for (const ast::TemplateParamSema& h : head)
      to.push_back(h.kind == ast::TemplateArgKind::type ? templates().type_arg(types().template_param(0, h.index, h.pack))
                                                         : TemplateArg{h.kind, h.type, ins().intern_value(ast::VDepParam{0, h.index})});
    return templates().substitute(t, Bindings::of(head[0].depth, to)).value_or(t);
  }

  // A generic lambda's function call operator specialized for `args` ([temp.inst]): its
  // specialization's entity and type, the return type deduced by instantiating the body
  // (`Questions::generic_instance`). None after a diagnosed error.
  struct GenericCall {
    EntityId spec;
    QualType type;
  };
  std::optional<GenericCall> generic_call(const MemberDeclRec& m, std::span<const TemplateArg> args, u32 at_) {
    ast::DeclSema tmpl{};
    tmpl.entity = m.entity;
    tmpl.type = m.type;
    tmpl.kind = ast::DeclKind::function;
    tmpl.template_params = ins().intern_seq(std::span<const ast::TemplateParamSema>(m.head));
    auto st = templates().specialization_type(tmpl, args);
    if (!st) return std::nullopt;
    EntityId spec = specialization(tmpl, args, *st);
    // In a template's definition, its instantiation decides ([temp.dep.expr]/3).
    if (in_pattern()) return GenericCall{spec, with_return(*st, builtin(Builtin::dependent))};
    // In its own body, once a return statement deduced its type ([dcl.spec.auto.general]/11).
    if (!lambdas_.empty() && lambdas_.back().call == spec && return_deduced_) return GenericCall{spec, with_return(*st, return_type_)};
    const GenericClosure* known = nullptr;
    if (auto g = generic_closures_.find(std::to_underlying(m.entity)); g != generic_closures_.end()) known = &g->second;
    if (known && known->erroneous) return std::nullopt;  // its definition's errors say it
    // Its definition is required ([temp.inst]/8): emitted, and what it requires with it.
    demands.push_back({spec, ast::Loc{item_, loc_base_ + at_}});
    GenericBinding binding{template_args, current_function};
    GenericResult t = q_->generic_instance(spec, known, binding);
    if (!t.type) {
      if (!t.diagnosed) diag(at_, Diag::auto_undeduced, w_->entities.rec(m.entity).name);
      return std::nullopt;
    }
    return GenericCall{spec, *t.type};
  }

  // Begins a specialization of a generic lambda's function call operator, `spec` of type
  // `spec_type` for `args`: the closure's frame rebuilt from its record (every capture is a field
  // by name), the head bound, its parameters declared. The enclosing function's constants are the
  // current context's locals.
  void open_generic(const GenericClosure& g, EntityId spec, QualType spec_type, std::span<const TemplateArg> args) {
    LambdaFrame f;
    // The template-heads around its definition, with their arguments, then its own.
    f.outer_scopes = std::exchange(template_scopes, g.scopes);
    f.outer_args = std::exchange(template_args, g.args);
    template_scopes.push_back(g.head);
    u16 depth = g.head.empty() ? 0 : g.head[0].depth;
    if (template_args.levels.size() <= depth) template_args.levels.resize(depth + 1);
    template_args.levels[depth].assign(args.begin(), args.end());
    f.closure = g.closure;
    f.call = spec;
    f.call_type = spec_type;
    f.scope = spec;
    f.mutable_ = (g.flags & closure_flag::mutable_) != 0;
    QualType ct = types().class_type(g.closure);
    f.this_type = types().pointer(f.mutable_ ? ct : types().add_cv(ct, Cv::const_));
    if (const ClassRec* r = class_rec(g.closure)) f.fields = r->fields;
    for (u32 i = 0; i < f.fields.size() && i < g.captures.size(); ++i) f.fields[i].name = g.captures[i];
    for (u32 i = 0; i < f.fields.size(); ++i)
      if (f.fields[i].name == ins().names.intern("this")) {
        f.this_field = i;
        f.star_this = !types().is_pointer(f.fields[i].type);
      }
    f.outer_locals = std::exchange(locals_, {});
    f.outer_values = std::exchange(local_values_, {});
    f.return_type = return_type_;
    f.declared_return = declared_return_;
    f.return_deduced = return_deduced_;
    f.return_seen = return_seen_;
    f.no_return_deduction = std::exchange(no_return_deduction, 0);
    f.loop_depth = std::exchange(loop_depth, 0);
    set_return_type(types().rec(spec_type).inner);
    f.scope_base = scopes_.size();
    lambdas_.push_back(std::move(f));
    push_scope();
    guard_scope();
    // The parameters: a pack's elements are its specialization's parameters at its place
    // ([temp.variadic]/8).
    TypeRec r = types().rec(spec_type);
    TypeRec pat = types().rec(g.call_type);
    std::size_t elems = r.params.size() + 1 >= pat.params.size() ? r.params.size() + 1 - pat.params.size() : 0;
    for (std::size_t i = 0, k = 0; i < pat.params.size(); ++i) {
      NameId n = i < g.params.size() ? g.params[i] : no_name();
      if (types().kind(pat.params[i]) == TypeKind::pack_expansion) {
        declare_param_pack(n);
        for (std::size_t e = 0; e < elems && k < r.params.size(); ++e) declare_pack_element(n, r.params[k++]);
        continue;
      }
      if (k < r.params.size()) declare_local(n, r.params[k++], local_flag::parameter, 0, false);
    }
    const LambdaFrame& fr = lambdas_.back();
    for (u32 i = 0; i < fr.fields.size(); ++i)
      if (fr.fields[i].name != no_name() && (!fr.this_field || *fr.this_field != i)) scopes_.back().push_back({fr.fields[i].name, capture_bit | i});
  }
  // Ends it: the specialization's type with the return type deduced, and its locals; none after
  // an error.
  struct GenericBody {
    QualType type;
    std::vector<ast::LocalRec> locals;
  };
  std::optional<GenericBody> close_generic(const Ex& body, u32 end) {
    LambdaFrame& f = lambdas_.back();
    if (body.ok() && deduces_return() && !return_deduced_ && !return_seen_)
      if (!deduce_return(nullptr, end > 0 ? end - 1 : 0)) {}
    std::optional<GenericBody> r;
    if (body.ok() && (return_deduced_ || !deduces_return()))
      r = GenericBody{deduces_return() ? with_return(f.call_type, return_type_) : f.call_type, locals_};
    pop_scope();
    LambdaFrame fr = std::move(lambdas_.back());
    lambdas_.pop_back();
    locals_ = std::move(fr.outer_locals);
    local_values_ = std::move(fr.outer_values);
    return_type_ = fr.return_type;
    declared_return_ = fr.declared_return;
    return_deduced_ = fr.return_deduced;
    return_seen_ = fr.return_seen;
    no_return_deduction = fr.no_return_deduction;
    loop_depth = fr.loop_depth;
    template_scopes = std::move(fr.outer_scopes);
    template_args = std::move(fr.outer_args);
    return r;
  }

  // A captureless closure converts to a pointer to its function call operator's function type
  // ([expr.prim.lambda.closure]/8): its static invoker, when `to` is that pointer type.
  std::optional<EntityId> closure_invoker(QualType from, QualType to) {
    if (!types().is_class(from) || !types().is_pointer(to)) return std::nullopt;
    const ClassRec* r = class_rec(types().class_of(from));
    if (!r) return std::nullopt;
    for (const MemberDeclRec& m : r->members)
      if (m.is_static && m.name == ins().names.intern("__invoke") &&
          types().canonical(types().pointer(m.type)) == types().canonical(Types::unqualified(to)))
        return m.entity;
    return std::nullopt;
  }
  // The conversion's result: the invoker's address, after `e`, which names the closure object,
  // is evaluated and discarded (the conversion function returns the address, /8).
  Ex invoker_pointer(Ex e, EntityId invoker) {
    QualType ft = w_->entities.rec(invoker).type;
    ExprInfo fi{ft, ValueCategory::lvalue, 0};
    Ex f = node(ast::DeclRef{fi, invoker}, e.begin, e.end, fi);
    Ex p = convert_node(f, ast::ConversionKind::function_to_pointer, types().pointer(ft));
    if (as<ast::LocalRef>(e) || as<ast::Lambda>(e)) return {p.id, e.begin, e.end, p.info};
    Ex d = convert_node(discarded(e), ast::ConversionKind::to_void, builtin(Builtin::void_));
    return node(ast::OrderedBinary{p.info, ast::OrderedOp::comma, at(d, e.begin), at(p, e.begin)}, e.begin, e.end, p.info);
  }

  // The closures of this part's lambdas, in the order their bodies ended, and each one's
  // operator()'s locals, which constant evaluation reads before the part is committed.
  std::vector<ast::ClosureDef> closures;
  std::vector<std::vector<ast::LocalRec>> closure_locals;
  static bool closure_has_code(const World& w, const ast::ClosureDef& c) {
    return !(c.flags & closure_flag::pattern) && !w.types.is_dependent(c.call_type);
  }
  // A member of a closure type: its function call operator or static invoker, constexpr when it
  // can be ([expr.prim.lambda.closure]/5: the operator is constexpr if it satisfies the
  // requirements; a constant evaluation that it does not satisfy fails anyway).
  // The declaration and body of closure `c`'s member `f`, its function call operator or static
  // invoker, as `function_ir` reads them: `body` the tree, `locals` the operator's (the invoker
  // has its parameters only), and the `this` type for the operator.
  struct ClosureMember {
    ast::DeclSema decl;
    ast::BodySema body;
    std::optional<QualType> this_type;
  };
  static ClosureMember closure_member_def(World& w, const ast::ClosureDef& c, EntityId f, NodeId body,
                                          std::vector<ast::LocalRec> locals) {
    ast::Interners& in = w.ins;
    bool call = c.call == f;
    std::size_t n = w.types.rec(c.call_type).params.size();
    if (!call) locals.resize(std::min(n, locals.size()));
    std::vector<ast::ParamSema> params;
    for (std::size_t k = 0; k < n && k < locals.size(); ++k) params.push_back({EntityId::none, locals[k].type, locals[k].name, NodeId::none, 0});
    ast::DeclSema d{};
    d.entity = f;
    d.type = call ? c.call_type : w.entities.rec(f).type;
    d.kind = ast::DeclKind::function;
    d.linkage = w.entities.rec(f).linkage;
    d.flags = u16(decl_flag::member | decl_flag::definition | decl_flag::inline_ | decl_flag::constexpr_ | (call ? 0 : decl_flag::static_));
    d.attributes = in.intern_seq(std::span<const NodeId>{});
    d.constraint = NodeId::none;
    d.constraints = ConstId::none;
    d.contracts = NodeId::none;
    d.params = in.intern_seq(std::span<const ast::ParamSema>(params));
    d.template_params = in.intern_seq(std::span<const ast::TemplateParamSema>{});
    ast::BodySema b{body, in.intern_seq(std::span<const ast::LocalRec>(locals)), in.intern_seq(std::span<const ast::ExpansionRec>{}),
                    in.intern_seq(std::span<const u32>{}), QualType{}, 0, in.intern_seq(std::span<const ast::ClosureDef>{})};
    std::optional<QualType> this_type;
    if (call) {
      QualType ct = w.types.class_type(c.closure);
      if (!(c.flags & closure_flag::mutable_)) ct = w.types.add_cv(ct, Cv::const_);
      this_type = w.types.pointer(ct);
    }
    return {d, b, this_type};
  }
  static bool closure_member(const World& w, EntityId f) {
    const EntityRec* r = w.entities.find(f);
    if (!r || r->scope == EntityId::none) return false;
    const EntityRec* c = w.entities.find(r->scope);
    return c && c->kind == EntityKind::class_ && w.ins.names.spelling(c->name).empty();
  }

  // ------------------------------------------------------------ the function being defined

  void set_return_type(QualType t) {
    return_type_ = t;
    declared_return_ = t;
    return_deduced_ = false;
    return_seen_ = false;
  }
  QualType return_type() const { return return_type_; }
  bool variadic_function_body = false;  // BodyAnalysis's function, excluding nested lambda bodies
  // A declared return type holding a placeholder ([dcl.spec.auto.general]/3): each return
  // statement deduces it, and the first one's deduction is the return type from then on (/11).
  bool deduces_return() const { return w_->types.has_placeholder(declared_return_); }
  bool return_deduced() const { return return_deduced_; }
  // Whether a return statement was seen, deducing or failing to.
  bool return_seen() const { return return_seen_; }
  // Inside a discarded statement or a dependent `if constexpr`: returns deduce nothing.
  u32 no_return_deduction = 0;
  // What `return e;` (`e` none for `return;` or the end of the body) deduces: the return type,
  // or none after a diagnosed error. In a template's definition a type-dependent `e` deduces
  // nothing; each instantiation does ([dcl.spec.auto.general]/12).
  std::optional<QualType> deduce_return(const Ex* e, u32 at_) {
    std::optional<QualType> t;
    QualType replaced{};
    if (!e) {
      // [dcl.type.auto.deduct]/2.2: as from a void operand, for `auto` or `decltype(auto)` alone.
      if (types().kind(declared_return_) != TypeKind::placeholder) {
        diag(at_, Diag::auto_deduction_failed, declared_return_, builtin(Builtin::void_));
        return std::nullopt;
      }
      t = types().add_cv(builtin(Builtin::void_), Types::cv(declared_return_));
    } else {
      t = deduce_placeholder(declared_return_, *e, at_, &replaced);
    }
    // In a discarded statement, or a branch of a dependent `if constexpr`, the operand is checked
    // against what it alone deduces.
    if (no_return_deduction > 0) return !t || types().is_dependent(*t) || !return_deduced_ ? t : std::optional(return_type_);
    return_seen_ = true;
    if (!t || types().is_dependent(*t)) return t;
    if (return_deduced_ && types().canonical(*t) != types().canonical(return_type_)) {
      diag(at_, Diag::auto_inconsistent, declared_return_, *t, return_type_);
      return std::nullopt;
    }
    return_deduced_ = true;
    return_type_ = *t;
    return t;
  }
  // `f`'s function type `t` with its return type deduced, for naming it: by its own body's return
  // statements so far, or by its definition before this point ([dcl.spec.auto.general]/11);
  // none after a diagnosed error.
  std::optional<QualType> deduced_function(EntityId f, QualType t, u32 at_) {
    if (!types().is_function(t) || !types().has_placeholder(types().rec(t).inner)) return t;
    if (f == current_function && return_deduced_) return with_return(t, return_type_);
    if (auto c = closure_calls_.find(std::to_underlying(f)); c != closure_calls_.end()) return c->second;
    if (f != current_function)
      if (auto d = q_->deduced_type(f)) return *d;
    // In a template's definition, naming a member of the current instantiation (or anything else
    // undeduced) is type-dependent: each instantiation deduces it ([temp.dep.expr]/3).
    if (templated() && !instantiating()) return with_return(t, builtin(Builtin::dependent));
    diag(at_, Diag::auto_undeduced, w_->entities.rec(f).name);
    return std::nullopt;
  }
  QualType with_return(QualType fn, QualType ret) {
    TypeRec r = types().rec(fn);
    return types().function(ret, r.params, r.variadic, r.linkage, r.fn_cv);
  }
  u32 loop_depth = 0;

  // ------------------------------------------------------------ jumps ([stmt.jump], [stmt.dcl]/2)

  // A block-scope variable whose initialization is not vacuous ([basic.life]/2): a jump may leave
  // its scope, but never enter it past its declaration ([stmt.dcl]/2).
  void set_initialized(u32 local) {
    if (initialized_.size() <= local) initialized_.resize(local + 1);
    initialized_[local] = true;
  }
  // Those of them in scope here, in declaration order, then the barriers open here.
  std::vector<u32> initialized_in_scope() const {
    std::vector<u32> r;
    for (const auto& s : scopes_)
      for (const auto& [name, l] : s)
        if (l < initialized_.size() && initialized_[l]) r.push_back(l);
    for (const auto& [depth, id] : barriers_) r.push_back(id);
    return r;
  }
  // Makes the innermost scope one no jump may enter, as if it began with an initialized variable:
  // a substatement of `if constexpr` or `if consteval` ([stmt.if]/2, /4).
  void jump_barrier() { barriers_.push_back({scopes_.size() - 1, 0x80000000u | next_barrier_++}); }
  // Whether a jump from a point where `from` are in scope to one where `to` are bypasses one.
  static bool bypasses(std::span<const u32> from, std::span<const u32> to) {
    return std::ranges::any_of(to, [&](u32 l) { return !std::ranges::contains(from, l); });
  }

  // The switch statements being analysed, innermost last ([stmt.switch]): the condition's type
  // after promotion, the variables in scope at the jump to a label, and the labels so far.
  struct SwitchScope {
    QualType type;
    bool dependent = false;
    std::vector<u32> initialized;
    std::vector<ConstId> values;
    bool has_default = false;
  };
  std::vector<SwitchScope> switches;

  // [stmt.switch]/2, /5: a switch's condition has integral or enumeration type, and is promoted.
  Ex switch_condition(Ex e, u32 at_) {
    e = rvalue(e);
    if (!e.ok() || dependent(e)) return e;
    if (!types().is_integral(e.type()) && !types().is_enum(e.type())) {
      diag(at_, Diag::switch_not_integral, e.type());
      return {NodeId::none, e.begin, e.end};
    }
    return promote(e);
  }

  // Labels and `goto`s ([stmt.label], [stmt.goto]): a label has function scope, so uses are
  // checked when the body ends (`check_labels`).
  void define_label(NameId name, u32 at_) {
    if (std::ranges::any_of(labels_, [&](const LabelPoint& l) { return l.name == name; })) diag(at_, Diag::redefined_label, name);
    else labels_.push_back({name, at_, initialized_in_scope()});
  }
  void use_label(NameId name, u32 at_) { gotos_.push_back({name, at_, initialized_in_scope()}); }
  void check_labels() {
    for (const LabelPoint& g : gotos_) {
      auto l = std::ranges::find(labels_, g.name, &LabelPoint::name);
      if (l == labels_.end()) diag(g.at, Diag::undeclared_label, g.name);
      else if (bypasses(g.initialized, l->initialized)) diag(g.at, Diag::jump_past_init);
    }
  }

  // A reference to block-scope variable `local` ([expr.prim.id.unqual]).
  Ex local_ref(u32 local, u32 begin, u32 end) {
    return source_expr(static_cast<u32>(lambdas_.size()), local, begin, end);
  }
  // Inside an immediate function context ([expr.const]): a consteval function's body, or the
  // consteval branch of `if consteval`. A call to a consteval function elsewhere is an immediate
  // invocation, evaluated where it appears.
  u32 immediate_context = 0;
  bool is_consteval(EntityId f) const { return consteval_.contains(std::to_underlying(f)); }
  // An immediate function may be named within an immediate invocation ([expr.const]). Wait
  // until the completed tree, where a folded invocation no longer contains the address operand.
  void check_immediate_addresses(NodeId root, u32 offset = 0) {
    if (!has_immediate_addresses_ || root == NodeId::none) return;
    auto check = [&](this const auto& self, const ast::Red& node) -> void {
      if (node.is<ast::DeclRef>()) {
        auto ref = node.get<ast::DeclRef>();
        if (ref.info.flags & expr_flag::immediate_address)
          diag(node.offset(), Diag::immediate_address, w_->entities.rec(ref.entity).name);
      }
      node.for_each_child(self);
    };
    check(ast::Red(ins().ast, root, offset));
  }

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
  struct NamespaceLambda {
    EntityId owner, scope;
    NameId name;
    u32 begin;
  };
  std::optional<NamespaceLambda> namespace_lambda;

  Ex undeduced_namespace_lambda(NameId name, u32 begin, u32 end) {
    diag(begin, Diag::auto_used_in_initializer, name);
    return {NodeId::none, begin, end};
  }

  // [class.access]/1–2: whether a member of class `c` with access `a` can be named here. A public
  // one anywhere; a private or protected one in a member of `c` or of a class nested in it
  // ([class.access.nest]), or in a friend of `c`: a function, or a class whose members are then
  // all friends ([class.friend]/2); a protected one also in a member or friend of a class derived
  // from `c` ([class.protected], without its restriction on the object expression yet).
  // `naming` is the class the member is named in ([class.access.base]/5), when it is a class
  // derived from `c`: a protected member may then be named in its members and friends too.
  bool accessible(EntityId c, ast::Access a, EntityId naming = EntityId::none) {
    if (a == ast::Access::public_ || a == ast::Access::none) return true;
    if (a == ast::Access::protected_ && naming != EntityId::none && naming != c && accessible(naming, ast::Access::private_))
      return true;
    const ClassRec* cr = class_rec(c);
    if (!cr) return true;
    auto befriended = [&](EntityId e) { return std::ranges::find(cr->friends, e) != cr->friends.end(); };
    if (current_function != EntityId::none && befriended(current_function)) return true;
    for (EntityId k = current_class; k != EntityId::none; k = w_->entities.rec(k).scope) {
      const EntityRec& r = w_->entities.rec(k);
      if (r.kind != EntityKind::class_ && r.kind != EntityKind::class_template) break;
      if (k == c || befriended(k)) return true;
      int paths = 0;
      if (a == ast::Access::protected_ && base_path(k, c, paths)) return true;
    }
    return false;
  }
  // Whether member `m` of class `c`, named `name` at `at` in class `naming`, is accessible here;
  // diagnosed if not.
  bool check_access(EntityId c, EntityId m, NameId name, u32 at, EntityId naming = EntityId::none) {
    const ClassRec* cr = class_rec(c);
    if (!cr) return true;
    ast::Access a = cr->access_of(m);
    if (!accessible(c, a, naming)) {
      diag(at, a == ast::Access::protected_ ? Diag::protected_member : Diag::private_member, name, types().class_type(c));
      return false;
    }
    if (!accessible_naming_base(naming, c)) {
      diag(at, Diag::inaccessible_base, types().class_type(c), types().class_type(naming));
      return false;
    }
    return true;
  }

  // Member access through a class template's instantiated base graph ([class.access.base]). A
  // static member can be shared through several subobjects, unlike a base conversion.
  bool accessible_naming_base(EntityId naming, EntityId owner) {
    if (naming == owner || naming == EntityId::none || w_->entities.rec(naming).templ == EntityId::none) return true;
    auto path = [&](this auto&& next, EntityId from) -> bool {
      if (from == owner) return true;
      const ClassRec* rec = class_rec(from);
      return rec && std::ranges::any_of(rec->bases, [&](const BaseRec& base) {
        return accessible(from, base.access) && next(base.cls);
      });
    };
    return path(naming);
  }

  Ex this_expr(u32 begin, u32 end) {
    if (!this_type) {
      diag(begin, Diag::this_outside_member);
      return {NodeId::none, begin, end};
    }
    if (!lambdas_.empty()) return captured_this(begin, end);
    ExprInfo info{*this_type, ValueCategory::prvalue, 0};
    return node(ast::This{info}, begin, end, info);
  }

  // A member of class `c` named `name` ([class.member.lookup]), declared by `owner`.
  // Finding a type or enumerator in the class hides a base-class function with that name too.
  struct Found {
    const FieldRec* field = nullptr;
    u32 index = 0;
    const MemberDeclRec* member = nullptr;
    EntityId owner = EntityId::none;
    bool ambiguous = false;
    EntityId nested_type = EntityId::none;
    EntityId enumerator = EntityId::none;
    EntityId entity() const {
      if (field) return field->entity;
      if (member) return member->entity;
      return enumerator == EntityId::none ? nested_type : enumerator;
    }
    bool needs_object() const { return field || (member && !member->is_static); }
  };
  std::optional<Found> direct_member(EntityId c, const ClassRec& cr, NameId name) {
    for (u32 i = 0; i < cr.fields.size(); ++i)
      if (cr.fields[i].name == name) return Found{&cr.fields[i], i, nullptr, c};
    for (const MemberDeclRec& m : cr.members)
      if (m.name == name && !m.is_ctor && !m.is_dtor) return Found{nullptr, 0, &m, c};
    for (const NestedType& nested : cr.types)
      if (nested.name == name) return Found{.owner = c, .nested_type = nested.entity};
    for (const NestedType& nested : cr.types) {
      // A type alias does not introduce the enumerators of the enumeration it names.
      if (!types().is_enum(nested.type) || types().enum_of(nested.type) != nested.entity) continue;
      const EnumRec* e = enum_rec(nested.entity);
      if (!e || e->scoped) continue;
      for (const EnumeratorRec& value : e->enumerators)
        if (value.name == name) return Found{.owner = c, .nested_type = nested.entity, .enumerator = value.entity};
    }
    return std::nullopt;
  }
  std::optional<Found> find_member(EntityId c, NameId name, bool unqualified = false) {
    const ClassRec* cr = class_rec(c);
    if (!cr) return std::nullopt;
    if (auto member = direct_member(c, *cr, name)) return member;
    std::optional<Found> r;
    for (const BaseRec& b : cr->bases) {
      // A dependent direct base is outside unqualified lookup in the template, including
      // during member analysis with concrete bindings ([temp.dep]).
      if (unqualified && b.dependent_in_pattern) continue;
      auto f = find_member(b.cls, name);
      if (!f) continue;
      if (f->ambiguous) return f;
      if (!r) {
        r = f;
      } else if (r->entity() != f->entity() || r->needs_object()) {
        r->ambiguous = true;
        return r;
      }
    }
    return r;
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
  Ex bound_member(EntityId c, NameId name, std::optional<Ex> object, bool arrow, u32 begin, u32 end, u32 name_at,
                  EntityId naming = EntityId::none) {
    return bind_members(c, member_functions(c, name), object, arrow, begin, end, name_at, naming);
  }

  Ex bind_members(EntityId c, std::vector<const MemberDeclRec*> cands, std::optional<Ex> object, bool arrow,
                  u32 begin, u32 end, u32 name_at, EntityId naming = EntityId::none) {
    ExprInfo info{builtin(Builtin::bound_member), ValueCategory::prvalue, 0};
    Ex r = node(ast::Member{info, cands.front()->entity, ~u32{0}, arrow ? 1u : 0u, 0, object ? at(*object, begin) : Child{}},
                begin, end, info);
    bound_[std::to_underlying(r.id)] = {object, std::move(cands), c, name_at, naming};
    return r;
  }
  bool is_bound_member(const Ex& e) { return e.ok() && types().is(e.type(), Builtin::bound_member); }

  // The implicit member access `name` inside a member function of `c` ([class.mfct.non.static]),
  // or a qualified name `C::name`: none if `c` has no such member.
  std::optional<Ex> class_member(EntityId c, NameId name, u32 begin, u32 end, u32 name_at, bool bare_address = false,
                                 bool unqualified = false) {
    auto f = find_member(c, name, unqualified);
    if (!f) return std::nullopt;
    Ex fail{NodeId::none, begin, end};
    if (f->ambiguous) {
      diag(name_at, Diag::ambiguous_member, name, types().class_type(c));
      return fail;
    }
    if (f->nested_type != EntityId::none) {
      if (f->enumerator == EntityId::none) {
        diag(name_at, Diag::type_as_expression, name);
        return fail;
      }
      if (!check_access(f->owner, f->nested_type, name, name_at, c)) return fail;
      return enumerator(f->enumerator, types().enum_type(f->nested_type), begin, end);
    }
    // `this` names the member when the class is the current class or one of its bases.
    bool has_this = this_type && current_class != EntityId::none &&
                    (current_class == f->owner || base_depth(types().class_type(current_class), types().class_type(f->owner)));
    // The current object as the class that declares the member.
    auto self_of = [&]() {
      Ex self = this_expr(begin, begin);
      return f->owner == current_class ? self : to_base(self, f->owner, name_at);
    };
    EntityId naming = c;
    c = f->owner;
    if (!(f->member && f->member->is_function) && !check_access(c, f->field ? f->field->entity : f->member->entity, name, name_at, naming))
      return fail;
    if (f->field) {
      if (bare_address) {
        diag(name_at, f->field->bit_width ? Diag::bit_field_address : Diag::member_pointer_values);
        return fail;
      }
      if (!has_this) {
        diag(begin, Diag::member_without_object, name);
        return fail;
      }
      if (unevaluated == 0 && !(checking_virtual_body && c == current_class) &&
          !require_supported_virtual_layout(types().class_type(c), name_at)) return fail;
      Ex self = self_of();
      if (!self.ok()) return fail;
      bool ref = types().is_reference(f->field->type);
      // The object's cv-qualifiers: `this`'s, which in a lambda is the captured one (a `*this`
      // copy is const unless the lambda is mutable, [expr.prim.lambda.closure]/12).
      QualType t = ref ? types().referee(f->field->type) : types().add_cv(f->field->type, Types::cv(types().pointee(self.type())));
      ExprInfo info{t, ValueCategory::lvalue, 0};
      return node(ast::Member{info, f->field->entity, f->index, 1, ref ? 1u : 0u, at(self, begin)}, begin, end, info);
    }
    const MemberDeclRec& m = *f->member;
    if (bare_address && !m.is_static &&
        !std::ranges::any_of(member_functions(c, name), [](const MemberDeclRec* f) { return f->is_static; })) {
      diag(name_at, Diag::member_pointer_values);
      return fail;
    }
    // A static member, data or function, is named like a namespace-scope entity
    // ([expr.prim.id.qual]): `S::f` can have its address taken or be called. A member function
    // template is a set to deduce from, as overloads are.
    if (!m.is_function || (m.is_static && !m.is_template && member_functions(c, name).size() == 1)) {
      if (m.is_function && !check_access(c, m.entity, name, name_at, naming)) return fail;
      if (!m.is_function) return static_member(c, m.entity, m.type, begin, end);
      auto mt = deduced_function(m.entity, m.type, begin);
      if (!mt) return fail;
      ExprInfo info{*mt, ValueCategory::lvalue, 0};
      return node(ast::DeclRef{info, m.entity}, begin, end, info);
    }
    std::optional<Ex> object;
    if (has_this) {
      object = self_of();
      if (!object->ok()) return fail;
    }
    Ex set = bound_member(c, name, object, true, begin, end, name_at, naming);
    // Only static members match ordinary function pointers or references ([over.over]). Keep
    // the bound member for calls, whose candidate set also includes the non-static members.
    const u32 key = std::to_underlying(set.id);
    std::vector<ast::DeclSema> statics;
    for (const MemberDeclRec* member : bound_.at(key).cands) {
      if (!member->is_static) continue;
      if (member->is_template) {
        if (auto d = q_->template_decl(member->entity)) statics.push_back(*d);
        continue;
      }
      ast::DeclSema d{};
      d.entity = member->entity;
      d.type = member->type;
      d.kind = ast::DeclKind::function;
      d.flags = member->is_deleted ? decl_flag::deleted : 0;
      d.constraints = ConstId::none;
      statics.push_back(d);
    }
    if (!statics.empty()) overloads_[key] = std::move(statics);
    return set;
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
        if (x.name == name) {
          // A definition may have been analyzed for a later point. Its enumerator is only
          // visible where the constant is available.
          Ex value = enumerator(x.entity, types().enum_type(e), begin, end);
          if (value.ok()) return value;
          break;
        }
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
  Ex member_call(Ex callee, std::vector<Ex> args, u32 end, u32 paren_at, bool assignment_notation = false) {
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
      shapes.push_back(argument_shape(a));
    std::vector<Candidate> cands;
    std::vector<const MemberDeclRec*> recs;
    std::deque<ast::DeclSema> patterns;
    std::vector<std::vector<TemplateArg>> deduced;  // a generic lambda operator's arguments, by candidate
    const bool lvalue_assignment = object && (m.arrow || object->cat() == ValueCategory::lvalue) &&
                                   ins().names.spelling(bound.cands.front()->name) == "operator=";
    for (const MemberDeclRec* c : bound.cands) {
      if (lvalue_assignment && types().rec(c->type).fn_ref == ast::RefQual::rvalue) continue;
      if (!c->head.empty()) {
        // A generic lambda's function call operator, a template whose definition is its lambda's:
        // the specialization deduction gives ([temp.over]/1), instantiated by `generic_call`.
        ast::DeclSema& td = patterns.emplace_back();
        td.entity = c->entity;
        td.type = c->type;
        td.kind = ast::DeclKind::function;
        td.constraints = ConstId::none;
        td.template_params = ins().intern_seq(std::span<const ast::TemplateParamSema>(c->head));
        Templates t = templates();
        auto targs = t.deduce_call(td, shapes, explicit_args);
        if (!targs) continue;
        auto st = t.specialization_type(td, *targs);
        if (!st) continue;
        cands.push_back({c->entity, *st, c->is_static, c->is_deleted});
        recs.push_back(c);
        deduced.push_back(std::move(*targs));
        continue;
      }
      if (!c->is_template) {
        if (ex != explicit_args_.end()) continue;
        cands.push_back({c->entity, c->type, c->is_static, c->is_deleted, nullptr, c->defaults});
        recs.push_back(c);
        deduced.emplace_back();
        continue;
      }
      auto tmpl = q_->template_decl(c->entity);
      if (!tmpl) continue;
      const ast::DeclSema& td = patterns.emplace_back(*tmpl);
      Templates t = templates();
      auto targs = t.deduce_call(td, shapes, explicit_args, c->defaults);
      if (!targs) continue;
      if (td.constraints != ConstId::none) {
        std::vector<ast::TemplateParamSema> ps = ins().seqs.elements(td.template_params);
        auto nf = t.substitute_constraint(td.constraints, Bindings::of(ps.empty() ? 0 : ps[0].depth, *targs));
        auto sat = nf ? q_->satisfied(*nf) : std::optional<bool>(false);
        if (sat && !*sat) continue;
      }
      auto st = t.specialization_type(td, *targs);
      if (!st) continue;
      cands.push_back({specialization(td, *targs, *st), *st, c->is_static, c->is_deleted, &td, c->defaults});
      recs.push_back(c);
      deduced.emplace_back();
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
    // A sole trivial implicit copy can bind to a temporary made by a converting constructor.
    // Prepare that conversion before ranking the reference binding. With competing assignment
    // candidates, their user-defined conversions need to be ranked together.
    if (assignment_notation && lvalue_assignment && args.size() == 1 && recs.size() == 1 &&
        recs.front()->copy_assignment == CopyAssignment::trivial) {
      const QualType parameter = types().rec(cands.front().type).params.front();
      const QualType target = types().referee(parameter);
      Ex& source = args.front();
      if (object_cv == Cv::none && !same_unqualified(source.type(), target) && !ics(source, parameter).viable) {
        const ClassRec* cls = class_rec(bound.cls);
        if (cls && cls->user_ctors && this->object_cv(target) == Cv::const_) {
          source = convert(source, Types::unqualified(target), source.begin);
          if (!source.ok()) return fail;
        }
      }
    }
    auto chosen = resolve(cands, args, begin, name, object_cv);
    if (!chosen) return fail;
    const MemberDeclRec* d = recs[*chosen];
    const Candidate& k = cands[*chosen];
    if (!check_access(bound.cls, d->entity, d->name, bound.name_at, bound.naming)) return fail;
    if (unevaluated == 0) {
      if (d->is_virtual) {
        diag(paren_at, Diag::virtual_call);
        return fail;
      }
      if (!d->is_static && !require_supported_virtual_layout(types().class_type(bound.cls), paren_at)) return fail;
    }
    const auto copy_assignment = d->copy_assignment;
    if (copy_assignment != CopyAssignment::none) {
      QualType ct = types().class_type(bound.cls);
      if (!usable_copy_assignment(copy_assignment, ct, paren_at)) return fail;
      if (copy_assignment == CopyAssignment::memberwise && !d->explicit_defaulted_assignment &&
          (!lvalue_assignment || args[0].cat() != ValueCategory::lvalue || !same_unqualified(args[0].type(), ct))) {
        diag(paren_at, Diag::unsupported_copy_assignment, ct);
        return fail;
      }
      if ((copy_assignment != CopyAssignment::trivial || !assignment_notation) &&
          !usable_copy_assignment(q_->require_copy_assignment(d->entity), ct, paren_at)) return fail;
    }
    EntityId fe = k.entity;
    std::optional<QualType> dt;
    if (!d->head.empty()) {
      auto g = generic_call(*d, deduced[*chosen], begin);
      if (!g) return fail;
      fe = g->spec;
      dt = g->type;
    } else {
      // A specialization called is one whose definition the program requires ([temp.inst]/8).
      if (k.pattern) demands.push_back({k.entity, ast::Loc{item_, loc_base_ + begin}});
      dt = deduced_function(k.entity, k.type, begin);
    }
    if (!dt) return fail;
    TypeRec fr = types().rec(*dt);
    auto children = call_args(fr, args, begin, paren_at, cands[*chosen].defaults);
    if (!children) return fail;
    ExprInfo info = result_info(fr.inner);
    if (d->is_static) {
      // The object expression is evaluated, then discarded ([expr.ref]).
      Ex call = node(ast::Call{info, fe, at(callee, begin), b_->list(*children)}, begin, end, info);
      if (!object) return call;
      Ex discarded = convert_node(*object, ast::ConversionKind::to_void, builtin(Builtin::void_));
      return node(ast::OrderedBinary{info, ast::OrderedOp::comma, at(discarded, begin), at(call, begin)}, begin, end, info);
    }
    if (!object) {
      diag(begin, Diag::member_without_object, d->name);
      return fail;
    }
    Ex o = *object;
    if (assignment_notation && copy_assignment == CopyAssignment::trivial) {
      // The chosen reference argument already designates the source object. Assignment does not
      // initialize a new class value, even when the copy constructor is deleted.
      return node(ast::Assign{info, ast::AssignOp::trivial_class_copy, at(o, begin), children->front(), info.type},
                  begin, end, info);
    }
    if (!m.arrow) {
      // A prvalue object is materialized: the call's object is a temporary ([class.temporary]/2).
      if (!o.glvalue()) o = materialize(o, o.type(), false);
      ExprInfo pi{types().pointer(o.type()), ValueCategory::prvalue, 0};
      o = node(ast::Unary{pi, ast::UnaryOp::address_of, at(o, o.begin)}, o.begin, o.end, pi);
    }
    if (assignment_notation) {
      contract_assert(children->size() == 1);
      return node(ast::AssignmentCall{info, fe, children->front(), at(o, begin)}, begin, end, info);
    }
    return node(ast::MemberCall{info, fe, at(o, begin), b_->list(*children)}, begin, end, info);
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
    // Its parameters' default arguments, by position, none where a parameter has none
    // ([dcl.fct.default]); empty if it has none.
    std::vector<NodeId> defaults{};
    // How many arguments a call must give ([over.match.viable]/2.3).
    std::size_t required(std::size_t params) const {
      std::size_t k = params;
      while (k > 0 && k - 1 < defaults.size() && defaults[k - 1] != NodeId::none) --k;
      return k;
    }
  };

  // The default arguments of a function's declaration `d`, by parameter, or empty if it has none.
  // `d` was found by lookup: its lists are interned.
  std::vector<NodeId> defaults_of(const ast::DeclSema& d) {
    std::vector<NodeId> r;
    for (const ast::ParamSema& p : ins().seqs.elements(d.params)) r.push_back(p.default_arg);
    if (std::ranges::all_of(r, [](NodeId n) { return n == NodeId::none; })) r.clear();
    return r;
  }
  // Those of a declaration lookup found: the latest one found has every earlier one's in its scope
  // ([dcl.fct.default]/4); declarations in other scopes have their own. This item's own
  // declarations are drafts and have none yet.
  std::vector<NodeId> found_defaults(const ast::DeclSema& d) {
    if (std::ranges::any_of(own, [&](const OwnDecl& w) { return w.sema.entity == d.entity && w.sema.params == d.params; })) return {};
    return defaults_of(d);
  }
  // Those of a function named by `callee` (a DeclRef, perhaps parenthesized): as its lookup found
  // them, or, for a member, as its class records them.
  std::vector<NodeId> defaults_of(Ex callee, EntityId f) {
    NodeId n = callee.id;
    while (n != NodeId::none && ast::kind_of(n) == ast::Nodes::index_of<ast::Paren>()) n = b_->get<ast::Paren>(n).operand.id;
    if (auto it = defaults_.find(std::to_underlying(n)); it != defaults_.end()) return it->second;
    EntityId scope = w_->entities.rec(f).scope;
    if (scope != EntityId::none && w_->entities.rec(scope).kind != EntityKind::namespace_)
      if (const ClassRec* cr = class_rec(scope))
        for (const MemberDeclRec& m : cr->members)
          if (m.entity == f) return m.defaults;
    return {};
  }

  // An implicit conversion sequence's ranking ([over.ics.rank]): exact match, promotion,
  // conversion, then a tie-break among exact matches (an added qualification, or a reference
  // to a more cv-qualified type, is worse); `viable` false when there is none.
  struct Ics {
    static constexpr int user_defined = 3;
    static constexpr int ellipsis = 4;
    bool viable = false;
    int rank = 0;
    int sub = 0;              // a qualification conversion among exact matches ([over.ics.rank]/3.2.5)
    bool ref = false;         // a reference binding: the tie-breaks of /3.2.3 and /3.2.6 apply
    bool rvalue_ref = false;  // binding an rvalue reference
    bool to_rvalue = false;   // to an rvalue
    int ref_cv = 0;           // the cv-qualifiers the referred-to type adds
    int base_depth = 0;       // a derived-to-base conversion's distance ([over.ics.rank]/4.4)
    bool function_ref = false;  // binding to a function lvalue ([over.ics.rank]/3.2.4)
    bool operator<(const Ics& o) const {
      if (rank != o.rank) return rank < o.rank;
      if (base_depth && o.base_depth && base_depth != o.base_depth) return base_depth < o.base_depth;
      if (ref && o.ref) {
        if (to_rvalue && o.to_rvalue && rvalue_ref != o.rvalue_ref) return rvalue_ref;  // /3.2.3
        if (function_ref && o.function_ref && rvalue_ref != o.rvalue_ref) return !rvalue_ref;  // /3.2.4
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

  // [dcl.init.ref]/5: a nonvolatile const lvalue reference can also bind directly to an object xvalue.
  bool direct_reference_category(const Ex& e, const TypeRec& ref) {
    if (ref.rvalue_ref && !types().is_function(ref.inner)) return e.cat() == ValueCategory::xvalue;
    return e.cat() == ValueCategory::lvalue ||
           (!ref.rvalue_ref && object_cv(ref.inner) == Cv::const_ && e.cat() == ValueCategory::xvalue);
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
    // The target parameter can select a function from an overload set ([over.over]).
    if (auto set = address_set(a)) {
      auto f = address_target(p);
      if (!f) return {};
      auto matches = address_matches(*set, *f);
      if (matches.size() != 1) return {};
      QualType t = matches.front().type;
      ExprInfo info = set->address ? ExprInfo{types().pointer(t), ValueCategory::prvalue, 0}
                                   : ExprInfo{t, ValueCategory::lvalue, 0};
      return ics({a.id, a.begin, a.end, info}, p);
    }
    if (types().is_reference(p)) {
      TypeRec r = types().rec(p);
      QualType t = r.inner;
      const bool direct_cat = direct_reference_category(a, r);
      if (direct_cat && same_unqualified(a.type(), t)) {
        if (!cv_within(object_cv(a.type()), object_cv(t))) return {};
        Ics v = binding({true, 0, 0}, r.rvalue_ref, a.cat() != ValueCategory::lvalue,
                        cv_bits(object_cv(t)) - cv_bits(object_cv(a.type())));
        v.function_ref = types().is_function(t);
        return v;
      }
      // [over.ics.ref]/1: binding to a base class subobject is a derived-to-base Conversion.
      if (int d = base_depth(a.type(), t); d && direct_cat) {
        if (!cv_within(object_cv(a.type()), object_cv(t))) return {};
        Ics v = binding({true, 2, 0}, r.rvalue_ref, a.cat() != ValueCategory::lvalue, cv_bits(object_cv(t)) - cv_bits(object_cv(a.type())));
        v.base_depth = d;
        return v;
      }
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
      // [over.best.ics]/6: a base class object copied from a derived class's, a Conversion.
      if (int d = base_depth(a.type(), to)) {
        Ics v{true, 2, 0};
        v.base_depth = d;
        return v;
      }
      return {};
    }
    if (types().is_pointer(to) && is_null_pointer_constant(a)) return {true, 2, 0};
    // A user-defined conversion: a closure's to a pointer to function ([over.best.ics]/2).
    if (closure_invoker(a.type(), to)) return {true, Ics::user_defined, 0};
    QualType from = value_type(a);
    if (types().is_class(from) || types().is_void(from)) return {};
    if (types().canonical(from) == types().canonical(to)) return {true, 0, 0};
    if (types().is_enum(to)) return {};
    if (types().is_enum(from)) {
      if (!is_unscoped_enum(from)) return {};
      if (types().is(to, Builtin::bool_) || types().is_floating(to)) return {true, 2, 0};  // [conv.fpint]
      if (!types().is_integral(to)) return {};
      // [conv.prom]/4, [over.ics.rank]/4.2: with a fixed type, to it is the better promotion.
      if (const EnumRec* er = enum_rec(types().enum_of(from)); er && er->fixed) {
        if (types().canonical(*er->underlying) == types().canonical(to)) return {true, 1, 0};
        if (builtin(sema::promoted(w_->target, *types().builtin_of(*er->underlying))) == to) return {true, 1, 1};
        return {true, 2, 0};
      }
      // [conv.prom]/3: without one, to the type its values need.
      const EnumRec* er = enum_rec(types().enum_of(from));
      QualType p = er && er->promotion ? *er->promotion : builtin(sema::promoted(w_->target, *types().builtin_of(underlying_of(from))));
      bool promotion = types().canonical(p) == types().canonical(to);
      return {true, promotion ? 1 : 2, 0};
    }
    if (types().is_pointer(from) && types().is_pointer(to))
    {
      if (qualification_convertible(from, to)) return {true, 0, 1};
      if (int d = base_depth(types().pointee(from), types().pointee(to));
          d && cv_within(Types::cv(types().pointee(from)), Types::cv(types().pointee(to)))) {
        Ics v{true, 2, 0};
        v.base_depth = d;
        return v;
      }
      return pointer_convertible(from, to) ? Ics{true, 2, 0} : Ics{};
    }
    if (types().is(to, Builtin::bool_) && (types().is_arithmetic(from) || types().is_pointer(from))) return {true, 2, 0};
    // [conv.fpprom]: float to double is a promotion; the other floating-point and
    // floating-integral conversions are conversions ([over.ics.scs]).
    if (types().is_arithmetic(from) && types().is_arithmetic(to) && (types().is_floating(from) || types().is_floating(to)))
      return {true, types().is(from, Builtin::float_) && types().is(to, Builtin::double_) ? 1 : 2, 0};
    if (types().is_integral(from) && types().is_integral(to)) {
      bool promotion = builtin(sema::promoted(w_->target, *types().builtin_of(from))) == to && !types().is(from, *types().builtin_of(to));
      return {true, promotion ? 1 : 2, 0};
    }
    return {};
  }

  // [over.ics.list]: rank a braced argument before initializing the selected parameter.
  // A disengaged result is a list conversion this slice does not classify.
  std::optional<Ics> list_ics(const Braced& list, QualType parameter) {
    const bool flat = std::ranges::none_of(list.clauses, [](const Braced::Clause& c) { return c.list || c.designated; });
    if (types().is_scalar(parameter)) {
      if (!flat || list.clauses.size() > 1) return Ics{};
      if (!types().is_arithmetic(parameter)) return std::nullopt;
      if (list.clauses.empty()) return Ics{true};
      const Ex& expr = list.clauses.front().expr;
      if (expr.ok() && types().is_class(expr.type())) return std::nullopt;
      return ics(expr, parameter);
    }
    if (!flat || !types().is_class(parameter)) return std::nullopt;
    EntityId entity = types().class_of(parameter);
    if (w_->entities.rec(entity).templ != EntityId::none) return std::nullopt;
    const ClassRec* record = class_rec(entity);
    if (!record || record->union_ || record->closure || record->user_ctors || !record->bases.empty() ||
        !std::ranges::all_of(record->fields, [&](const FieldRec& f) {
          return types().is_arithmetic(f.type) && record->access_of(f.entity) == ast::Access::public_;
        }))
      return std::nullopt;
    // A single class-valued clause can instead use the same-class or base conversion.
    for (const Braced::Clause& c : list.clauses) {
      if (!c.expr.ok()) return Ics{};
      if (!types().is_arithmetic(c.expr.type())) return std::nullopt;
    }
    if (list.clauses.size() > record->fields.size()) return Ics{};
    for (std::size_t i = 0; i < list.clauses.size(); ++i)
      if (!ics(list.clauses[i].expr, record->fields[i].type).viable) return Ics{};
    // The field conversions establish viability. Their ranks and narrowing do not rank the list.
    return Ics{true, Ics::user_defined};
  }

  // The best viable candidate for `args` ([over.match.best]), diagnosed at `at` when there is
  // none or more than one, or when it is deleted. `object_cv` is the implicit object's, for
  // non-static member functions.
  std::optional<std::size_t> resolve(std::span<const Candidate> cands, std::span<const Ex> args, u32 at, NameId name,
                                     std::optional<Cv> object_cv, std::optional<QualType> ctor_of = std::nullopt,
                                     std::span<const Braced* const> lists = {}) {
    std::vector<std::pair<std::size_t, std::vector<Ics>>> viable;
    const Braced* first_list = lists.empty() ? nullptr : *std::ranges::find_if(lists, [](const Braced* list) { return list; });
    const Braced* unsupported = nullptr;
    for (std::size_t i = 0; i < cands.size(); ++i) {
      TypeRec fr = types().rec(cands[i].type);
      const bool template_list = first_list && cands[i].pattern;
      const bool pack = template_list && !fr.params.empty() && types().kind(fr.params.back()) == TypeKind::pack_expansion;
      const std::size_t fixed = fr.params.size() - (pack ? 1 : 0);
      if (args.size() < cands[i].required(fixed) || (args.size() > fixed && !fr.variadic && !pack)) continue;
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
        Ics c{true};
        if (k >= fixed) {
          c.rank = Ics::ellipsis;
        } else if ((!first_list || !lists[k]) && (!template_list || !types().is_dependent(fr.params[k]))) {
          c = ics(args[k], fr.params[k]);
        }
        ok = c.viable;
        seq.push_back(c);
      }
      if (!ok) continue;
      // A candidate excluded by any argument cannot affect selection, even when another
      // argument's list conversion is outside the supported subset.
      const Braced* unknown = template_list ? first_list : nullptr;
      if (first_list && !template_list) {
        for (std::size_t k = 0; k < args.size() && k < fixed && ok; ++k) {
          if (!lists[k]) continue;
          auto c = list_ics(*lists[k], fr.params[k]);
          if (c) {
            ok = c->viable;
            seq[k + 1] = *c;
          } else if (!unknown) {
            unknown = lists[k];
          }
        }
      }
      if (!ok) continue;
      if (unknown) {
        if (!unsupported || unknown->begin < unsupported->begin) unsupported = unknown;
      } else {
        viable.push_back({i, std::move(seq)});
      }
    }
    if (unsupported) {
      diag(unsupported->begin, Diag::not_supported, lex::Tk::l_brace);
      return std::nullopt;
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

  // A call returning a reference to a function is an lvalue. For object references the category
  // follows the reference kind ([expr.call]).
  ExprInfo result_info(QualType r) {
    if (!types().is_reference(r)) return {Types::unqualified(r), ValueCategory::prvalue, 0};
    QualType t = types().referee(r);
    return {t, types().rec(r).rvalue_ref && !types().is_function(t) ? ValueCategory::xvalue : ValueCategory::lvalue, 0};
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
    if (!require_complete(t, begin) || !require_supported_virtual_layout(t, begin)) return fail;
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
      shapes.push_back(argument_shape(a));
    for (const MemberDeclRec& m : cr->members)
      if (m.is_ctor && m.is_template) {
        // A constructor template: the specialization deduction gives ([temp.over]/1), never a
        // copy constructor ([class.copy.ctor]/5).
        auto tmpl = q_->template_decl(m.entity);
        if (!tmpl) continue;
        const ast::DeclSema& td = patterns.emplace_back(*tmpl);
        Templates t = templates();
        auto targs = t.deduce_call(td, shapes, {}, m.defaults);
        if (!targs) continue;
        auto st = t.specialization_type(td, *targs);
        if (!st) continue;
        Candidate k{EntityId::none, *st, true, m.is_deleted, &td, m.defaults};
        if (copy_init && (types().rec(*st).params.empty() || k.required(types().rec(*st).params.size()) > 1)) continue;
        // [class.copy.ctor]/5: never `S(S)`, a constructor taking its own class by value.
        if (TypeRec sr = types().rec(*st); sr.params.size() == 1 && Types::unqualified(types().canonical(sr.params[0])) == ut) continue;
        k.entity = specialization(td, *targs, *st);
        cands.push_back(std::move(k));
      } else if (m.is_ctor) {
        TypeRec fr = types().rec(m.type);
        Candidate k{m.entity, m.type, true, m.is_deleted, nullptr, m.defaults};
        // Copy-initialization converts with a constructor that takes one argument ([class.conv.ctor]).
        if (copy_init && (fr.params.empty() || k.required(fr.params.size()) > 1)) continue;
        user_copy = user_copy || is_copy_ctor(fr, ut, k.required(fr.params.size()));
        cands.push_back(std::move(k));
      }
    QualType void_ = builtin(Builtin::void_);
    if (!user_copy) {
      QualType source = cr->defaulted_copy_const ? types().add_cv(ut, Cv::const_) : ut;
      QualType p = types().lvalue_ref(source);
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
    if (cr->defaulted_copy != CopyState::ordinary) {
      const bool defaulted = k.entity == EntityId::none ? !fr.params.empty() :
          std::ranges::contains(cr->defaulted_copy_declarations, k.entity);
      if (defaulted) {
        if (cr->defaulted_copy != CopyState::trivial) {
          diag(begin, Diag::defaulted_copy_unsupported, ut);
          return fail;
        }
        Ex src = bind_reference(args[0], fr.params[0], args[0].begin);
        return src.ok() ? convert_node(src, ast::ConversionKind::lvalue_to_rvalue, ut) : fail;
      }
    }
    if (k.entity == EntityId::none) {
      // Trivial: a copy of the argument's object, or nothing to construct.
      if (fr.params.empty()) return value_init_aggregate(ut, begin, end);
      Ex a = args[0];
      return a.glvalue() ? convert_node(a, ast::ConversionKind::lvalue_to_rvalue, ut) : a;
    }
    for (std::size_t i = args.size(); i < fr.params.size() && i < k.defaults.size(); ++i) {
      auto a = default_argument(k.defaults[i], end);
      if (!a) return fail;
      args.push_back(*a);
    }
    std::vector<Child> cs;
    for (std::size_t i = 0; i < args.size(); ++i) {
      Ex a = i >= fr.params.size() ? variadic_argument(args[i])
           : list                  ? list_element(fr.params[i], args[i])
                                   : convert(args[i], fr.params[i], args[i].begin);
      if (!a.ok()) return fail;
      // A parameter of class type: a temporary the caller initializes and destroys ([expr.call]/7).
      if (i < fr.params.size() && types().is_class(fr.params[i]) && !a.glvalue())
        a = materialize(a, Types::unqualified(fr.params[i]), false, true);
      cs.push_back({a.id, a.begin >= begin ? a.begin - begin : 0});
    }
    ExprInfo info{ut, ValueCategory::prvalue, 0};
    return node(ast::CtorCall{info, k.entity, 0, b_->list(cs)}, begin, end, info);
  }
  // [class.copy.ctor]/2: a constructor whose first parameter is `C&` or `cv C&` and whose others
  // have default arguments (`required` is how many need arguments).
  bool is_copy_ctor(const TypeRec& fr, QualType c, std::size_t required) {
    return !fr.params.empty() && required <= 1 && types().is_reference(fr.params[0]) && !types().rec(fr.params[0]).rvalue_ref &&
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
    if (!require_supported_virtual_layout(t, v.begin)) return {NodeId::none, v.begin, v.end};
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
    e = overload_address(e, r, at_);
    if (!e.ok()) return e;
    if (is_bound_member(e) || types().is(e.type(), Builtin::overload_set)) return rvalue(e);
    TypeRec rr = types().rec(r);
    QualType t = rr.inner;
    Ex fail{NodeId::none, e.begin, e.end};
    if (e.glvalue() && e.info.bit_width) {
      diag(at_, Diag::bit_field_reference);
      return fail;
    }
    const bool direct_cat = direct_reference_category(e, rr);
    if (direct_cat && same_unqualified(e.type(), t)) {
      if (cv_within(object_cv(e.type()), object_cv(t))) return extend ? extended(e) : e;
      diag(at_, Diag::bind_drops_qualifiers, e.type(), r);
      return fail;
    }
    // [dcl.init.ref]/5.1, 5.3: to the base class subobject of a derived class's object, a prvalue
    // materialized first (and its lifetime extended with the reference's, [class.temporary]/6).
    if (types().is_class(e.type()) && types().is_class(t) && !same_unqualified(e.type(), t) &&
        (direct_cat || rr.rvalue_ref || object_cv(t) == Cv::const_) &&
        !(rr.rvalue_ref && e.cat() == ValueCategory::lvalue)) {
      int paths = 0;
      if (is_complete(e.type()) && base_path(types().class_of(e.type()), types().class_of(t), paths)) {
        if (!cv_within(object_cv(e.type()), object_cv(t))) {
          diag(at_, Diag::bind_drops_qualifiers, e.type(), r);
          return fail;
        }
        if (!e.glvalue()) return to_base(materialize(e, Types::unqualified(e.type()), extend), types().class_of(t), at_);
        Ex b = to_base(e, types().class_of(t), at_);
        return b.ok() && extend ? extended(b) : b;
      }
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
    if (!e.glvalue() && types().is_array(t) && same_unqualified(e.type(), t) &&
        cv_within(object_cv(e.type()), object_cv(t)))
      return materialize(e, t, extend);  // [dcl.init.ref]/5.3: a reference-compatible array prvalue
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
    // Through a base subobject or an identity-preserving glvalue cast ([class.temporary]/6).
    if (auto c = as<ast::ImplicitConversion>(e); c &&
        (c->conversion == ast::ConversionKind::derived_to_base || c->conversion == ast::ConversionKind::base_to_derived ||
         c->conversion == ast::ConversionKind::qualification)) {
      Ex inner = extended({c->operand.id, e.begin + c->operand.at, e.end, {}});
      return node(ast::ImplicitConversion{c->info, c->conversion, c->function, at(inner, e.begin)}, e.begin, e.end, c->info);
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
          u32 ctx = context_of(d);
          if (!(p->second & capture_bit)) {
            const ast::LocalRec& local = ctx == lambdas_.size() ? locals_[p->second] : lambdas_[ctx].outer_locals[p->second];
            if (!unevaluated && (local.flags & local_flag::default_argument_parameter)) {
              diag(begin, Diag::parameter_in_default_argument, name);
              return {NodeId::none, begin, end};
            }
          }
          // A local of an enclosing function, or an init-capture: through the lambdas' captures.
          if (ctx != lambdas_.size() || (p->second & capture_bit)) return captured(ctx, p->second, name, begin, end);
          const ast::LocalRec& l = locals_[p->second];
          // A variable whose type its own initializer deduces ([dcl.spec.auto.general]/11).
          if (types().has_placeholder(l.type)) {
            diag(begin, Diag::auto_used_in_initializer, name);
            return {NodeId::none, begin, end};
          }
          // A function parameter pack of the definition: its uses are expanded ([temp.variadic]).
          if (types().kind(l.type) == TypeKind::pack_expansion) {
            if (pattern_packs_.empty()) {
              diag(begin, Diag::unexpanded_pack, name);  // [temp.variadic]/5: outside every expansion
              return {NodeId::none, begin, end};
            }
            note_pack({0, 0, name});
            return dependent_node({}, begin, end);
          }
          return local_ref(p->second, begin, end);
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
    u32 pack_ctx = 0;
    if (auto l = pack_element(name, is_pack, &pack_ctx)) {
      // An element of an enclosing function's pack, in a lambda: captured as any local is.
      if (pack_ctx != lambdas_.size()) return captured(pack_ctx, *l, name, begin, end);
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
      if (auto r = class_member(current_class, name, begin, end, begin, false, true)) return *r;
      if (!class_rec(current_class)) {
        if (auto m = q_->earlier_member(name)) {
          if (m->kind == ast::DeclKind::enumerator) return enumerator(m->entity, m->type, begin, end);
          // A static member function needs no object ([class.static.mfct]).
          if (m->kind == ast::DeclKind::function && (m->flags & decl_flag::static_))
            return decls_expr({*m}, true, name, begin, end, callee);
          if (m->kind != ast::DeclKind::variable) {
            diag(begin, Diag::member_without_object, name);
            return {NodeId::none, begin, end};
          }
          return static_member(current_class, m->entity, m->type, begin, end);
        }
        // A nondependent base participates in lookup while a class template is being defined.
        if (w_->entities.rec(current_class).templ != EntityId::none)
          for (EntityId base : q_->defining_bases(current_class))
            if (auto m = class_member(base, name, begin, end, begin)) return *m;
      }
    }
    if (namespace_lambda && namespace_lambda->name == name)
      return undeduced_namespace_lambda(name, begin, end);
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
  bool unsupported_builtin(NameId name) { return lex::lacking_builtin(ins().names.spelling(name)) && !declared(name); }
  // Whether a declaration in scope gives `name`, hiding the built-in function of that name.
  bool declared(NameId name) {
    for (const auto& scope : scopes_)
      if (std::ranges::any_of(scope, [&](const auto& p) { return p.first == name; })) return true;
    if (std::ranges::any_of(own, [&](const auto& p) { return p.name == name; })) return true;
    if (std::ranges::any_of(block_usings_, [&](const BlockUsing& u) { return u.name == name; })) return true;
    if (names_member(name)) return true;
    for (DeclId d : q_->visible_decls(name, directives())) {
      (void)d;
      return true;
    }
    return false;
  }

  // A built-in function ycxx implements (`lex::builtin_functions`) that no declaration hides.
  const lex::BuiltinFunctionInfo* builtin_function(NameId name) {
    const lex::BuiltinFunctionInfo* b = lex::builtin_function(ins().names.spelling(name));
    return b && !declared(name) ? b : nullptr;
  }
  // A call of one (docs/EXTENSIONS.md): `__builtin_inf*` and `__builtin_huge_val*` are +infinity
  // and `__builtin_nan*` a quiet NaN, constants of their type; `__builtin_fabs*` a `BuiltinCall`.
  Ex builtin_call(const lex::BuiltinFunctionInfo& b, std::vector<Ex> args, u32 begin, u32 end) {
    using enum lex::BuiltinFunction;
    Builtin kind = b.type == 'f' ? Builtin::float_ : b.type == 'l' ? Builtin::long_double : Builtin::double_;
    if (!ast::computes(w_->target[kind].format)) {
      diag(begin, Diag::extended_float_not_supported);
      return {NodeId::none, begin, end};
    }
    QualType t = builtin(kind);
    std::size_t want = b.function == fabs || b.function == nan ? 1 : 0;
    if (args.size() != want) {
      diag(begin, args.size() < want ? Diag::too_few_arguments : Diag::too_many_arguments, u64(want), u64(args.size()));
      return {NodeId::none, begin, end};
    }
    if (std::ranges::any_of(args, [](const Ex& a) { return !a.ok(); })) return {NodeId::none, begin, end};
    switch (b.function) {
      case inf:
      case huge_val:
        return literal(t, w_->values.floating(t, std::numeric_limits<double>::infinity()), ValueCategory::prvalue, begin, end);
      case nan: {
        // The payload is a string literal's contents read as `strtoull` reads them: decimal, octal
        // after `0`, hexadecimal after `0x`; empty is 0. It fills the significand below the quiet bit.
        Ex a = args[0];
        while (auto p = as<ast::Paren>(a)) a = {p->operand.id, a.begin + p->operand.at, a.end};
        auto lit = as<ast::Literal>(a);
        std::optional<u64> payload;
        if (auto text = lit ? w_->values.narrow_string(lit->value) : std::nullopt) {
          std::string_view s = *text;
          if (s.ends_with('\0')) s.remove_suffix(1);
          int base = s.starts_with("0x") || s.starts_with("0X") ? 16 : s.starts_with('0') ? 8 : 10;
          if (base == 16) s.remove_prefix(2);
          u64 v = 0;
          auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
          if (s.empty() || (r.ec == std::errc{} && r.ptr == s.data() + s.size())) payload = v;
        }
        if (!payload) {
          diag(args[0].begin, Diag::builtin_nan_string);
          return {NodeId::none, begin, end};
        }
        bool single = ast::float_format(ins(), w_->target, t) == ast::FloatFormat::binary32;
        u64 bits = single ? 0x7fc00000u | (*payload & 0x3fffffu) : 0x7ff8000000000000u | (*payload & 0x7ffffffffffffu);
        return literal(t, ast::float_value(ins(), w_->target, t, ast::Bits128{bits, 0}), ValueCategory::prvalue, begin, end);
      }
      case fabs: {
        if (dependent(args[0])) return dependent_node({args[0]}, begin, end, t);
        Ex x = convert(args[0], t, args[0].begin);
        if (!x.ok()) return {NodeId::none, begin, end};
        ExprInfo info{t, ValueCategory::prvalue, 0};
        std::vector<ast::Child> children{at(x, begin)};
        return node(ast::BuiltinCall{info, u8(fabs), b_->list(children)}, begin, end, info);
      }
    }
    std::unreachable();
  }

  std::optional<lex::VaBuiltin> va_builtin(NameId name) {
    if (w_->target.va_list != ast::VaList::sysv_x86_64) return std::nullopt;
    auto b = lex::va_builtin(ins().names.spelling(name));
    return b && !declared(name) ? b : std::nullopt;
  }

  Ex va_call(lex::VaBuiltin builtin_op, Ex cursor, std::optional<Ex> source,
             std::optional<QualType> fetch, u32 begin, u32 end) {
    const bool starts = builtin_op == lex::VaBuiltin::start || builtin_op == lex::VaBuiltin::legacy_start;
    const Ex invalid{NodeId::none, begin, end};
    if (starts) {
      const bool variadic = lambdas_.empty() ? variadic_function_body : types().rec(lambdas_.back().call_type).variadic;
      if (!variadic) {
        diag(begin, Diag::va_start_context);
        return invalid;
      }
    }
    if (!cursor.ok() || (source && !source->ok())) return invalid;
    QualType result = builtin(Builtin::void_);
    if (fetch) {
      result = Types::unqualified(types().canonical(*fetch));
      if (!types().is_dependent(result) && !types().is_pointer(result)) {
        auto b = types().builtin_of(result);
        if (b != Builtin::int_ && b != Builtin::unsigned_int && b != Builtin::long_ &&
            b != Builtin::unsigned_long && b != Builtin::long_long && b != Builtin::unsigned_long_long) {
          diag(begin, Diag::va_arg_type, *fetch);
          return invalid;
        }
      }
    }

    // A local va_list decays to a pointer to its single record; an adjusted parameter is
    // already a pointer. The pointee's identity and cv-qualifiers must match in either case.
    const QualType array = w_->builtin_va_list();
    const QualType pointer = types().pointer(types().rec(array).inner);
    auto normalize = [&](Ex e) {
      if (dependent(e)) return e;
      Ex p = rvalue(e);
      if (p.ok() && types().canonical(p.type()) != pointer) {
        diag(e.begin, Diag::va_list_operand, e.type());
        return Ex{NodeId::none, e.begin, e.end};
      }
      return p;
    };
    cursor = normalize(cursor);
    if (source) source = normalize(*source);
    if (!cursor.ok() || (source && !source->ok())) return invalid;

    std::vector<Ex> args{cursor};
    if (source) args.push_back(*source);
    if (types().is_dependent(result) || std::ranges::any_of(args, [&](Ex e) { return dependent(e); }))
      return dependent_node(args, begin, end, result);
    std::vector<ast::Child> children;
    for (Ex e : args) children.push_back(at(e, begin));
    ast::VaOp op = starts ? ast::VaOp::start :
                   fetch ? ast::VaOp::arg : source ? ast::VaOp::copy : ast::VaOp::end;
    ExprInfo info{result, ValueCategory::prvalue, 0};
    return node(ast::VariadicBuiltin{info, op, b_->list(children)}, begin, end, info);
  }

  // The expression naming what lookup found for `name` ([expr.prim.id]): one entity, or an
  // overload set.
  // `unqualified`: an unqualified-id looked up at namespace scope or introduced by a block
  // using-declaration. Calls may add ADL when every result is a namespace function.
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
    bool adl = unqualified && std::ranges::all_of(set, [&](const ast::DeclSema& x) {
      if (x.kind != ast::DeclKind::function) return false;
      EntityId scope = w_->entities.rec(x.entity).scope;
      return scope == EntityId::none || w_->entities.rec(scope).kind == EntityKind::namespace_;
    });
    bool templ = std::ranges::any_of(set, [](const ast::DeclSema& x) { return (x.flags & decl_flag::template_) != 0; });
    if (set.size() > 1 || templ) {
      if (!std::ranges::all_of(set, [](const ast::DeclSema& x) { return x.kind == ast::DeclKind::function; })) {
        diag(begin, Diag::ambiguous_name, name);  // [basic.lookup.general]/1: two entities
        return {NodeId::none, begin, end};
      }
      ExprInfo info{builtin(Builtin::overload_set), ValueCategory::lvalue, 0};
      Ex r = node(ast::DeclRef{info, d.entity}, begin, end, info);
      if (adl) unqualified_[std::to_underlying(r.id)] = set;
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
    QualType dt = d.type;
    if (d.kind == ast::DeclKind::function && !(adl && callee)) {
      auto t = deduced_function(d.entity, d.type, begin);
      if (!t) return {NodeId::none, begin, end};
      dt = *t;
    }
    ExprInfo info{types().referee(dt), ValueCategory::lvalue, 0};
    bool object_reference = types().is_reference(dt) && !types().is_function(info.type);
    Ex r = node(ast::DeclRef{info, d.entity, object_reference}, begin, end, info);
    if (adl) unqualified_[std::to_underlying(r.id)] = {d};
    if (d.kind == ast::DeclKind::function)
      if (auto ds = found_defaults(d); !ds.empty()) defaults_[std::to_underlying(r.id)] = std::move(ds);
    declared_types_[std::to_underlying(r.id)] = dt;
    return r;
  }

  // ------------------------------------------------------------ literals ([lex.literal])

  Ex literal(QualType t, ConstId v, ValueCategory cat, u32 begin, u32 end) {
    ExprInfo info{t, cat, ins().values.is_dependent(v) ? expr_flag::dependent : u8{0}};
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
      IntShape s = shape(w_->target, k);
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

  enum class LiteralOperatorForm { invalid, integer, raw, other };

  // [over.literal]: check the adjusted parameter types, including types named by aliases.
  LiteralOperatorForm literal_operator_form(QualType t) {
    using enum LiteralOperatorForm;
    using enum Builtin;
    auto f = types().rec(t);
    if (f.kind != TypeKind::function || f.variadic || f.params.empty() || f.params.size() > 2) return invalid;
    QualType first = f.params.front();
    constexpr std::array characters{char_, wchar_t_, char8_t_, char16_t_, char32_t_};
    auto character = [&](QualType q) {
      auto kind = types().builtin_of(q);
      return kind && std::ranges::contains(characters, *kind);
    };
    if (f.params.size() == 1) {
      if (types().is(first, unsigned_long_long)) return integer;
      if (types().is(first, long_double) || character(first)) return other;
    }
    if (!types().is_pointer(first)) return invalid;
    QualType elem = types().pointee(first);
    if (Types::cv(types().canonical(elem)) != Cv::const_) return invalid;
    if (f.params.size() == 1) return types().is(elem, char_) ? raw : invalid;
    return character(elem) && types().is(f.params[1], w_->target.size_type) ? other : invalid;
  }

  bool numeric_literal_template(std::span<const ast::TemplateParamSema> head) {
    return head.size() == 1 && head[0].kind == ast::TemplateArgKind::value && head[0].pack && types().is(head[0].type, Builtin::char_);
  }

  // [lex.ext]: ordinary lookup selects the integer call form before the value is checked as nULL.
  Ex integer_user_literal(NameId name, const lex::Literal& lit, u32 begin, u32 end) {
    Ex callee = name_expr(name, begin, end, true);
    if (!callee.ok()) return callee;
    Ex fail{NodeId::none, begin, end};
    if (as<ast::DepName>(callee)) {
      undeclared(begin, name);
      return fail;
    }
    bool cooked = false, raw = false, numeric_template = false;
    auto record = [&](QualType type) {
      auto form = literal_operator_form(type);
      cooked |= form == LiteralOperatorForm::integer;
      raw |= form == LiteralOperatorForm::raw;
    };
    if (auto it = overloads_.find(std::to_underlying(callee.id)); it != overloads_.end()) {
      for (const ast::DeclSema& d : it->second) {
        if (d.flags & decl_flag::template_) numeric_template |= numeric_literal_template(ins().seqs.elements(d.template_params));
        else record(d.type);
      }
    } else {
      record(callee.type());
    }
    if (!cooked) {
      if (raw && numeric_template) diag(begin, Diag::integer_literal_operator_conflict);
      else if (raw || numeric_template) diag(begin, Diag::integer_literal_operator_unsupported);
      else diag(begin, Diag::integer_literal_operator_missing, name);
      return fail;
    }
    if (lit.overflow) {
      diag(begin, Diag::integer_literal_too_large);
      return fail;
    }
    lex::Literal arg;
    arg.base = lit.base;
    arg.int_suffix = lex::IntSuffix::ull;
    arg.value = lit.value;
    return call(callee, {integer_literal(arg, begin, end)}, end, begin);
  }

  // [lex.fcon]: `double`, or `float` or `long double` by the suffix. The lexer rounded the value to
  // binary32 or binary64, the formats of these types on the target; the extended types' suffixes
  // (`f16`, `bf16`, …) have no values yet (#336).
  Ex float_literal(const lex::Literal& lit, u32 begin, u32 end) {
    using enum lex::FloatSuffix;
    lex::FloatSuffix suffix = lit.float_suffix;
    Builtin k = suffix == f ? Builtin::float_ : suffix == l ? Builtin::long_double : Builtin::double_;
    ast::FloatFormat want = suffix == f ? ast::FloatFormat::binary32 : ast::FloatFormat::binary64;
    if ((suffix != none && suffix != f && suffix != l) || w_->target[k].format != want) {
      diag(begin, Diag::extended_float_not_supported);
      return {NodeId::none, begin, end};
    }
    QualType t = builtin(k);
    return literal(t, ast::float_value(ins(), w_->target, t, ast::Bits128{lit.value, 0}), ValueCategory::prvalue, begin, end);
  }

  static constexpr Builtin literal_character_type(lex::Encoding encoding) {
    using enum lex::Encoding;
    switch (encoding) {
      case ordinary: return Builtin::char_;
      case utf8: return Builtin::char8_t_;
      case utf16: return Builtin::char16_t_;
      case utf32: return Builtin::char32_t_;
      case wide: return Builtin::wchar_t_;
    }
    std::unreachable();
  }

  // [lex.ccon]: a character literal has the type given by its encoding; a multicharacter one, int.
  Ex char_literal(const lex::Literal& l, u32 begin, u32 end) {
    QualType t = builtin(l.multichar ? Builtin::int_ : literal_character_type(l.encoding));
    return literal(t, w_->values.integer(t, l.value), ValueCategory::prvalue, begin, end);
  }

  // [lex.string]: an lvalue array of const code units, including the terminating null.
  Ex string_literal(const lex::Literal& l, u32 begin, u32 end) {
    if (l.encoding == lex::Encoding::wide) {
      diag(begin, Diag::not_supported, lex::Tk::string_lit);
      return {NodeId::none, begin, end};
    }
    std::vector<u32> units = l.units;
    units.push_back(0);
    Builtin elem = literal_character_type(l.encoding);
    QualType t = types().array(types().add_cv(builtin(elem), Cv::const_), units.size());
    return literal(t, w_->values.string(t, units), ValueCategory::lvalue, begin, end);
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
    if (!d || d->reference || !types().is_integral(e.type()) || Types::is_volatile(e.type()) ||
        w_->entities.rec(d->entity).scope == EntityId::none)
      return std::nullopt;
    return q_->constant_value(d->entity);
  }

  // An expression statement's expression, a discarded-value expression ([expr.context]).
  Ex discarded(Ex e) {
    if (address_set(e)) return rvalue(e);
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
    // The underlying value of an enum literal can be folded directly to an integer constant.
    if (auto lit = as<ast::Literal>(e); lit && types().is_enum(e.type()) && types().is_integral(to) &&
                                        (k == ast::ConversionKind::integral || k == ast::ConversionKind::boolean)) {
      u64 bits = w_->values.rec(lit->value).bits;
      if (k == ast::ConversionKind::boolean) bits = bits != 0;
      return literal(to, w_->values.integer(to, bits), ValueCategory::prvalue, e.begin, e.end);
    }
    u8 width = k == ast::ConversionKind::lvalue_to_rvalue || cat != ValueCategory::prvalue ? e.info.bit_width : u8(0);
    ExprInfo info{to, cat, 0, width};
    return node(ast::ImplicitConversion{info, k, EntityId::none, at(e, e.begin)}, e.begin, e.end, info);
  }

  // A target takes a function type ([over.over]), after removing a reference or pointer.
  std::optional<QualType> address_target(QualType to) {
    QualType t = types().referee(to);
    if (types().is_pointer(t)) t = types().pointee(t);
    if (!types().is_function(t)) return std::nullopt;
    return t;
  }

  struct AddressSet {
    Ex name;
    bool address;
    const std::vector<ast::DeclSema>* decls;
  };
  // Recover the name of an overload set from `f`, `&f` or parentheses around either.
  std::optional<AddressSet> address_set(Ex e) {
    if (!e.ok()) return std::nullopt;
    bool address = types().is_pointer(e.type()) && types().is(types().pointee(e.type()), Builtin::overload_set);
    if (address) {
      while (auto p = as<ast::Paren>(e)) e = {p->operand.id, e.begin + p->operand.at, e.end, p->info};
      auto u = as<ast::Unary>(e);
      if (!u || u->op != ast::UnaryOp::address_of) return std::nullopt;
      const Builtin kind = bound_.contains(std::to_underlying(u->operand.id)) ? Builtin::bound_member : Builtin::overload_set;
      e = {u->operand.id, e.begin + u->operand.at, e.end, ExprInfo{builtin(kind), ValueCategory::lvalue, 0}};
    }
    if (!types().is(e.type(), Builtin::overload_set) && !is_bound_member(e)) return std::nullopt;
    auto it = overloads_.find(std::to_underlying(e.id));
    if (it == overloads_.end()) return std::nullopt;
    return AddressSet{e, address, &it->second};
  }

  ArgShape argument_shape(const Ex& a) {
    auto set = address_set(a);
    if (!set) return {a.type(), a.cat() == ValueCategory::lvalue, !is_bound_member(a) && !is_braced(a)};
    ArgShape shape{a.type(), !set->address, false};
    if (std::ranges::any_of(*set->decls, [](const ast::DeclSema& d) { return (d.flags & decl_flag::template_) != 0; }))
      return shape;
    for (const ast::DeclSema& d : *set->decls) {
      if (!constraints_satisfied(d)) continue;
      const auto ft = types().has_placeholder(types().rec(d.type).inner) ? q_->deduced_type(d.entity) : std::optional(d.type);
      if (ft) shape.overloads.push_back(set->address ? types().pointer(*ft) : *ft);
    }
    return shape;
  }

  bool constraints_satisfied(const ast::DeclSema& d, std::span<const TemplateArg> args = {}) {
    if (d.constraints == ConstId::none) return true;
    std::vector<ast::TemplateParamSema> ps = ins().seqs.elements(d.template_params);
    auto nf = templates().substitute_constraint(d.constraints, Bindings::of(ps.empty() ? 0 : ps[0].depth, args));
    return nf && q_->satisfied(*nf).value_or(true);
  }

  struct AddressMatch {
    EntityId entity;
    QualType type;
    const ast::DeclSema* decl;
  };
  // The functions selected by type, constraints and partial ordering ([over.over]).
  std::vector<AddressMatch> address_matches(const AddressSet& set, QualType fn) {
    const auto ex = explicit_args_.find(std::to_underlying(set.name.id));
    const bool explicit_id = ex != explicit_args_.end();
    const std::span<const TemplateArg> args = explicit_id ? std::span<const TemplateArg>(ex->second) : std::span<const TemplateArg>();
    const QualType target = types().canonical(fn);
    std::vector<AddressMatch> found;
    for (const ast::DeclSema& d : *set.decls) {
      if (!(d.flags & decl_flag::template_)) {
        if (explicit_id || !constraints_satisfied(d)) continue;
        const auto ft = types().has_placeholder(types().rec(d.type).inner) ? q_->deduced_type(d.entity) : std::optional(d.type);
        if (ft && types().canonical(*ft) == target) found.push_back({d.entity, *ft, &d});
        continue;
      }
      Templates t = templates();
      auto deduced = t.deduce_declaration(d, fn, args, true);
      if (!deduced || !constraints_satisfied(d, *deduced)) continue;
      auto ft = t.specialization_type(d, *deduced);
      if (!ft) continue;
      EntityId f = specialization(d, *deduced, *ft);
      if (types().has_placeholder(types().rec(*ft).inner)) ft = q_->deduced_type(f);
      if (ft && types().canonical(*ft) == target) found.push_back({f, *ft, &d});
    }
    auto better = [&](const AddressMatch& a, const AddressMatch& b) {
      const bool at = (a.decl->flags & decl_flag::template_) != 0;
      const bool bt = (b.decl->flags & decl_flag::template_) != 0;
      if (at != bt) return !at;
      Templates t = templates();
      if (at) {
        auto [ab, ba] = t.address_ordering(*a.decl, *b.decl);
        if (ab != ba) return ab;
        if (!ab || !equivalent_for_constraints(*a.decl, *b.decl)) return false;
      }
      return t.subsumes(a.decl->constraints, b.decl->constraints) && !t.subsumes(b.decl->constraints, a.decl->constraints);
    };
    std::vector<AddressMatch> remaining;
    for (const AddressMatch& a : found)
      if (std::ranges::none_of(found, [&](const AddressMatch& b) { return &a != &b && better(b, a); }))
        remaining.push_back(a);
    return remaining;
  }

  Ex overload_address(Ex e, QualType to, u32 at_) {
    auto set = address_set(e);
    if (!set) return e;
    auto fn = address_target(to);
    if (!fn) return e;
    auto found = address_matches(*set, *fn);
    NameId name = w_->entities.rec(set->decls->front().entity).name;
    Ex fail{NodeId::none, e.begin, e.end};
    if (found.empty()) {
      diag(at_, Diag::overload_address_none, name, *fn);
      return fail;
    }
    if (found.size() != 1) {
      diag(at_, Diag::overload_address_ambiguous, name);
      return fail;
    }
    const AddressMatch& match = found.front();
    if (auto it = bound_.find(std::to_underlying(set->name.id)); it != bound_.end()) {
      const Bound& member = it->second;
      if (!check_access(member.cls, match.decl->entity, name, member.name_at, member.naming)) return fail;
    }
    if (match.decl->flags & decl_flag::deleted) {
      diag(set->name.begin, Diag::deleted_function, name);
      return fail;
    }
    // Taking a specialization's address demands its definition ([temp.inst]).
    if (match.decl->flags & decl_flag::template_)
      demands.push_back({match.entity, ast::Loc{item_, loc_base_ + set->name.begin}});
    const bool immediate = (match.decl->flags & decl_flag::consteval_) && !immediate_context;
    has_immediate_addresses_ = has_immediate_addresses_ || immediate;
    ExprInfo info{match.type, ValueCategory::lvalue, immediate ? expr_flag::immediate_address : u8(0)};
    Ex r = node(ast::DeclRef{info, match.entity}, set->name.begin, set->name.end, info);
    if (!set->address) return {r.id, e.begin, e.end, r.info};
    ExprInfo pi{types().pointer(match.type), ValueCategory::prvalue, 0};
    return node(ast::Unary{pi, ast::UnaryOp::address_of, at(r, e.begin)}, e.begin, e.end, pi);
  }

  // The conversions every operand that needs a prvalue gets ([basic.lval], [conv.lval],
  // [conv.array], [conv.func]).
  Ex rvalue(Ex e) {
    if (auto c = e.ok() ? const_captures_.find(std::to_underlying(e.id)) : const_captures_.end(); c != const_captures_.end())
      return literal(Types::unqualified(e.type()), c->second, ValueCategory::prvalue, e.begin, e.end);
    if (auto set = address_set(e)) {
      diag(e.begin, Diag::overload_address, w_->entities.rec(set->decls->front().entity).name);
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
    if (unevaluated == 0 && !require_supported_virtual_layout(t, e.begin)) return {NodeId::none, e.begin, e.end};
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
      return convert_node(e, ast::ConversionKind::integral, builtin(sema::promoted(w_->target, *types().builtin_of(u))));
    }
    auto b = types().builtin_of(e.type());
    if (!b || !is_integral(*b)) return e;
    Builtin p = sema::promoted(w_->target, *b, e.info.bit_width);
    if (p == *b) return e;
    return convert_node(e, ast::ConversionKind::integral, builtin(p));
  }

  // The conversion of a prvalue of arithmetic or unscoped enumeration type to arithmetic type `to`:
  // boolean to bool ([conv.bool]), else integral ([conv.integral], [conv.prom]), floating
  // ([conv.double], [conv.fpprom]) or floating-integral ([conv.fpint]). A literal enumerator can
  // use its existing integer constant directly.
  Ex arithmetic_conversion(Ex e, QualType to) {
    using enum ast::ConversionKind;
    bool from_floating = types().is_floating(e.type()), to_floating = types().is_floating(to);
    if (to_floating && types().is_enum(e.type()) && as<ast::Literal>(e)) e = convert_node(e, integral, underlying_of(e.type()));
    ast::ConversionKind k = types().is(to, Builtin::bool_) ? boolean
                          : from_floating && to_floating   ? floating
                          : from_floating || to_floating   ? floating_integral
                                                           : integral;
    return convert_node(e, k, to);
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
    if (types().is_reference(to)) return bind_reference(e, to, at_);
    e = overload_address(e, to, at_);
    if (!e.ok()) return e;
    QualType target = Types::unqualified(to);
    if (auto inv = closure_invoker(e.type(), target)) return invoker_pointer(e, *inv);
    if (types().is_array(target)) {
      if (auto r = string_init(target, e)) return *r;
      diag(at_, Diag::array_initializer);  // [dcl.init.general]/16.5: a list or a string literal
      return {NodeId::none, e.begin, e.end};
    }
    // A class object is copy-initialized from an expression of its own class: a glvalue through
    // the (trivial, implicit) copy constructor, a prvalue directly ([dcl.init.general]/17.6).
    if (types().is_class(target)) {
      if (!require_complete(target, at_) || !require_supported_virtual_layout(target, at_)) return {NodeId::none, e.begin, e.end};
      if (initializes(target, e) && !e.glvalue()) return e;  // the prvalue initializes it directly
      const ClassRec* cr = class_rec(types().class_of(target));
      // A copy through a constructor that is not trivial, or one converting another type
      // ([class.conv.ctor]).
      if (cr && (cr->user_ctors || cr->copy_ctor != EntityId::none || cr->copy_deleted ||
                 cr->defaulted_copy != CopyState::ordinary))
        return construct(target, {e}, e.begin, e.end, false, true);
      if (!initializes(target, e)) {
        // A base class object copied from a derived class's ([dcl.init.general]/17.6.2): from its
        // base class subobject.
        if (base_depth(e.type(), target)) {
          if (!e.glvalue()) e = materialize(e, Types::unqualified(e.type()), false);
          Ex b = to_base(e, types().class_of(target), at_);
          if (!b.ok()) return b;
          return convert_node(b, ast::ConversionKind::lvalue_to_rvalue, target);
        }
        diag(at_, Diag::no_conversion, e.type(), to);
        return {NodeId::none, e.begin, e.end};
      }
      return convert_node(e, ast::ConversionKind::lvalue_to_rvalue, target);
    }
    if (types().is_pointer(target) && is_null_pointer_constant(e))
      return convert_node(e, ast::ConversionKind::null_pointer, target);
    if (types().is_member_pointer(target) && is_null_pointer_constant(e)) {
      diag(at_, Diag::member_pointer_values);
      return {NodeId::none, e.begin, e.end};
    }
    e = rvalue(e);
    if (!e.ok()) return e;
    QualType from = e.type();
    if ((types().is_member_pointer(target) && types().is(from, Builtin::nullptr_t)) ||
        (types().is(target, Builtin::bool_) && types().is_member_pointer(from))) {
      diag(at_, Diag::member_pointer_values);
      return {NodeId::none, e.begin, e.end};
    }
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
    if (e.ok() && is_unscoped_enum(from) && types().is_arithmetic(target)) return arithmetic_conversion(e, target);
    if (types().is_void(from)) {
      diag(at_, Diag::void_value);
      return {NodeId::none, e.begin, e.end};
    }
    if (from == target) return e;
    if (types().is_arithmetic(from) && types().is_arithmetic(target)) return arithmetic_conversion(e, target);
    if (types().is(target, Builtin::bool_) && (types().is_pointer(from) || types().is(from, Builtin::nullptr_t))) {
      if (types().is_pointer(from)) return convert_node(e, ast::ConversionKind::boolean, target);
    }
    if (types().is_pointer(from) && types().is_pointer(target) && qualification_convertible(from, target))
      return convert_node(e, ast::ConversionKind::qualification, target);
    if (types().is_pointer(from) && types().is_pointer(target) && pointer_convertible(from, target))
      return convert_node(e, ast::ConversionKind::pointer, target);
    // [conv.ptr]/3: a pointer to a derived class to one to its base class.
    if (types().is_pointer(from) && types().is_pointer(target) && types().is_class(types().pointee(from)) &&
        types().is_class(types().pointee(target)) && types().class_of(types().pointee(from)) != types().class_of(types().pointee(target)) &&
        cv_within(Types::cv(types().pointee(from)), Types::cv(types().pointee(target))) && is_complete(types().pointee(from))) {
      int paths = 0;
      if (base_path(types().class_of(types().pointee(from)), types().class_of(types().pointee(target)), paths)) {
        Ex b = to_base(e, types().class_of(types().pointee(target)), at_);
        if (!b.ok() || types().canonical(b.type()) == types().canonical(target)) return b;
        return convert_node(b, ast::ConversionKind::qualification, target);
      }
    }
    diag(at_, Diag::no_conversion, from, to);
    return {NodeId::none, e.begin, e.end};
  }

  // [expr.static.cast]/3: a cast to an rvalue reference can name a reference-compatible
  // lvalue as an xvalue. Return no conversion when this rule does not apply.
  std::optional<Ex> cast_lvalue_to_xvalue(Ex e, QualType to, QualType target, u32 at_) {
    std::optional<EntityId> base;
    if (!same_unqualified(e.type(), target)) {
      if (types().is_class(e.type()) && types().is_class(target) && is_complete(e.type())) {
        int paths = 0;
        const auto cls = types().class_of(target);
        if (!base_path(types().class_of(e.type()), cls, paths)) return std::nullopt;
        base = cls;
      } else if (!qualification_convertible(types().pointer(e.type()), types().pointer(target))) {
        return std::nullopt;
      }
    }
    if (!cv_within(object_cv(e.type()), object_cv(target))) {
      diag(at_, Diag::bind_drops_qualifiers, e.type(), to);
      return Ex{NodeId::none, e.begin, e.end};
    }
    if (base) e = to_base(e, *base, at_);
    if (!e.ok()) return e;
    if (e.info.bit_width) return materialize(rvalue(e), target, false);
    return convert_node(e, ast::ConversionKind::qualification, target, ValueCategory::xvalue);
  }

  // [expr.static.cast]: reverse a valid conversion from the complete derived class to its
  // base. `base_clause` admits only nonvirtual bases. A separate result for a failed applicable
  // cast keeps the usual cast rules available for other operands.
  std::optional<Ex> cast_base_to_derived(Ex e, QualType target, bool pointer, u32 at_) {
    QualType from = pointer ? types().pointee(e.type()) : e.type();
    QualType to = pointer ? types().pointee(target) : target;
    if (!types().is_class(from) || !types().is_class(to) || same_unqualified(from, to) ||
        !cv_within(object_cv(from), object_cv(to)) || !is_complete(to))
      return std::nullopt;
    EntityId d = types().class_of(to), b = types().class_of(from);
    int paths = 0;
    auto path = base_path(d, b, paths);
    if (!path) return std::nullopt;
    if (!check_base_path(to, b, path, paths, at_)) return Ex{NodeId::none, e.begin, e.end};
    if (!require_supported_virtual_layout(to, at_)) return Ex{NodeId::none, e.begin, e.end};
    // Build the conversions in evaluation order, starting with B and undoing one edge at a time.
    for (std::size_t i = path->size(); i-- > 0;) {
      QualType result = target;
      if (i != 0) {
        QualType parent = types().add_cv(types().class_type((*path)[i - 1]), object_cv(to));
        result = pointer ? types().pointer(parent) : parent;
      }
      ExprInfo info{result, e.cat(), 0};
      e = node(ast::ImplicitConversion{info, ast::ConversionKind::base_to_derived, (*path)[i], at(e, e.begin)},
               e.begin, e.end, info);
    }
    return e;
  }

  // [over.match.conv]: the bounded exact-result case of direct initialization.
  std::optional<Ex> exact_conversion_cast(Ex object, QualType target, u32 cast_at) {
    const EntityId cls = types().class_of(object.type());
    const NameId name = ins().names.intern(lex::conversion_function_name);
    const ClassRec* record = class_rec(cls);
    if (!record || !record->bases.empty()) return std::nullopt;
    auto candidates = member_functions(cls, name);
    std::erase_if(candidates, [&](const MemberDeclRec* candidate) {
      const TypeRec fn = types().rec(candidate->type);
      return !cv_within(Types::cv(object.type()), fn.fn_cv);
    });
    const MemberDeclRec* selected = nullptr;
    for (const MemberDeclRec* candidate : candidates) {
      const TypeRec fn = types().rec(candidate->type);
      if (types().is_reference(fn.inner)) return std::nullopt;
      if (!same_unqualified(fn.inner, target)) continue;
      if (selected) return std::nullopt;
      selected = candidate;
    }
    if (!selected) return std::nullopt;
    // An exact result wins a tie on the implicit object. Leave sets with
    // different viable object parameter types to general conversion ranking.
    const TypeRec selected_fn = types().rec(selected->type);
    for (const MemberDeclRec* candidate : candidates) {
      const TypeRec fn = types().rec(candidate->type);
      if (fn.fn_cv != selected_fn.fn_cv || fn.fn_ref != selected_fn.fn_ref) return std::nullopt;
    }
    Ex callee = bind_members(cls, {selected}, object, false, object.begin, object.end, cast_at);
    return member_call(callee, {}, object.end, cast_at);
  }

  // A conversion as `static_cast<to>(e)` performs it ([expr.static.cast]); a cast in functional
  // notation or a cast-expression is one when the supported static_cast rules apply
  // ([expr.type.conv]/2, [expr.cast]/4). The downcast slice is for named static_cast expressions.
  Ex static_cast_to(Ex e, QualType to, u32 at_, bool named_static_cast = false) {
    if (!e.ok()) return e;
    if (dependent(e) || types().is_dependent(to))
      return dependent_node({e}, e.begin, e.end, types().is_dependent(to) ? std::nullopt : std::optional(Types::unqualified(to)));
    if (address_set(e)) return convert(e, to, at_);  // [over.over]: a cast supplies a target type
    QualType target = Types::unqualified(to);
    if (types().is_void(target)) return convert_node(e, ast::ConversionKind::to_void, target);  // /6
    if (types().is_reference(to)) {
      const TypeRec ref = types().rec(to);
      if (ref.rvalue_ref && e.cat() == ValueCategory::lvalue && types().is_object(ref.inner)) {
        if (auto cast = cast_lvalue_to_xvalue(e, to, ref.inner, at_)) return *cast;
      } else if (named_static_cast && !ref.rvalue_ref && e.cat() == ValueCategory::lvalue) {
        if (auto cast = cast_base_to_derived(e, ref.inner, /*pointer=*/false, at_)) return *cast;
      }
      return convert(e, to, at_);
    }
    if (types().is_class(target) || types().is_array(target)) return convert(e, to, at_);
    if (named_static_cast && types().is_class(e.type()))
      if (auto cast = exact_conversion_cast(e, target, at_)) return *cast;
    Ex v = rvalue(e);
    if (!v.ok()) return v;
    QualType from = Types::unqualified(v.type());
    if (named_static_cast && types().is_pointer(from) && types().is_pointer(target)) {
      if (auto cast = cast_base_to_derived(v, target, /*pointer=*/true, at_)) return *cast;
    }
    // [expr.static.cast]: a conversion to an enumeration without a fixed underlying type
    // retains the original integral value for the constant evaluator's range check.
    const EnumRec* target_enum = types().is_enum(target) ? enum_rec(types().enum_of(target)) : nullptr;
    if (types().is_enum(target) && types().is_floating(from)) {
      QualType u = underlying_of(target);
      auto c = target_enum && target_enum->fixed && constant_of ? constant_of(v) : std::nullopt;
      if (c && w_->values.rec(*c).kind == ValueKind::floating && types().builtin_of(u)) {
        double x = std::trunc(w_->values.rec(*c).real);
        IntShape sh = shape(w_->target, *types().builtin_of(u));
        double top = std::ldexp(1.0, int(sh.is_signed ? sh.bits - 1 : sh.bits));
        if (sh.is_signed ? x >= -top && x < top : x > -1 && x < top)
          return literal(target, w_->values.integer(u, sh.is_signed ? u64(i64(x)) : u64(x)), ValueCategory::prvalue, v.begin, v.end);
      }
      // An underlying `bool` is reached by [conv.fpint]'s truncation too, through int, not by
      // [conv.bool].
      Ex i = types().is(u, Builtin::bool_) ? arithmetic_conversion(arithmetic_conversion(v, builtin(Builtin::int_)), u)
                                           : arithmetic_conversion(v, u);
      return convert_node(i, ast::ConversionKind::integral, target);
    }
    if (types().is_enum(target) && (types().is_integral(from) || types().is_enum(from))) {
      if (types().canonical(from) == types().canonical(target)) return v;
      if (auto lit = as<ast::Literal>(v); lit && target_enum && target_enum->fixed)
        return literal(target, w_->values.integer(underlying_of(target), w_->values.rec(lit->value).bits), ValueCategory::prvalue,
                       v.begin, v.end);
      return convert_node(v, ast::ConversionKind::integral, target);
    }
    if (is_scoped_enum(from) && types().is_arithmetic(target)) {
      if (types().is_floating(target)) v = arithmetic_conversion(v, underlying_of(from));
      return arithmetic_conversion(v, target);
    }
    // /14: a pointer to cv void to a pointer to an object type at least as cv-qualified.
    if (types().is_pointer(from) && types().is_pointer(target) && types().is_void(types().pointee(from)) &&
        types().is_object(types().pointee(target)) && cv_within(object_cv(types().pointee(from)), object_cv(types().pointee(target))))
      return convert_node(v, ast::ConversionKind::pointer, target);
    Ex converted = convert(v, to, at_);
    // An explicit cast to the field's declared type produces a new prvalue without its width.
    if (converted.ok() && converted.info.bit_width)
      return convert_node(converted, ast::ConversionKind::integral, converted.type());
    return converted;
  }

  // [basic.stc.dynamic] makes the ordinary declarations visible even without a header.
  Ex allocation_name_expr(AllocationFunction f, u32 begin, u32 end) {
    const NameId name = ins().names.intern(allocation_name(f));
    std::vector<ast::DeclSema> found;
    for (const ast::DeclSema& d : w_->allocation_functions()) {
      if (w_->entities.rec(d.entity).name != name) continue;
      q_->use_allocation_function(d.entity);
      found.push_back(d);
    }
    auto written = q_->namespace_members(EntityId::none, name);
    found.insert(found.end(), written.begin(), written.end());
    return decls_expr(std::move(found), true, name, begin, end);
  }

  // [expr.new]: initialize the object after the call to the global allocation function.
  Ex new_object(QualType t, const std::optional<std::vector<Ex>>& args, const Braced* braces, u32 begin, u32 end) {
    Ex fail{NodeId::none, begin, end};
    if (types().has_placeholder(t) || types().is_dependent(t)) {
      diag(begin, Diag::not_supported, lex::Tk::kw_new);
      return fail;
    }
    if (!types().is_object(t)) {
      diag(begin, Diag::new_requires_object, t);
      return fail;
    }
    if (types().is_class(t) && !require_complete(t, begin)) return fail;
    if (!scalar_allocation_type(t)) {
      diag(begin, Diag::not_supported, lex::Tk::kw_new);
      return fail;
    }

    Ex init;
    if (braces) {
      init = list_init(t, *braces);
    } else if (!args) {
      if (Types::is_const(types().canonical(t))) {
        diag(begin, Diag::new_const_uninitialized, t);
        return fail;
      }
      init = default_init(t, end, end);
    } else if (args->empty()) {
      init = value_init(t, end, end);
    } else {
      if (args->size() != 1) {
        diag((*args)[1].begin, Diag::too_many_initializers, t);
        return fail;
      }
      const Ex& value = args->front();
      if (!value.ok()) return fail;
      if (is_braced(value)) {
        diag(value.begin, Diag::braces_for_scalar, t);
        return fail;
      }
      // [dcl.init.general]: direct-initialization of bool from std::nullptr_t yields false.
      init = types().is(t, Builtin::bool_) && types().is(value.type(), Builtin::nullptr_t)
          ? to_bool(value, value.begin) : convert(value, t, value.begin);
    }
    if (!init.ok()) return fail;
    QualType pointer = types().pointer(t);
    if (dependent(init)) return dependent_node({init}, begin, end, pointer);

    Ex size = size_of(t, false, begin, begin);
    ast::DeclSema f = w_->allocation_function(AllocationFunction::allocate);
    q_->use_allocation_function(f.entity);
    ExprInfo fn_info{f.type, ValueCategory::lvalue, 0};
    Ex callee = node(ast::DeclRef{fn_info, f.entity}, begin, begin, fn_info);
    ExprInfo call_info{types().rec(f.type).inner, ValueCategory::prvalue, 0};
    Child sizes[] = {at(size, begin)};
    Ex allocation = node(ast::Call{call_info, f.entity, at(callee, begin), b_->list(sizes)}, begin, begin, call_info);
    ExprInfo info{pointer, ValueCategory::prvalue, 0};
    return node(ast::NewObject{info, at(allocation, begin), at(init, begin)}, begin, end, info);
  }

  // [expr.delete]: cv-qualifiers are ignored when passing the address to the global deallocator.
  Ex delete_object(Ex e, u32 begin, u32 end) {
    Ex fail{NodeId::none, begin, end};
    if (!e.ok()) return fail;
    QualType v = builtin(Builtin::void_);
    if (dependent(e)) return dependent_node({e}, begin, end, v);
    if (types().is_class(e.type()) || types().is_array(e.type())) {
      diag(begin, Diag::not_supported, lex::Tk::kw_delete);  // only pointer operands in this slice
      return fail;
    }
    if (!types().is_pointer(e.type()) || !types().is_object(types().pointee(e.type()))) {
      diag(begin, Diag::delete_requires_pointer, e.type());
      return fail;
    }
    QualType t = types().pointee(e.type());
    if (types().is_class(t) && !require_complete(t, begin)) return fail;
    if (!scalar_allocation_type(t)) {
      diag(begin, Diag::not_supported, lex::Tk::kw_delete);
      return fail;
    }
    Ex addr = convert_node(rvalue(e), ast::ConversionKind::pointer, types().pointer(v));
    if (!addr.ok()) return fail;
    Ex size = size_of(t, false, begin, begin);
    EntityId f = w_->allocation_function(AllocationFunction::deallocate_sized).entity;
    q_->use_allocation_function(f);
    ExprInfo info{v, ValueCategory::prvalue, 0};
    return node(ast::DeleteObject{info, f, at(addr, begin), at(size, begin)}, begin, end, info);
  }

  bool scalar_allocation_type(QualType t) {
    return types().is_integral(t) || types().is_pointer(t);
  }

  // `sizeof` and `alignof` of a type ([expr.sizeof], [expr.alignof]), a `size_t` prvalue; zero
  // after a diagnostic.
  Ex size_of(QualType t, bool align, u32 begin, u32 end) {
    QualType st = builtin(w_->target.size_type);
    if (types().is_dependent(t)) return dependent_node({}, begin, end, st);
    t = types().referee(t);  // [expr.sizeof]/2, [expr.alignof]/3
    if (!require_supported_virtual_layout(t, begin)) return {NodeId::none, begin, end};
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
  // What a complete object type's layout says of it as a component, once per class
  // (`class_places_`).
  std::optional<ast::Placed> place_of(QualType t) {
    TypeRec r = types().rec(t);
    switch (r.kind) {
    case TypeKind::builtin:
      if (!w_->target.has(r.builtin) || r.builtin == Builtin::void_) return std::nullopt;
      return ast::scalar_placed(w_->target[r.builtin].size, w_->target[r.builtin].align);
    case TypeKind::pointer:
    case TypeKind::reference: return ast::scalar_placed(w_->target.pointer_size, w_->target.pointer_align);  // a reference member's
    case TypeKind::member_pointer: {
      ast::BuiltinLayout l = ast::member_pointer_layout(w_->target, types().is_function(r.inner));
      return ast::scalar_placed(l.size, l.align);
    }
    case TypeKind::enum_: return is_complete(t) ? place_of(underlying_of(t)) : std::nullopt;
    case TypeKind::array: {
      if (r.bound_value == ConstId::none) return std::nullopt;
      auto e = place_of(r.inner);
      if (!e) return std::nullopt;
      return ast::array_placed(*e, r.bound);
    }
    case TypeKind::class_: {
      auto l = class_layout(types().class_of(t));
      if (!l) return std::nullopt;
      return l->placed;
    }
    default: return std::nullopt;  // void, functions
    }
  }
  // A complete class's layout (`ast::record_layout`, the rule the back end lays it out by).
  std::optional<ast::RecordLayout> class_layout(EntityId ce) {
    if (auto it = class_places_.find(std::to_underlying(ce)); it != class_places_.end()) return it->second;
    const ClassRec* c = is_complete(types().class_type(ce)) ? class_rec(ce) : nullptr;
    if (!c || c->layout_requires_virtual_support) return std::nullopt;
    std::vector<ast::Placed> bases, places;
    for (const BaseRec& b : c->bases) {
      auto m = place_of(types().class_type(b.cls));
      if (!m) return std::nullopt;
      bases.push_back(*m);
    }
    for (const FieldRec& f : c->fields) {
      auto m = place_of(f.type);
      if (!m) return std::nullopt;
      m->bit_width = f.bit_width;
      places.push_back(*m);
    }
    return class_places_[std::to_underlying(ce)] = ast::record_layout(bases, places, c->union_, c->pod, std::to_underlying(ce));
  }
  std::map<u32, ast::RecordLayout> class_places_;  // complete classes' layouts

  // ------------------------------------------------------------ placeholder types ([dcl.spec.auto])

  // What declared type `t`, which holds a placeholder, deduces from initializer `e`
  // ([dcl.type.auto.deduct]): for `decltype(auto)`, `e`'s decltype ([dcl.type.decltype]); for
  // `auto`, `t` with `auto` replaced by an invented type parameter, deduced as from a call with
  // `e` as the argument ([temp.deduct.call]). `Builtin::dependent` while `e` is type-dependent;
  // none after a diagnosed failure, at `at_`.
  // `replaced` receives what replaced the placeholder, which several declarators of one
  // declaration must agree on ([dcl.type.auto.deduct]/2).
  std::optional<QualType> deduce_placeholder(QualType t, const Ex& e, u32 at_, QualType* replaced = nullptr) {
    if (!e.ok()) return std::nullopt;
    if (dependent(e)) {
      if (replaced) *replaced = builtin(Builtin::dependent);
      return builtin(Builtin::dependent);
    }
    // An overload set, or its address, deduces nothing ([temp.deduct.call]/6); a template-id
    // naming one specialization is not resolved yet (#335).
    if (types().is(e.type(), Builtin::overload_set) || is_bound_member(e) ||
        (types().is_pointer(e.type()) && types().is(types().pointee(e.type()), Builtin::overload_set))) {
      diag(at_, Diag::auto_deduction_failed, t, e.type());
      return std::nullopt;
    }
    if (types().kind(t) == TypeKind::placeholder && types().rec(t).decltype_auto) {
      if (Types::cv(types().canonical(t)) != Cv::none) {  // [dcl.type.auto.deduct]/5: the placeholder alone
        diag(at_, Diag::decltype_auto_alone);
        return std::nullopt;
      }
      // `decltype(auto) x = (y);`: the parenthesized expression's ([dcl.type.auto.deduct]/5).
      QualType r = decltype_of(e, as<ast::Paren>(e).has_value());
      if (replaced) *replaced = r;
      return r;
    }
    if (types_has_decltype_auto(t)) {
      diag(at_, Diag::decltype_auto_alone);
      return std::nullopt;
    }
    // `auto x = f();` of a void `f` deduces void, which the declaration then rejects.
    if (types().is_void(e.type()) && types().kind(t) == TypeKind::placeholder) {
      if (replaced) *replaced = e.type();
      return types().add_cv(e.type(), Types::cv(t));
    }
    if (types().is(e.type(), Builtin::overload_set) || is_bound_member(e) || types().is_void(e.type())) {
      diag(at_, Diag::auto_deduction_failed, t, e.type());
      return std::nullopt;
    }
    Templates tm = templates();
    const u16 depth = u16(template_scopes.size());
    Bindings invent;
    invent.placeholder = types().template_param(depth, 0);
    auto p0 = tm.substitute(t, invent);
    std::vector<std::optional<TemplateArg>> out(1);
    if (p0) {
      auto [p, a, allow] = tm.call_pair(*p0, ArgShape{e.type(), e.cat() == ValueCategory::lvalue}, depth);
      if (tm.deduce(p, a, depth, out, allow) && out[0]) {
        Bindings deduced;
        deduced.placeholder = out[0]->type;
        if (replaced) *replaced = out[0]->type;
        if (auto r = tm.substitute(t, deduced)) return *r;
      }
    }
    diag(at_, Diag::auto_deduction_failed, t, e.type());
    return std::nullopt;
  }
  // Whether `decltype(auto)` appears in `t` other than as `t` itself, which is ill-formed
  // ([dcl.type.auto.deduct]/5): `decltype(auto)&`, `decltype(auto)*`.
  bool types_has_decltype_auto(QualType t) {
    for (;;) {
      TypeRec r = types().rec(t);
      if (r.kind == TypeKind::placeholder) return r.decltype_auto;
      if (r.kind != TypeKind::pointer && r.kind != TypeKind::member_pointer && r.kind != TypeKind::reference &&
          r.kind != TypeKind::array && r.kind != TypeKind::function)
        return false;
      t = r.inner;
    }
  }

  // [dcl.struct.bind]/1, /2, /6: the hidden entity is an array copy or an ordinary
  // deduced reference to the initializer. The copy takes its cv from the declaration.
  std::optional<std::pair<QualType, Ex>> array_binding(QualType declared, Ex source, u64 names, u32 at_) {
    if (!source.ok()) return std::nullopt;
    QualType array = source.type();
    if (dependent(source) || !types().is_array(array)) {
      diag(at_, Diag::binding_unsupported_type, array);
      return std::nullopt;
    }
    if (!require_complete(array, at_)) return std::nullopt;
    if (u64 bound = types().rec(array).bound; names != bound) {
      diag(at_, Diag::binding_arity, names, bound);
      return std::nullopt;
    }
    QualType leaf = array;
    while (types().is_array(leaf)) leaf = types().rec(leaf).inner;
    if (!types().is_scalar(leaf) || types().is_member_pointer(leaf)) {
      diag(at_, Diag::binding_unsupported_type, array);
      return std::nullopt;
    }
    if (types().is_reference(declared)) {
      auto ref = deduce_placeholder(declared, source, source.begin);
      if (!ref) return std::nullopt;
      Ex init = bind_reference(source, *ref, source.begin, true);
      if (!init.ok()) return std::nullopt;
      return std::pair{*ref, init};
    }
    QualType type = types().add_cv(object_unqualified(array), Types::cv(declared));
    if (!source.glvalue()) source = materialize(source, array, false);
    return std::pair{type, convert_node(source, ast::ConversionKind::array_copy, type)};
  }

  // [dcl.type.decltype]/1: an unparenthesized id-expression or member access names its entity's
  // declared type; any other expression has its type, as `T&&` for an xvalue and `T&` for an
  // lvalue.
  QualType decltype_of(Ex e, bool parenthesized) {
    if (!parenthesized) {
      if (auto l = as<ast::LocalRef>(e)) return local_declared_type(locals_[l->local]);
      if (as<ast::Dependent>(e))
        if (auto it = declared_types_.find(std::to_underlying(e.id)); it != declared_types_.end()) return it->second;
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
  // ------------------------------------------------------------ base classes ([class.derived])

  // The direct bases leading from class `d` to its base class `b` ([class.derived.general]/2),
  // each step's class, `b` last; none if `b` is not a base of `d`. `paths` counts the ways `b` is
  // a base: more than one is an ambiguous base ([class.mi]/5, [conv.ptr]/3).
  std::optional<std::vector<EntityId>> base_path(EntityId d, EntityId b, int& paths) {
    std::optional<std::vector<EntityId>> found;
    const ClassRec* r = class_rec(d);
    if (!r) return std::nullopt;
    for (const BaseRec& x : r->bases) {
      if (x.cls == b) {
        ++paths;
        found = std::vector<EntityId>{b};
        continue;
      }
      if (auto p = base_path(x.cls, b, paths)) {
        p->insert(p->begin(), x.cls);
        found = std::move(p);
      }
    }
    return found;
  }
  // The number of steps from class type `from` to its unambiguous base class type `to`, 0 when
  // `to` is not one (cv-qualifiers aside).
  int base_depth(QualType from, QualType to) {
    if (!types().is_class(from) || !types().is_class(to) || types().is_dependent(from) || types().is_dependent(to)) return 0;
    EntityId d = types().class_of(from), b = types().class_of(to);
    if (d == b || !is_complete(from)) return 0;
    int paths = 0;
    auto p = base_path(d, b, paths);
    return p && paths == 1 ? int(p->size()) : 0;
  }
  // Check ambiguity before looking at the access of any edge ([class.access.base]/4).
  bool check_base_path(QualType ct, EntityId b, const std::optional<std::vector<EntityId>>& path, int paths, u32 at_) {
    if (!path || paths != 1) {
      diag(at_, Diag::ambiguous_base, types().class_type(b), Types::unqualified(ct));
      return false;
    }
    EntityId from = types().class_of(ct);
    for (EntityId step : *path) {
      const ClassRec* r = class_rec(from);
      auto x = std::ranges::find(r->bases, step, &BaseRec::cls);
      if (!accessible(from, x->access)) {
        diag(at_, Diag::inaccessible_base, types().class_type(b), Types::unqualified(ct));
        return false;
      }
      from = step;
    }
    return true;
  }

  // `e`, a glvalue of class type or a pointer to one, converted to its base class `b`
  // ([conv.ptr]/3, [dcl.init.ref]/5, [expr.static.cast]): one derived-to-base conversion per
  // direct base on the way, the cv-qualifiers kept.
  Ex to_base(Ex e, EntityId b, u32 at_) {
    if (!e.ok()) return e;
    bool pointer = types().is_pointer(e.type());
    QualType ct = pointer ? types().pointee(e.type()) : e.type();
    Cv cv = Types::cv(ct);
    int paths = 0;
    auto path = base_path(types().class_of(ct), b, paths);
    if (!check_base_path(ct, b, path, paths, at_)) return {NodeId::none, e.begin, e.end};
    if (!require_supported_virtual_layout(ct, at_)) return {NodeId::none, e.begin, e.end};
    for (EntityId step : *path) {
      QualType st = types().add_cv(types().class_type(step), cv);
      ExprInfo info{pointer ? types().pointer(st) : st, pointer ? ValueCategory::prvalue : e.cat(), 0};
      e = node(ast::ImplicitConversion{info, ast::ConversionKind::derived_to_base, step, at(e, e.begin)}, e.begin, e.end, info);
    }
    return e;
  }

  bool pointer_convertible(QualType from, QualType to) {
    QualType a = types().pointee(from), b = types().pointee(to);
    return types().is_void(b) && types().is_object(a) && cv_within(object_cv(a), object_cv(b));
  }

  // [expr.type]: combine the pointee cv-qualifiers of a pointer to void and a
  // pointer to an object or void.
  std::optional<QualType> composite_void_pointer(QualType a, QualType b) {
    if (!types().is_pointer(a) || !types().is_pointer(b)) return std::nullopt;
    a = types().pointee(a);
    b = types().pointee(b);
    if (!types().is_void(a)) std::swap(a, b);
    if (!types().is_void(a) || (!types().is_void(b) && !types().is_object(b))) return std::nullopt;
    Cv cv = Cv(std::to_underlying(object_cv(a)) | std::to_underlying(object_cv(b)));
    return types().pointer(types().add_cv(builtin(Builtin::void_), cv));
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
    if (types().is_member_pointer(t)) diag(at_, Diag::member_pointer_values);
    else diag(at_, Diag::not_contextually_bool, t);
    return {NodeId::none, e.begin, e.end};
  }

  // [expr.arith.conv]: both operands are promoted and converted to the common type, a floating
  // type if either is one.
  QualType arithmetic_conversions(Ex& a, Ex& c) {
    a = promote(a);
    c = promote(c);
    Builtin k = common_integer(w_->target, *types().builtin_of(a.type()), *types().builtin_of(c.type()));
    QualType t = builtin(k);
    if (a.type() != t) a = arithmetic_conversion(a, t);
    if (c.type() != t) c = arithmetic_conversion(c, t);
    return t;
  }

  // ------------------------------------------------------------ operators ([expr.unary], [expr.compound])

  bool is_integral_or_unscoped(QualType t) { return types().is_integral(t); }
  // A pointer to a completely-defined object type, which [expr.add] and [expr.pre.incr] need.
  bool is_object_pointer(QualType t) {
    return types().is_pointer(t) && types().is_object(types().pointee(t)) && is_complete(types().pointee(t));
  }

  bool usable_copy_assignment(CopyAssignment state, QualType type, u32 at_) {
    switch (state) {
    case CopyAssignment::trivial:
    case CopyAssignment::memberwise: return true;
    case CopyAssignment::const_member: diag(at_, Diag::const_member_assign, type); break;
    case CopyAssignment::deleted:
      if (const ClassRec* r = class_rec(types().class_of(type)); r && r->closure && r->default_deleted)
        diag(at_, Diag::closure_assignment, type);
      else diag(at_, Diag::deleted_function, ins().names.intern("operator="));
      break;
    case CopyAssignment::none:
    case CopyAssignment::unsupported: diag(at_, Diag::unsupported_copy_assignment, type); break;
    }
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
    // `+closure`: the built-in `+` of the pointer its conversion function gives ([over.built]/9).
    if (op == plus && types().is_class(e.type()))
      if (const ClassRec* r = class_rec(types().class_of(e.type())))
        for (const MemberDeclRec& m : r->members)
          if (m.is_static && m.name == ins().names.intern("__invoke")) {
            Ex p = invoker_pointer(e, m.entity);
            return {p.id, begin, end, p.info};
          }
    auto make = [&](Ex operand, QualType t, ValueCategory cat) {
      ExprInfo info{t, cat, 0, cat == ValueCategory::prvalue ? u8(0) : operand.info.bit_width};
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
      if (e.glvalue() && e.info.bit_width) {
        diag(op_at, Diag::bit_field_address);
        return {NodeId::none, begin, end};
      }
      if (is_bound_member(e) && address_set(e))
        return make(e, types().pointer(builtin(Builtin::overload_set)), ValueCategory::prvalue);
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
      if (types().is_pointer(t) && !require_supported_virtual_layout(types().pointee(t), op_at))
        return {NodeId::none, begin, end};
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
        if (types().is_floating(a.type()) || types().is_floating(c.type()))
          return invalid();  // [expr.arith.conv]/1: and between an enumeration and a floating type
        if (types().is_enum(a.type())) a = promote(a);
        if (types().is_enum(c.type())) c = promote(c);
      }
    }
    QualType ta = a.type(), tc = c.type();
    bool arith = types().is_arithmetic(ta) && types().is_arithmetic(tc);
    bool integral = types().is_integral(ta) && types().is_integral(tc);
    auto arithmetic_bin = [&](ast::BinaryOp kind) {
      QualType t = arithmetic_conversions(a, c);
      return bin(kind, a, c, t);
    };
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
      return arithmetic_bin(k);
    case rem:
    case bit_and:
    case bit_xor:
    case bit_or:
      if (!integral) return invalid();
      return arithmetic_bin(k);
    case add:
    case sub:
      if (arith) return arithmetic_bin(k);
      // [expr.add]: a pointer to a complete object type and an integer; or, for `-`, two
      // pointers to the same type, giving std::ptrdiff_t (long).
      if (is_object_pointer(ta) && types().is_integral(tc)) {
        if (!require_supported_virtual_layout(types().pointee(ta), op_at)) return {NodeId::none, begin, end};
        return bin(k, a, promote(c), ta);
      }
      if (op == add && types().is_integral(ta) && is_object_pointer(tc)) {
        if (!require_supported_virtual_layout(types().pointee(tc), op_at)) return {NodeId::none, begin, end};
        return bin(k, promote(a), c, tc);
      }
      if (op == sub && is_object_pointer(ta) && is_object_pointer(tc) &&
          Types::unqualified(types().pointee(ta)) == Types::unqualified(types().pointee(tc))) {
        if (!require_supported_virtual_layout(types().pointee(ta), op_at)) return {NodeId::none, begin, end};
        return bin(k, a, c, builtin(Builtin::long_));
      }
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
      if (equality && ((types().is_member_pointer(ta) && (same_unqualified(ta, tc) || is_null_pointer_constant(r))) ||
                       (types().is_member_pointer(tc) && is_null_pointer_constant(l)))) {
        diag(op_at, Diag::member_pointer_values);
        return {NodeId::none, begin, end};
      }
      if (auto to = composite_void_pointer(ta, tc)) {
        a = convert(a, *to, op_at);
        c = convert(c, *to, op_at);
        if (!a.ok() || !c.ok()) return {NodeId::none, begin, end};
        return bin(k, a, c, b);
      }
      if (types().is_pointer(ta) && types().is_pointer(tc)) {
        QualType pa = types().pointee(ta), pc = types().pointee(tc);
        // [expr.type]/4.6: a pointer to a derived class converted to one to its base class.
        if (base_depth(pa, pc)) {
          a = to_base(a, types().class_of(pc), op_at);
          if (!a.ok()) return Ex{NodeId::none, begin, end};
          ta = a.type();
          pa = types().pointee(ta);
        } else if (base_depth(pc, pa)) {
          c = to_base(c, types().class_of(pa), op_at);
          if (!c.ok()) return Ex{NodeId::none, begin, end};
          tc = c.type();
          pc = types().pointee(tc);
        }
        QualType unqualified = Types::unqualified(pa);
        Cv ca = Types::cv(pa), cc = Types::cv(pc);
        if (unqualified != Types::unqualified(pc)) {
          // [expr.type], [conv.qual]: complete arrays of the same shape combine the
          // cv-qualifiers of their elements, including through nested array dimensions.
          if (!types().is_array(pa)) return invalid();
          unqualified = object_unqualified(pa);
          if (unqualified != object_unqualified(pc) || !is_complete(pa)) return invalid();
          ca = object_cv(pa);
          cc = object_cv(pc);
        }
        Cv cv = Cv(std::to_underlying(ca) | std::to_underlying(cc));
        QualType composite = types().pointer(types().add_cv(unqualified, cv));
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
    if (op == AsgOp::assign && types().is_class(l.type())) {
      NameId name = ins().names.intern("operator=");
      Ex callee = member(l, false, name, l.end, op_at, op_at);
      return callee.ok() ? member_call(callee, {r}, end, op_at, true) : Ex{NodeId::none, begin, end};
    }
    // `=` is a member only ([over.assign]); `operator_function_ok` rejects non-member ones.
    static constexpr std::string_view assign_spellings[] = {"=", "*=", "/=", "%=", "+=", "-=", "<<=", ">>=", "&=", "^=", "|="};
    if (auto o = overloaded(assign_spellings[std::to_underlying(op)], {l, r}, begin, end, op_at)) return *o;
    if (!check_modifiable(l, op_at)) return {NodeId::none, begin, end};
    QualType lt = l.type();
    QualType computation = Types::unqualified(lt);
    Ex c = r;
    if (op == AsgOp::assign) {
      c = convert(r, lt, r.begin);
    } else {
      Ex lv = rvalue(l);  // only to read its type; the IR loads the left operand itself
      QualType tl = lv.type();
      // [expr.arith.conv]/1: an enumeration and a floating type do not mix.
      if ((types().is_enum(tl) && types().is_floating(r.type())) || (types().is_floating(tl) && types().is_enum(r.type()))) {
        diag(op_at, Diag::invalid_operands, lt, r.type());
        return {NodeId::none, begin, end};
      }
      Ex rv = is_unscoped_enum(r.type()) ? promote(r) : rvalue(r);  // [conv.prom]
      if (!rv.ok()) return {NodeId::none, begin, end};
      QualType tr = rv.type();
      bool shift = op == AsgOp::shl || op == AsgOp::shr;
      bool integral_only = shift || op == AsgOp::rem || op == AsgOp::bit_and || op == AsgOp::bit_xor || op == AsgOp::bit_or;
      if (types().is_arithmetic(tl) && types().is_arithmetic(tr) &&
          (!integral_only || (types().is_integral(tl) && types().is_integral(tr)))) {
        if (shift) {
          computation = builtin(sema::promoted(w_->target, *types().builtin_of(tl), lv.info.bit_width));
          c = promote(rv);
        } else {
          Builtin left = sema::promoted(w_->target, *types().builtin_of(tl), lv.info.bit_width);
          Builtin right = sema::promoted(w_->target, *types().builtin_of(tr), rv.info.bit_width);
          Builtin common = common_integer(w_->target, left, right);
          computation = builtin(common);
          c = convert(rv, computation, r.begin);
        }
      } else if ((op == AsgOp::add || op == AsgOp::sub) && is_object_pointer(tl) && types().is_integral(tr)) {
        if (!require_supported_virtual_layout(types().pointee(tl), op_at)) return {NodeId::none, begin, end};
        computation = tl;
        c = promote(rv);
      } else {
        diag(op_at, Diag::invalid_operands, lt, r.type());
        return {NodeId::none, begin, end};
      }
    }
    if (!c.ok()) return {NodeId::none, begin, end};
    ExprInfo info{lt, ValueCategory::lvalue, 0, l.info.bit_width};
    return node(ast::Assign{info, ast::AssignOp(std::to_underlying(op)), at(l, begin), at(c, begin), computation}, begin,
                end, info);
  }

  // [expr.assign]/9: `x = {…}` assigns `T{…}`, list-initialized, to an object of type T.
  Ex assign_list(Ex l, const Braced& b, u32 op_at) {
    u32 begin = l.begin, end = b.end;
    if (l.ok() && (dependent(l) || dependent(b))) return dependent_node({l}, begin, end, std::nullopt, ValueCategory::lvalue);
    if (!l.ok() || !check_modifiable(l, op_at)) return {NodeId::none, begin, end};
    const bool class_copy = types().is_class(l.type());
    if (class_copy) {
      if (!require_complete(l.type(), op_at) || !require_supported_virtual_layout(l.type(), op_at))
        return {NodeId::none, begin, end};
      const auto* cls = class_rec(types().class_of(l.type()));
      const auto* copy = cls ? cls->implicit_copy_assignment() : nullptr;
      CopyAssignment state = CopyAssignment::unsupported;
      if (copy && std::ranges::count(cls->members, copy->name, &MemberDeclRec::name) == 1)
        state = copy->copy_assignment == CopyAssignment::memberwise ? CopyAssignment::unsupported
                                                                    : copy->copy_assignment;
      if (!usable_copy_assignment(state, Types::unqualified(l.type()), op_at)) return {NodeId::none, begin, end};
    }
    Ex c = list_init(Types::unqualified(l.type()), b);
    if (!c.ok()) return {NodeId::none, begin, end};
    ExprInfo info{l.type(), ValueCategory::lvalue, 0, l.info.bit_width};
    auto op = class_copy ? ast::AssignOp::trivial_class_copy : ast::AssignOp::assign;
    return node(ast::Assign{info, op, at(l, begin), at(c, begin), Types::unqualified(l.type())},
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
    // [expr.cond]/4.1: glvalues of one category, of a class and its base class: the derived one
    // converted to the base, as reference binding does.
    if (t.glvalue() && t.cat() == f.cat() && types().is_class(t.type()) && types().is_class(f.type())) {
      if (base_depth(t.type(), f.type()) && cv_within(object_cv(t.type()), object_cv(f.type())))
        t = to_base(t, types().class_of(f.type()), q_at);
      else if (base_depth(f.type(), t.type()) && cv_within(object_cv(f.type()), object_cv(t.type())))
        f = to_base(f, types().class_of(t.type()), q_at);
      if (!t.ok() || !f.ok()) return {NodeId::none, begin, end};
    }
    if (t.glvalue() && t.cat() == f.cat() && t.type() == f.type()) {
      if (t.info.bit_width || f.info.bit_width) {
        diag(q_at, Diag::bit_field_conditional);
        return {NodeId::none, begin, end};
      }
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
      } else if (auto to = composite_void_pointer(ta, tb)) {
        a = convert(a, *to, q_at);
        b = convert(b, *to, q_at);
        if (!a.ok() || !b.ok()) return {NodeId::none, begin, end};
        info = {*to, ValueCategory::prvalue, 0};
      } else if (types().is_pointer(ta) && types().is_pointer(tb) &&
                 (base_depth(types().pointee(ta), types().pointee(tb)) || base_depth(types().pointee(tb), types().pointee(ta)))) {
        // [expr.cond]/7.3: the composite pointer type, a pointer to the base class.
        bool down = base_depth(types().pointee(ta), types().pointee(tb));
        if (down) a = convert(a, tb, q_at);
        else b = convert(b, ta, q_at);
        if (!a.ok() || !b.ok()) return {NodeId::none, begin, end};
        info = {down ? tb : ta, ValueCategory::prvalue, 0};
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
  Ex call(Ex callee, std::vector<Ex> args, u32 end, u32 paren_at, std::optional<u32> lookup_at = std::nullopt) {
    u32 begin = callee.begin;
    std::vector<NodeId> defaults;  // the chosen function's default arguments
    if (!callee.ok() || std::ranges::any_of(args, [](const Ex& a) { return !a.ok(); })) return {NodeId::none, begin, end};
    // A member function called with a dependent argument is resolved when the template is
    // instantiated ([temp.dep.expr]).
    bool dependent_args = any_dependent(args);
    if (is_bound_member(callee) && !dependent_args) return member_call(callee, std::move(args), end, paren_at);
    // [temp.dep.candidate]: an unqualified call with a type-dependent argument is resolved when the
    // template is instantiated, among what lookup finds at the definition and argument-dependent
    // lookup finds at the point of instantiation.
    std::optional<NameId> unqualified = unqualified_name(callee);
    if (unqualified) {
      u32 offset = lookup_at.value_or(callee.begin);
      if (std::ranges::any_of(args, [&](const Ex& a) { return types().is_dependent(a.type()); })) {
        dependent_calls.push_back(offset);
      } else {
        callee = with_adl(callee, *unqualified, args, q_->dependent_call(offset));
        if (auto d = as<ast::DepName>(callee)) {
          undeclared(callee.begin, d->name);
          return {NodeId::none, begin, end};
        }
      }
    }
    if (dependent(callee) || dependent_args) {
      std::vector<Ex> all{callee};
      all.insert(all.end(), args.begin(), args.end());
      return dependent_node(all, begin, end);
    }
    // Keep the direct initialization path for a sole known function
    // ([dcl.init.list]/1).
    auto braced = std::ranges::find_if(args, [&](const Ex& a) { return is_braced(a); });
    if (braced != args.end() && types().is(callee.type(), Builtin::overload_set)) {
      const std::vector<ast::DeclSema>& set = overloads_.at(std::to_underlying(callee.id));
      if (set.size() == 1 && !(set[0].flags & decl_flag::template_)) {
        auto st = deduced_function(set[0].entity, set[0].type, callee.begin);
        if (!st) return {NodeId::none, begin, end};
        defaults = found_defaults(set[0]);
        if (set[0].flags & decl_flag::consteval_) consteval_.insert(std::to_underlying(set[0].entity));
        if (set[0].flags & decl_flag::deleted) deleted_.insert(std::to_underlying(set[0].entity));
        ExprInfo ci{*st, ValueCategory::lvalue, 0};
        callee = node(ast::DeclRef{ci, set[0].entity}, callee.begin, callee.end, ci);
      }
    }
    if (types().is(callee.type(), Builtin::overload_set)) {
      const std::vector<ast::DeclSema>& set = overloads_.at(std::to_underlying(callee.id));
      auto ex = explicit_args_.find(std::to_underlying(callee.id));
      std::span<const TemplateArg> explicit_args;
      if (ex != explicit_args_.end()) explicit_args = ex->second;
      std::vector<const Braced*> lists;
      if (braced != args.end())
        for (const Ex& a : args) lists.push_back(braced_list(a));
      std::vector<ArgShape> shapes;
      if (lists.empty())
        for (const Ex& a : args) shapes.push_back(argument_shape(a));
      std::vector<Candidate> cands;
      std::vector<const ast::DeclSema*> decls;
      for (const ast::DeclSema& d : set) {
        bool deleted = (d.flags & decl_flag::deleted) != 0;
        if (!(d.flags & decl_flag::template_)) {
          // Explicit template arguments name only templates ([temp.arg.explicit]).
          if (ex != explicit_args_.end()) continue;
          cands.push_back({d.entity, d.type, true, deleted, nullptr, found_defaults(d)});
          decls.push_back(&d);
          continue;
        }
        if (!lists.empty()) {
          // General deduction from a braced list is deferred (#231). Keep a possible
          // candidate so it cannot disappear before viability is checked.
          cands.push_back({d.entity, d.type, true, deleted, &d, found_defaults(d)});
          decls.push_back(&d);
          continue;
        }
        // A function template contributes the specialization deduction gives ([temp.over]/1),
        // or nothing when deduction fails ([temp.deduct.general]).
        Templates t = templates();
        auto targs = t.deduce_call(d, shapes, explicit_args, found_defaults(d));
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
        cands.push_back({specialization(d, *targs, *st), *st, true, deleted, &d, found_defaults(d)});
        decls.push_back(&d);
      }
      auto chosen = resolve(cands, args, callee.begin, w_->entities.rec(set.front().entity).name, std::nullopt, std::nullopt, lists);
      if (!chosen) return {NodeId::none, begin, end};
      const ast::DeclSema& d = *decls[*chosen];
      const Candidate& k = cands[*chosen];
      if (d.flags & decl_flag::consteval_) consteval_.insert(std::to_underlying(k.entity));
      // A specialization called is one whose definition the program requires ([temp.inst]/8).
      if (k.pattern) demands.push_back({k.entity, ast::Loc{item_, loc_base_ + callee.begin}});
      auto kt = deduced_function(k.entity, k.type, callee.begin);
      if (!kt) return {NodeId::none, begin, end};
      ExprInfo ci{*kt, ValueCategory::lvalue, 0};
      callee = node(ast::DeclRef{ci, k.entity}, callee.begin, callee.end, ci);
      defaults = k.defaults;
    } else if (auto d = as<ast::DeclRef>(callee); d && deleted_.contains(std::to_underlying(d->entity))) {
      // One function: still not callable when deleted ([dcl.fct.def.delete]).
      diag(callee.begin, Diag::deleted_function, w_->entities.rec(d->entity).name);
      return {NodeId::none, begin, end};
    } else if (d && unqualified && types().has_placeholder(types().rec(callee.type()).inner)) {
      // A sole ordinary function's return type can be deduced now that ADL has supplied no
      // other candidate. Keep the lookup's defaults when rewriting the reference.
      auto dt = deduced_function(d->entity, callee.type(), callee.begin);
      if (!dt) return {NodeId::none, begin, end};
      defaults = defaults_of(callee, d->entity);
      ExprInfo ci{*dt, ValueCategory::lvalue, 0};
      callee = node(ast::DeclRef{ci, d->entity}, callee.begin, callee.end, ci);
    }
    // A call of a class object calls its `operator()` ([over.call.object]); surrogate call
    // functions (conversions to pointers to functions) are not considered yet.
    if (types().is_class(callee.type()) && !types().is(callee.type(), Builtin::overload_set)) {
      EntityId c = types().class_of(callee.type());
      if (!require_complete(callee.type(), paren_at)) return {NodeId::none, begin, end};
      NameId op = ins().names.intern("operator()");
      if (member_functions(c, op).empty()) {
        diag(paren_at, Diag::not_callable, callee.type());
        return {NodeId::none, begin, end};
      }
      Ex bound = bound_member(c, op, callee, false, begin, callee.end, paren_at);
      return member_call(bound, std::move(args), end, paren_at);
    }
    EntityId function = EntityId::none;
    QualType ft = callee.type();
    Ex named = callee;  // `(f)()` takes f's default arguments too ([expr.call]/1)
    while (auto p = as<ast::Paren>(named)) named = Ex{p->operand.id};
    if (auto d = as<ast::DeclRef>(named); d && types().is_function(ft) && defaults.empty()) defaults = defaults_of(named, d->entity);
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
    auto children = call_args(fr, args, begin, paren_at, defaults);
    if (!children) return {NodeId::none, begin, end};
    ExprInfo info = result_info(fr.inner);
    return node(ast::Call{info, function, at(callee, begin), b_->list(*children)}, begin, end, info);
  }

  // The unqualified-id of a callee that permits ADL ([basic.lookup.argdep]/1). A qualified or
  // parenthesized callee has no such entry.
  std::optional<NameId> unqualified_name(const Ex& callee) {
    if (auto d = as<ast::DepName>(callee)) return d->name;
    auto r = as<ast::DeclRef>(callee);
    if (!r || !unqualified_.contains(std::to_underlying(callee.id))) return std::nullopt;
    return w_->entities.rec(r->entity).name;
  }

  EntityId enclosing_namespace(EntityId entity) {
    EntityId s = w_->entities.rec(entity).scope;
    while (s != EntityId::none && w_->entities.rec(s).kind != EntityKind::namespace_) s = w_->entities.rec(s).scope;
    return s;
  }

  // The namespace found by the existing operator lookup.
  std::optional<EntityId> associated_namespace(QualType t) {
    TypeRec r = types().rec(t);
    while (r.kind == TypeKind::pointer || r.kind == TypeKind::array || r.kind == TypeKind::reference) r = types().rec(r.inner);
    if (r.kind != TypeKind::class_ && r.kind != TypeKind::enum_) return std::nullopt;
    return enclosing_namespace(r.entity);
  }

  struct AdlAssociations {
    std::vector<EntityId> namespaces;
    std::vector<EntityId> classes;
  };

  // [basic.lookup.argdep]/3: the namespaces and classes associated with the argument types.
  // The namespace query adds each nearest non-inline ancestor and its inline namespace set.
  AdlAssociations adl_associations(std::span<const Ex> args, bool at_instantiation, bool ordinary_call) {
    AdlAssociations result;
    auto add_namespace = [&](EntityId entity) {
      EntityId ns = enclosing_namespace(entity);
      if (!std::ranges::contains(result.namespaces, ns)) result.namespaces.push_back(ns);
    };
    auto add_class = [&](EntityId cls) {
      // Friend declarations in class templates are outside this slice. Keep the existing
      // dependent lookup's limits on when it instantiates associated classes.
      const EntityRec& entity = w_->entities.rec(cls);
      if (entity.kind == EntityKind::class_template || entity.templ != EntityId::none) return;
      if (!std::ranges::contains(result.classes, cls)) result.classes.push_back(cls);
    };
    // [basic.lookup.argdep]/3: bases contribute their entities. Expanding a base's template
    // arguments requires a separate occurrence of its type in the argument walk.
    std::vector<EntityId> base_walk;
    auto collect_bases = [&](this auto&& visit, EntityId cls) -> void {
      if (!ordinary_call && w_->entities.rec(cls).templ != EntityId::none) return;
      if (std::ranges::contains(base_walk, cls)) return;
      base_walk.push_back(cls);
      if (const ClassRec* c = q_->adl_class(cls, at_instantiation)) {
        for (const BaseRec& b : c->bases) {
          add_namespace(b.cls);
          add_class(b.cls);
          visit(b.cls);
        }
      }
    };
    std::vector<QualType> expanded_types;
    auto collect = [&](this auto&& visit, QualType t) -> void {
      t = Types::unqualified(types().canonical(t));
      if (std::ranges::contains(expanded_types, t)) return;
      expanded_types.push_back(t);
      TypeRec r = types().rec(t);
      switch (r.kind) {
      case TypeKind::class_:
      case TypeKind::enum_: {
        add_namespace(r.entity);
        const EntityRec& entity = w_->entities.rec(r.entity);
        if (entity.scope != EntityId::none && w_->entities.rec(entity.scope).kind == EntityKind::class_)
          add_class(entity.scope);
        if (r.kind == TypeKind::class_) {
          add_class(r.entity);
          collect_bases(r.entity);
        }
        if (!ordinary_call) break;
        auto visit_type_arg = [&](const TemplateArg& arg) {
          if (arg.kind == ast::TemplateArgKind::type) visit(arg.type);
        };
        for (const TemplateArg& arg : entity.args) {
          if (arg.kind == ast::TemplateArgKind::pack)
            std::ranges::for_each(ins().seqs.elements(arg.elems), visit_type_arg);
          else
            visit_type_arg(arg);
        }
        break;
      }
      case TypeKind::function:
        for (QualType p : r.params) visit(p);
        visit(r.inner);
        break;
      case TypeKind::member_pointer:
        visit(r.member_owner);
        visit(r.inner);
        break;
      case TypeKind::pointer:
      case TypeKind::array:
      case TypeKind::reference:
        visit(r.inner);
        break;
      default: break;
      }
    };
    for (const Ex& a : args) collect(a.type());
    return result;
  }

  // Merge the ordinary candidates with ADL before overload resolution ([over.call.func]).
  Ex with_adl(Ex callee, NameId name, std::span<const Ex> args, bool dependent_call) {
    std::vector<ast::DeclSema> set;
    if (auto it = unqualified_.find(std::to_underlying(callee.id)); it != unqualified_.end()) set = it->second;
    bool changed = false;
    auto default_count = [&](const ast::DeclSema& d) {
      return std::ranges::count_if(found_defaults(d), [](NodeId n) { return n != NodeId::none; });
    };
    auto add_candidate = [&](const ast::DeclSema& d) {
      auto prior = std::ranges::find(set, d.entity, &ast::DeclSema::entity);
      if (prior == set.end()) {
        set.push_back(d);
        changed = true;
      } else if (d.params != prior->params && default_count(d) > default_count(*prior)) {
        *prior = d;
        changed = true;
      }
    };
    auto add = [&](bool at_instantiation) {
      auto associations = adl_associations(args, at_instantiation, !dependent_call);
      for (EntityId ns : associations.namespaces)
        for (const ast::DeclSema& d : q_->adl_functions(ns, name, at_instantiation)) add_candidate(d);
      auto add_friend = [&](DeclId f) {
        if (auto d = q_->decl_sema(f); d.entity != EntityId::none) add_candidate(d);
      };
      // [basic.lookup.argdep]/4: a reachable associated class contributes the friends it
      // declared, even when their namespace has yet to bind the name for ordinary lookup.
      for (EntityId cls : associations.classes) {
        if (const ClassRec* rec = q_->adl_class(cls, at_instantiation)) {
          for (const ast::MemberRec& f : rec->adl_friends)
            if (w_->entities.rec(f.entity).name == name) add_friend(f.decl);
        } else if (!at_instantiation) {
          for (DeclId f : q_->earlier_adl_friends(cls, name)) add_friend(f);
        }
      }
    };
    add(false);
    if (dependent_call) add(true);
    if (!changed) return callee;
    ExprInfo info{builtin(Builtin::overload_set), ValueCategory::lvalue, 0};
    Ex r = node(ast::DeclRef{info, set.front().entity}, callee.begin, callee.end, info);
    if (auto ex = explicit_args_.find(std::to_underlying(callee.id)); ex != explicit_args_.end())
      explicit_args_[std::to_underlying(r.id)] = ex->second;
    overloads_[std::to_underlying(r.id)] = std::move(set);
    return r;
  }

  // The synthesized begin/end calls in [stmt.ranged] start with no ordinary candidates.
  Ex adl_call(NameId name, Ex arg, u32 begin, u32 end, u32 lookup_at) {
    ExprInfo info{builtin(Builtin::dependent), ValueCategory::lvalue, 0};
    Ex callee = node(ast::DepName{info, name}, begin, end, info);
    return call(callee, {arg}, end, end, lookup_at);
  }

  // A braced-init-list as a call's argument: a placeholder, list-initializing its parameter once
  // the callee is known (`call_args`).
  // The argument a parameter's default argument gives a call, positioned at the call's `)`: its
  // tree, analysed at the declaration ([dcl.fct.default]/5). One of a template that depends on its
  // parameters would be instantiated for the call ([temp.inst]/12), which is not supported yet.
  std::optional<Ex> default_argument(NodeId d, u32 at_) {
    NodeId arg = ins().ast.get<ast::DefaultArg>(d).arg.id;
    if (ast::kind_of(arg) == ast::Nodes::index_of<ast::Dependent>()) {
      diag(at_, Diag::default_arg_dependent);
      return std::nullopt;
    }
    ExprInfo info = ins().ast.get<ast::DefaultArg>(d).info;
    return Ex{b_->adopt(d), at_, at_, info};
  }

  Ex braced_argument(std::unique_ptr<Braced> b) {
    // The placeholder carries the list's dependency for [temp.dep] and [temp.dep.expr].
    const bool type_dep = type_dependent(*b);
    ExprInfo info{builtin(type_dep ? Builtin::dependent : Builtin::void_), ValueCategory::prvalue,
                  type_dep || dependent(*b) ? expr_flag::dependent : u8(0)};
    Ex r = node(ast::ValueInit{info}, b->begin, b->end, info);
    braced_args_[std::to_underlying(r.id)] = std::move(b);
    return r;
  }
  const Braced* braced_list(const Ex& e) const {
    if (!e.ok()) return nullptr;
    auto it = braced_args_.find(std::to_underlying(e.id));
    return it == braced_args_.end() ? nullptr : it->second.get();
  }
  bool is_braced(const Ex& e) const { return braced_list(e) != nullptr; }

  // [dcl.init.list]: the compiler forms an array of const elements and initializes the
  // list from that array. libycxx supplies a private pointer/size constructor for this.
  std::optional<Ex> initializer_list_argument(QualType t, const Braced& list) {
    const EntityRec specialization = w_->entities.rec(types().class_of(t));
    if (specialization.templ == EntityId::none || specialization.args.size() != 1 ||
        specialization.args[0].kind != ast::TemplateArgKind::type)
      return std::nullopt;
    const EntityRec primary = w_->entities.rec(specialization.templ);
    if (primary.name != ins().names.intern("initializer_list") || primary.scope == EntityId::none)
      return std::nullopt;
    const EntityRec ns = w_->entities.rec(primary.scope);
    if (ns.kind != EntityKind::namespace_ || ns.scope != EntityId::none || ns.name != ins().names.intern("std"))
      return std::nullopt;
    const QualType element = types().canonical(specialization.args[0].type);
    if (element != builtin(Builtin::int_)) return std::nullopt;

    const QualType const_element = types().add_cv(element, Cv::const_);
    const QualType pointer = types().pointer(const_element);
    const QualType size_type = builtin(w_->target.size_type);
    const ClassRec* record = class_rec(types().class_of(t));
    if (!record) return std::nullopt;
    for (const MemberDeclRec& member : record->members) {
      if (!member.is_ctor || member.is_template || member.is_deleted) continue;
      const TypeRec fn = types().rec(member.type);
      if (fn.variadic || fn.params.size() != 2 || types().canonical(fn.params[0]) != pointer ||
          types().canonical(fn.params[1]) != size_type)
        continue;
      const EntityId constructor = member.entity;
      const QualType array = types().array(const_element, list.clauses.size());
      Ex backing = list_init(array, list);
      if (!backing.ok()) return backing;
      Ex first = rvalue(materialize(backing, array, false));
      Ex size = literal(size_type, w_->values.integer(size_type, list.clauses.size()),
                        ValueCategory::prvalue, list.begin, list.end);
      ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
      const std::array args{at(first, list.begin), at(size, list.begin)};
      return node(ast::CtorCall{info, constructor, 0, b_->list(args)}, list.begin, list.end, info);
    }
    return std::nullopt;
  }

  // A class reference parameter binds directly to a single reference-related expression.
  // Otherwise the list initializes a prvalue of the referenced class ([dcl.init.list]); the
  // materialized object lives until the end of the call's full-expression ([class.temporary]).
  Ex braced_parameter(QualType parameter, const Braced& list) {
    parameter = Types::unqualified(parameter);
    if (!types().is_reference(parameter) || !types().is_class(types().referee(parameter)) ||
        types().is_dependent(parameter) || dependent(list))
      return list_init(parameter, list);
    QualType t = types().referee(parameter);
    if (list.clauses.size() == 1) {
      const Braced::Clause& c = list.clauses.front();
      if (!c.list && !c.designated && c.expr.ok() && types().is_class(c.expr.type())) {
        int paths = 0;
        if (same_unqualified(c.expr.type(), t) || base_path(types().class_of(c.expr.type()), types().class_of(t), paths))
          return bind_reference(c.expr, parameter, c.begin);
      }
    }
    if (!list.clauses.empty() && !types().rec(parameter).rvalue_ref && object_cv(t) == Cv::const_) {
      if (auto init = initializer_list_argument(t, list)) return bind_reference(*init, parameter, list.begin);
    }
    return bind_reference(list_init(Types::unqualified(t), list), parameter, list.begin);
  }

  // The arguments of a call to a function of type `fr`, each copy-initializing its parameter.
  // A parameter without an argument takes its default argument ([dcl.fct.default], [expr.call]/5).
  std::optional<std::vector<Child>> call_args(const TypeRec& fr, std::span<const Ex> given, u32 begin, u32 paren_at,
                                              std::span<const NodeId> defaults = {}) {
    if (unevaluated == 0 && !require_supported_virtual_signature(fr, paren_at)) return std::nullopt;
    std::vector<QualType> params = fr.params;  // copied: converting may intern new types
    std::vector<Ex> args(given.begin(), given.end());
    for (std::size_t i = args.size(); i < params.size() && i < defaults.size() && defaults[i] != NodeId::none; ++i) {
      auto a = default_argument(defaults[i], paren_at);
      if (!a) return std::nullopt;
      args.push_back(*a);
    }
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
      const Braced* list = braced_list(args[i]);
      if (list && i >= params.size()) {
        diag(args[i].begin, Diag::not_supported, lex::Tk::l_brace);  // [expr.call]/12: no list for an ellipsis
        return std::nullopt;
      }
      Ex a = list               ? braced_parameter(params[i], *list)
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

  // [expr.call]/12: an argument matched by the ellipsis gets the lvalue conversions, the integral
  // promotions and the floating-point promotion; std::nullptr_t becomes void*; void is no value.
  Ex variadic_argument(Ex e) {
    e = promote(e);
    if (!e.ok()) return e;
    if (types().is(e.type(), Builtin::float_)) return convert_node(e, ast::ConversionKind::floating, builtin(Builtin::double_));
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
    if (!require_supported_virtual_layout(types().pointee(a.type()), at_)) return {NodeId::none, begin, end};
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
  std::vector<std::pair<EntityId, NameId>> statics_;  // the static locals so far: (function, name)
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
    u32 ctx = 0;  // the context whose locals they are: the function's, or a lambda's
  };
  std::vector<ParamPack> param_packs_;
  std::vector<std::optional<ConstId>> local_values_;  // by local
  std::vector<bool> initialized_;                     // by local (`set_initialized`)
  std::vector<std::pair<std::size_t, u32>> barriers_; // (scope depth, id), `jump_barrier`
  u32 next_barrier_ = 0;
  struct LabelPoint {
    NameId name;
    u32 at;
    std::vector<u32> initialized;  // in scope at the label, or at the `goto`
  };
  std::vector<LabelPoint> labels_, gotos_;
  bool has_immediate_addresses_ = false;
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
    EntityId naming = EntityId::none;  // the class they were named in ([class.access.base]/5)
  };
  std::map<u32, Bound> bound_;                        // by draft bound-member Member
  std::set<u32> deleted_;                             // deleted functions named so far
  std::map<u32, std::vector<NodeId>> defaults_;       // by a function's DeclRef: the default arguments its lookup found
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
  QualType declared_return_{};
  bool return_deduced_ = false;
  bool return_seen_ = false;
  std::vector<LambdaFrame> lambdas_;  // outermost first
  std::vector<std::pair<EntityId, Fp128>> completed_closures_;  // (enclosing entity, signature)
  std::map<u32, QualType> closure_calls_;  // by operator() entity: its type, the return type deduced
  std::map<u32, GenericClosure> generic_closures_;  // by operator() entity: this part's generic lambdas
  std::map<u32, ConstId> const_captures_;  // by draft Member naming a captured constant: its value
};

}  // namespace ycxx::sema
