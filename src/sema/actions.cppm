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
import ycxx.ir;
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
  struct CapturedUsing {
    NameId name{};
    std::vector<ast::DeclSema> decls;
    std::optional<std::size_t> type_index;
  };
  std::vector<std::pair<NameId, QualType>> aliases;
  std::vector<CapturedUsing> usings;
  bool erroneous = false;  // its definition was diagnosed: its specializations say nothing more
  // Set for a lambda outside every function body: the part it is in, which its specializations
  // are analysed in, and the class it is in, if any. `body_begin` is an offset in that part.
  struct Part {
    ast::ItemId item{};
    u32 begin = 0;  // the part's first token in the item
    EntityId lookup_class = EntityId::none;
  };
  std::optional<Part> part{};
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

class Sema;
struct Ex;

struct ConstantCallEvaluation {
  std::optional<ConstId> value;
  u64 reason = reason_unsupported;
  bool diagnosed = false;
};
// What naming an alias template's specialization reads again: immediate calls its type retains or
// may erase, checks of a dependent array's rank, or a type with a dependent operand that has no
// representation (`decltype(f<T>())` of a deduced return type), known only once the arguments are.
struct AliasCallPattern {
  enum class Kind { none, retained, may_erase, unrepresented } kind = Kind::none;
  bool class_result = false;
  bool rank_checks = false;
  bool qualified_type_arguments = false;
  bool closures = false;  // its type-id makes closure types, each specialization its own (#1549)
};

struct PresumedSource {
  std::string_view file;
  u32 line = 0;
  u32 column = 0;
};

// What sema asks of its context (§B1.4, §B3). In the engine these are `visible_decls(::, name,
// at item)` and fetches of `decl_sema`.
// What the members of a local class see of the block that encloses its definition, as of its
// class-head (docs/design/local-classes.md): the block's names, innermost first and one per name;
// the class whose scope follows the block's ([basic.lookup.unqual]); and, for a class local to a
// member function of a local class, that class's own enclosing block.
struct OuterBlock {
  struct Name {
    enum class Kind : u8 { local, type, using_, namespace_alias };
    Kind kind = Kind::local;
    NameId name{};
    ast::LocalRec local{};
    std::optional<ConstId> value;  // a constant's
    QualType type{};
    std::vector<ast::DeclSema> decls;
    EntityId ns = EntityId::none;
    bool type_alias = false;
    bool type_template = false;
  };
  EntityId function = EntityId::none;  // the enclosing function, or lambda's function call operator
  EntityId cls = EntityId::none;       // the class the function is a member of, if any
  std::vector<Name> names;
  std::vector<EntityId> using_enums;  // innermost first
  std::vector<EntityId> directives;
  std::vector<std::vector<ast::TemplateParamSema>> template_scopes;
  Bindings template_args;
  bool templated = false;  // in a template's definition: the class is a templated entity
  std::shared_ptr<const OuterBlock> outer;
};
// A deduction-guide ([temp.deduct.guide]) as class template argument deduction reads it: a
// function declaration, template or not, returning a specialization of its class template, and
// which of its parameters have default arguments. Its lists hold no nodes.
struct DeductionGuide {
  ast::DeclSema decl;
  // BOOTSTRAP: std::vector<bool>: GCC 16.2 may fail to read an exported struct holding one from a
  // module (#1705); a precaution here, not a confirmed failure. Intended: `std::vector<bool> defaulted;`
  std::vector<std::uint8_t> defaulted;
};

class Questions {
public:
  virtual bool exceptions_enabled() const { return true; }
  virtual bool rtti_enabled() const { return true; }
  virtual ast::Compatibility compatibility() const { return ast::Compatibility::spec; }
  // The declarations of `name` in the global namespace visible at the current point: those that
  // precede the current item, and, inside a function body, the item's own (§B2, [basic.scope.pdecl]).
  // `directives` are the namespaces block-scope using-directives nominate ([namespace.udir]): their
  // names join the lookup where [namespace.udir]/2 puts them.
  virtual std::vector<DeclId> visible_decls(NameId name, std::span<const EntityId> directives = {},
                                            bool immediate_call = false) = 0;
  virtual std::vector<ast::DeclSema> qualifier_decls(NameId, std::optional<EntityId>,
                                                     std::span<const EntityId>) { return {}; }
  virtual ast::DeclSema decl_sema(DeclId d) = 0;
  // The written in-class declaration of a member from this unit's class record.
  virtual std::optional<ast::DeclSema> written_member(EntityId) { return std::nullopt; }
  // The declaration a class template specialization made of a friend function it defines (#259).
  virtual std::optional<ast::DeclSema> class_friend(EntityId) { return std::nullopt; }
  virtual bool peer_pure_dispatch(EntityId) { return false; }
  virtual void use_typed_dynamic_cast() {}
  virtual u32 presumed_line(engine::Loc) { return 0; }
  virtual PresumedSource presumed_source(engine::Loc) { return {}; }
  virtual bool in_default_member_initializer() const { return false; }
  // The block around the local class whose member is analysed here ([class.local]), if any.
  virtual std::shared_ptr<const OuterBlock> local_outer_block() { return nullptr; }
  // A use of a global allocation function that may lack a written declaration in the unit.
  // An aligned signature also brings its parameter's enum shape into the unit.
  virtual void use_allocation_function(EntityId, bool = false) {}
  // A call of a C library function a builtin lowers to (`World::string_function`).
  virtual void use_library_function(EntityId) {}
  // The static object denoted by a class-valued template parameter ([temp.param]).
  virtual void use_template_parameter_object(EntityId) {}
  virtual bool plain_class_template_type(EntityId) { return false; }
  // Check the selected implicitly declared copy assignment against its subobjects.
  virtual CopyAssignment require_copy_assignment(EntityId) { return CopyAssignment::unsupported; }
  // [except.spec]: whether the implicit exception specification of `f`, a special member defaulted
  // on its first declaration without a noexcept-specifier, is non-throwing; nothing for another function.
  virtual std::optional<bool> defaulted_nonthrowing(EntityId) { return std::nullopt; }
  // What a constant evaluation fetches (§B7): `function_ir` of a function defined before the
  // current point ([expr.const]), and a namespace-scope variable as the interpreter sees it.
  virtual interp::Callee function_ir(EntityId f, bool& diagnosed_failure) = 0;
  virtual std::optional<interp::GlobalVar> global(EntityId v) = 0;
  // A variable template's required definition has already reported an error.
  virtual bool variable_instantiation_failed(EntityId) { return false; }
  virtual bool is_immediate_function(EntityId) { return false; }
  virtual bool is_constexpr(EntityId f) = 0;  // a constexpr or consteval function
  // Whether class `c` is complete at the current point ([class.mem.general]): its definition
  // precedes the point and does not enclose it.
  virtual bool is_complete(EntityId c) = 0;
  // Whether a class of the unit can need virtual support: the unit says `virtual` somewhere. A
  // fact of its tokens, so a check that skips classes without it does not depend on which classes
  // were analysed first (#947).
  virtual bool may_have_virtual_layouts() { return true; }
  // An omitted field's initializer, analysed at its declaration into the requesting builder.
  // `NodeId::none` records a diagnosed error. No answer means this context cannot replay it.
  // A required base selects the call-free parenthesized aggregate form. The fallback returns
  // the literal from folding a template specialization's scalar initializer.
  virtual std::optional<NodeId> replay_default_member(Sema&, EntityId, EntityId = EntityId::none, bool = false) {
    return std::nullopt;
  }
  virtual std::optional<NodeId> replay_function_default(Sema&, const ast::DefaultArg&, EntityId, QualType, u32) {
    return std::nullopt;
  }
  // Whether the default argument of parameter `parameter` of function `f` still waits for its
  // class to be complete ([class.mem.general]), so a call before then cannot take it.
  virtual bool defaults_pending(EntityId, std::size_t) { return false; }
  // The effective specification of a written trivial destructor whose class is being completed.
  virtual std::optional<bool> replay_destructor_exception(Sema&, EntityId, u32) { return std::nullopt; }
  // A selected specialization's exception specification ([except.spec]/13): of a member template,
  // of a member of a class template specialization, or of a function template.
  virtual std::optional<QualType> replay_member_exception(Sema&, const ast::DeclSema&, EntityId, QualType type) {
    return type;
  }
  // Whether function (or function template) `f` has a noexcept condition kept for its
  // specializations' replays.
  virtual bool deferred_exception(EntityId) { return false; }
  // Whether a special member's provisional type still lacks its completed specification.
  virtual bool unresolved_special_member_exception(EntityId) { return false; }
  // A concrete source query keeps its original position if its tokens are read again.
  virtual bool is_complete_at_source(EntityId c) { return is_complete(c); }
  // Whether a class's layout, or a base's or stored member's, depends on attributes or packing
  // the layout does not apply (ClassRec::unapplied_layout).
  virtual bool class_layout_uncertain(EntityId /*cls*/) { return false; }
  // The classes and enumerations whose layout written annotations may change, aliases included.
  virtual std::set<EntityId> uncertain_object_layouts() { return {}; }
  virtual engine::Loc source_loc(engine::Loc loc, u32) { return loc; }
  // Only a base or initializer with an owning replay can retain this dependent pattern.
  virtual bool in_type_trait_pattern() const { return false; }
  virtual bool in_type_trait_replay() const { return false; }
  virtual void record_dependent_type_trait_operand(engine::Loc) {}
  virtual bool replaying_type_trait_operand(engine::Loc) { return false; }
  virtual std::optional<bool> type_trait_at_demand(EntityId, engine::Loc) { return std::nullopt; }
  virtual bool type_trait_instantiation_failed(EntityId) { return false; }
  // Inside the enumerator list of an enumeration this unit is defining: its record so far, which
  // is published only after the list (#156).
  virtual const EnumRec* defining_enum(EntityId) { return nullptr; }
  // A scoped member enum's declaration supplies its fixed type. Looking up its enumerators
  // requires the definition ([temp.inst]). False means its instantiation was diagnosed.
  virtual bool require_enum_definition(EntityId) { return true; }
  // Scopedness is fixed even when the enum's enumerator list is still open.
  virtual std::optional<bool> defining_enum_is_scoped(EntityId e) {
    if (const EnumRec* record = defining_enum(e)) return record->scoped;
    return std::nullopt;
  }
  // A fixed enumeration is complete even before its enumerator list ends ([dcl.enum]).
  virtual std::optional<bool> defining_enum_is_fixed(EntityId e) {
    if (const EnumRec* record = defining_enum(e)) return record->fixed;
    return std::nullopt;
  }
  // This unit's class and enumeration records (#115).
  virtual const Classes& classes() = 0;
  // The constant a variable usable in constant expressions holds ([expr.const]), if it is known.
  virtual std::optional<ConstId> constant_value(EntityId v) = 0;
  virtual ConstantCallEvaluation constant_call(Sema&, const Ex&) { return {}; }
  virtual AliasCallPattern alias_call_pattern(EntityId) { return {}; }
  virtual std::optional<QualType> replay_alias_calls(Sema&, const ast::DeclSema&, const Bindings&) {
    return std::nullopt;
  }
  // In a member declaration of a class being defined: the member named `name` declared before it
  // ([basic.scope.class]; the class's member list is not complete yet). An expression can also
  // request the function declarations of that name in source order.
  virtual std::optional<ast::DeclSema> earlier_member(NameId name, bool type_only = false,
                                                     std::vector<ast::DeclSema>* functions = nullptr) = 0;
  // In a member of a nested class ([class.nest]): the member named `name` that the enclosing
  // class `cls`, still being defined, declares before the nested class.
  virtual std::optional<ast::DeclSema> enclosing_earlier_member(EntityId /*cls*/, NameId /*name*/, bool /*type_only*/,
                                                               std::vector<ast::DeclSema>* /*functions*/ = nullptr) {
    return std::nullopt;
  }
  // The direct bases of class `c` while it is being defined (its base-clause), none otherwise.
  virtual std::vector<EntityId> defining_bases(EntityId) { return {}; }
  virtual std::span<const BaseRec> defining_base_records(EntityId) { return {}; }
  // The direct bases of a specialization whose members are being analysed (including those
  // dependent in the pattern, for access checks during instantiation).
  virtual std::span<const BaseRec> instantiating_base_records(EntityId) { return {}; }
  // Whether a using-declaration of class `c` names `name` in a dependent base: in its template
  // definition, a member of an unknown specialization ([namespace.udecl], [temp.dep.type]/7).
  virtual bool dependent_member_using(EntityId, NameId) { return false; }
  // Whether class `c` is an explicit specialization, which has no template-head of its own
  // ([temp.expl.spec]).
  virtual bool explicit_specialization(EntityId) { return false; }
  // Namespaces ([basic.namespace]), the global one being `EntityId::none`: the namespace (or
  // alias) unqualified lookup finds for `name`, and qualified lookup of `name` in namespace `ns`
  // ([namespace.qual]).
  virtual std::optional<EntityId> namespace_named(NameId name, std::span<const EntityId> directives = {}) = 0;
  virtual std::vector<ast::DeclSema> namespace_members(EntityId ns, NameId name, bool type_only = false,
                                                      bool direct_only = false, bool immediate_call = false) = 0;
  // [basic.lookup.argdep]: functions in one associated namespace's inline namespace set, and
  // a class's record if complete there. Ask at the lexical point, and for a dependent call
  // also at the point of instantiation ([temp.dep.candidate]).
  virtual std::vector<ast::DeclSema> adl_functions(EntityId ns, NameId name, bool at_instantiation) = 0;
  virtual const ClassRec* adl_class(EntityId c, bool at_instantiation) = 0;
  // Earlier friend declarations in a class whose member list is still being read.
  virtual std::vector<DeclId> earlier_adl_friends(EntityId, NameId) { return {}; }
  // The declaration of a class's friend `f`: a class template specialization's own (#259).
  virtual ast::DeclSema friend_decl(const ast::MemberRec& f) { return decl_sema(f.decl); }
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
  // The unit's normal forms of concept-ids (`Sema::concept_constraint`), keyed by the concept and
  // its interned arguments; none where nothing keeps them.
  virtual std::map<std::pair<u32, u32>, ConstId>* concept_normal_forms() { return nullptr; }
  virtual void record_nested_constraint_operand(u32 /*begin*/, u32 /*end*/) {}
  virtual std::optional<u32> nested_constraint_operand_end(u32 /*begin*/) { return std::nullopt; }
  // Whether checking normal form `c`'s satisfaction raised a hard error (an atom not of type
  // bool, not constant, or not supported): the expression that asked is then invalid.
  virtual bool satisfaction_failed(ConstId /*c*/) { return false; }
  // A value-dependent explicit-specifier's condition, atom `c` with its mapping substituted
  // ([dcl.fct.spec]): whether the specialization is explicit; none when substitution into it
  // fails, which fails deduction ([temp.deduct.general]).
  virtual std::optional<bool> explicit_value(ConstId /*c*/) { return false; }
  // A class template's static data member whose initializer is value-dependent; and a use of a
  // specialization's static data member, which instantiates its initializer ([temp.inst]/3).
  // The declaration of template `t` here: its head and, for an alias template, its type.
  virtual std::optional<ast::DeclSema> template_decl(EntityId /*t*/) { return std::nullopt; }
  // The deduction-guides for class template `t` declared before the current point
  // ([temp.deduct.guide]): function declarations, templates or not, whose return type names a
  // specialization of `t`.
  virtual std::vector<DeductionGuide> deduction_guides(EntityId /*t*/) { return {}; }
  virtual std::optional<ast::DeclSema> replay_function_decl(EntityId function) { return template_decl(function); }
  virtual bool value_dependent_member(EntityId /*v*/) { return false; }
  // False when its initializer was in error, which was reported: what uses it says nothing more.
  virtual bool use_static_member(EntityId /*v*/, bool /*unevaluated*/ = false) { return true; }
  // A static local declared ([stmt.dcl]/3), for constant evaluation's questions about it
  // (`global`): its type, and its constant if constant-initialized.
  virtual void static_local(EntityId /*v*/, QualType /*type*/, std::optional<ConstId> /*value*/) {}
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
  // Publishes a closure type's record, checking its captures in the lambda's lexical context.
  virtual const ClassRec& publish_class(EntityId c, ClassRec r, Sema& context) = 0;
  // In an instantiation: the pack expansions the definition of what it re-analyses recorded.
  virtual const std::vector<Expansion>* pattern_expansions() { return nullptr; }
  // The namespace the item being analysed is in: what owns the closure types of lambdas in its
  // unevaluated operands outside every class and declaration that owns them (#1549).
  virtual EntityId enclosing_namespace() { return EntityId::none; }
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

enum class Construction { direct, direct_paren_aggregate, copy, direct_list, copy_list, default_init };
enum class ConstructorDestination { object, delegation, polymorphic_base, lambda_result };

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
inline constexpr u8 deferred_decay_copy = 8;  // a decay-copy still needs function body replay
inline constexpr u8 type_dependent = 16;  // the producer proved dependence even without a structural type
inline constexpr u8 dependent_integral_arithmetic = 32;  // integral arithmetic depending on value parameters
inline constexpr u8 structural_projection = 64;  // a class parameter field in a member alias argument
static_assert((ast::Conditional::unchecked_class_copy &
               (dependent | immediate_address | unrepresented_value | deferred_decay_copy | type_dependent |
                dependent_integral_arithmetic | structural_projection)) == 0);
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

// Only this fixed set of declarations can consume a peer-owned virtual table.
bool native_virtual_declaration(const MemberDeclRec& member, const Types& types, auto&& written_member) {
  if (member.is_pure) return false;
  auto integer = [&](QualType type) {
    const auto builtin = types.builtin_of(type);
    return builtin && ast::is_integral(*builtin) && types.target()[*builtin].size <= 8;
  };
  const auto declared = written_member(member.entity);
  if (!declared || (declared->flags & (decl_flag::definition | decl_flag::inline_ | decl_flag::deleted))) return false;
  const auto type = types.rec(member.type);
  return !type.variadic && type.fn_ref == ast::RefQual::none &&
         (type.fn_cv == Cv::none || type.fn_cv == Cv::const_) && type.params.size() < 32 &&
         integer(type.inner) && std::ranges::all_of(type.params, integer);
}

bool native_virtual_class(const ClassRec& record) {
  return record.polymorphic && !record.virtual_destructor &&
         !record.user_ctors && record.destructor == EntityId::none;
}

bool native_virtual_root_declarations(const ClassRec& record, const Types& types, auto&& written_member) {
  if (!native_virtual_class(record) || !record.bases.empty()) return false;
  return std::ranges::all_of(record.members, [&](const MemberDeclRec& member) {
    // Class completion also records an implicit copy assignment, even for opaque classes.
    if (!member.is_virtual)
      return member.copy_assignment != CopyAssignment::none && !member.explicit_defaulted_assignment;
    return native_virtual_declaration(member, types, written_member);
  });
}

// [Itanium ABI §2.5.2]: root virtuals occupy slots in declaration order. A noncovariant
// override shares its primary base's slot, and refers to the declaration that introduced it.
std::optional<u32> native_virtual_root_slot(const ClassRec& root, EntityId function) {
  u32 slot = 0;
  for (const auto& member : root.members) {
    if (!member.is_virtual) continue;
    if (member.entity == function) return slot;
    ++slot;
  }
  return std::nullopt;
}

bool native_virtual_nonkey_member(const ClassRec& root, EntityId function) {
  if (root.peer_virtual_layout != PeerVirtualLayout::root) return false;
  // The certified root has only ordinary non-pure, non-inline virtual declarations.
  // Its first slot is the Itanium key function and carries table ownership.
  const auto slot = native_virtual_root_slot(root, function);
  return slot && *slot != 0;
}

// The first root table producer uses the existing native root layout. Its two non-pure,
// non-inline declarations occupy the primary table in order, starting with the key function.
std::optional<std::array<EntityId, 2>> native_virtual_table_slots(const ClassRec& root) {
  if (root.peer_virtual_layout != PeerVirtualLayout::root || root.fields.size() != 1 ||
      root.access_of(root.fields.front().entity) != ast::Access::public_ || !root.friends.empty())
    return std::nullopt;
  std::array<EntityId, 2> slots{};
  std::size_t count = 0;
  for (const auto& member : root.members) {
    if (!member.is_virtual) continue;
    if (count == slots.size()) return std::nullopt;
    slots[count++] = member.entity;
  }
  if (count != slots.size()) return std::nullopt;
  return slots;
}

EntityId virtual_table_entity(World& world, EntityId cls) {
  return world.entities.entity({EntityKind::virtual_table, world.ins.names.intern("vtable"),
                                world.types.builtin(Builtin::unsigned_char), LangLinkage::cxx,
                                ast::Linkage::external, {}, cls});
}

// This profile has only one directly declared virtual. An inherited slot alone
// does not give a derived class its own key function (Itanium ABI §5.2.3).
EntityId borrowed_dynamic_cast_key(const ClassRec& record, const Types& types) {
  if (record.peer_virtual_layout == PeerVirtualLayout::none || !record.fields.empty() || !record.friends.empty())
    return EntityId::none;
  const auto key = std::ranges::find(record.members, true, &MemberDeclRec::is_virtual);
  if (key == record.members.end() || key->is_pure || record.access_of(key->entity) != ast::Access::public_ ||
      std::ranges::find(std::next(key), record.members.end(), true, &MemberDeclRec::is_virtual) != record.members.end())
    return EntityId::none;
  const auto type = types.rec(key->type);
  if (!types.is(type.inner, Builtin::int_) || !type.params.empty() || type.fn_cv != Cv::none || type.fn_noexcept)
    return EntityId::none;
  return key->entity;
}

EntityId type_info_entity(World& world, EntityId cls) {
  return world.entities.entity({EntityKind::type_info, world.ins.names.intern("typeinfo"),
                                world.types.builtin(Builtin::unsigned_char), LangLinkage::cxx,
                                ast::Linkage::external, {}, cls});
}

// Itanium C++ ABI §3.2.6: the entry of a pure virtual function in a table.
EntityId pure_virtual_handler(World& world) {
  const auto lang = LangLinkage::c;
  const auto type = world.types.function(world.types.builtin(Builtin::void_), {}, false, lang);
  return world.entities.entity({EntityKind::function, world.ins.names.intern("__cxa_pure_virtual"), type, lang});
}

EntityId dynamic_cast_runtime(World& world) {
  const QualType v = world.types.builtin(Builtin::void_);
  const QualType p = world.types.pointer(world.types.add_cv(v, Cv::const_));
  const QualType params[] = {p, p, p, world.types.builtin(world.target.ptrdiff_type)};
  const auto lang = LangLinkage::c;
  const auto type = world.types.function(world.types.pointer(v), params, false, lang);
  return world.entities.entity({EntityKind::function, world.ins.names.intern("__dynamic_cast"), type, lang});
}

bool native_virtual_derived_declarations(const ClassRec& record, const Classes& classes, const Types& types,
                                        auto&& written_member) {
  if (!native_virtual_class(record) || record.bases.size() != 1 || record.fields.size() > 1) return false;
  const BaseRec& edge = record.bases.front();
  if (edge.access != ast::Access::public_ || edge.dependent_in_pattern) return false;
  const ClassRec* base = classes.find(edge.cls);
  if (!base || base->peer_virtual_layout != PeerVirtualLayout::root ||
      !native_virtual_root_declarations(*base, types, written_member)) return false;
  bool has_override = false;
  for (const auto& member : record.members) {
    if (!member.is_virtual && member.copy_assignment != CopyAssignment::none &&
        !member.explicit_defaulted_assignment && !member.is_deleted) continue;
    if (!member.is_virtual || has_override || member.overridden_virtual == EntityId::none ||
        !native_virtual_root_slot(*base, member.overridden_virtual) ||
        !native_virtual_declaration(member, types, written_member)) return false;
    has_override = true;
  }
  return true;
}

bool native_virtual_consumer(const ClassRec& record, const Classes& classes, const Types& types,
                            auto&& written_member) {
  if (record.peer_virtual_layout == PeerVirtualLayout::primary_base)
    return native_virtual_derived_declarations(record, classes, types, written_member);
  return (record.fields.empty() || record.peer_virtual_layout == PeerVirtualLayout::root) &&
         native_virtual_root_declarations(record, types, written_member);
}

// Flags of `ClosureDef::flags`.
namespace closure_flag {
inline constexpr u16 mutable_ = 1;      // its function call operator is not const
inline constexpr u16 captureless = 2;   // no lambda-capture: it converts to a pointer to function
inline constexpr u16 pattern = 4;       // in a template's or a generic lambda's definition: not code
inline constexpr u16 polymorphic_result = 8;  // checked return, without native or interpreter ABI support
inline constexpr u16 generic_invoker = 16;   // a synthesized static invoker specialization
inline constexpr u16 generated_conversion = 32;
inline constexpr u16 static_call = 64;
}

// What a cell demands of the program besides its value (query-engine.md §B1.4): a template
// specialization whose definition is required ([temp.inst]). A variable name carries its
// substituted declared type so a potentially-constant variable can be instantiated at its use.
struct Demand {
  EntityId spec;
  ast::Loc at;
  std::optional<QualType> variable_type = std::nullopt;
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
    diag_at(q_->source_loc({std::to_underlying(item_), loc_base_ + at}, at), d, args...);
  }
  template <class... A>
  void diag_at(engine::Loc loc, Diag d, const A&... args) {
    engine::Diagnostic x{loc, diag_id(d), {arg(args)...}};
    // The elements of an expansion say an equal diagnostic once (ast.md §6.4).
    if (!frames_.empty() && std::ranges::contains(*diags_, x)) return;
    diags_->push_back(std::move(x));
  }
  std::size_t diag_count() const { return diags_->size(); }
  // A required constant call can diagnose its instantiated body in a separate cell.
  std::size_t diagnosed_constant_calls = 0;
  std::vector<engine::Diagnostic>& diagnostics() { return *diags_; }
  bool has_error_since(std::size_t begin) const {
    return std::ranges::any_of(std::span(*diags_).subspan(begin), [](const engine::Diagnostic& d) {
      auto kind = sema_diag_of(d.id);
      return kind && message_of(*kind).severity == lex::Severity::error;
    });
  }
  // Whether a diagnostic since `begin` says the tokens do not have the grammar's shape (an
  // expected or unexpected token), as opposed to a well-formedness rule: what decides a tentative
  // parse ([dcl.ambig.res]/2).
  bool has_syntax_error_since(std::size_t begin) const {
    return std::ranges::any_of(std::span(*diags_).subspan(begin), [](const engine::Diagnostic& d) {
      switch (sema_diag_of(d.id).value_or(Diag::not_supported)) {
      case Diag::expected_token: case Diag::expected_expression: case Diag::type_as_expression:
      case Diag::expected_statement: case Diag::expected_declaration: case Diag::expected_type_specifier:
      case Diag::expected_declarator: case Diag::expected_unqualified_id: case Diag::unexpected_token:
      case Diag::unexpected_identifier: case Diag::missing_type_specifier:
        return true;
      default:
        return false;
      }
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
  ExprInfo expression_info(NodeId id, bool draft = false) {
    const NodeId published = draft ? b_->adopted_node(id) : id;
    ExprInfo info{};
    ast::dispatch<ast::Nodes>(ast::kind_of(id), [&]<class T>(std::type_identity<T>) {
      if constexpr (requires(T value) { value.info; })
        info = published == NodeId::none ? b_->get<T>(id).info : ins().ast.get<T>(published).info;
    });
    return info;
  }

  QualType builtin(Builtin k) { return types().builtin(k); }

  // What constant evaluation answers for list-initialization's narrowing check ([dcl.init.list]);
  // set by the parser, which owns the evaluator.
  std::function<std::optional<ConstId>(Ex)> constant_of;
  // What constant evaluation answers for an immediate invocation ([expr.const]): its value, none
  // for a void call, or why it is not a constant expression. Set by the parser too.
  struct Evaluation {
    std::optional<ConstId> value;
    u64 reason = reason_unsupported;
    bool constant = false;
    bool function_instantiation_failure = false;
  };
  std::function<Evaluation(Ex)> evaluate_immediate;

  // ------------------------------------------------------------ classes ([class], [expr.ref])

  // A type-name naming a class, found by unqualified lookup ([basic.lookup.unqual]): none when
  // the name is not declared, or a variable or function hides it ([basic.scope.hiding]). With
  // `elaborated`, non-type declarations are ignored ([basic.lookup.elab]).
  // A typedef-name too, unless `aliases` is false (after a class-key, [dcl.type.elab]/2).
  std::optional<QualType> class_name(NameId name, bool elaborated = false, bool aliases = true,
                                    std::optional<u32> type_at = std::nullopt, bool search_locals = true,
                                    std::optional<EntityId>* qualifier_namespace = nullptr);
  // Operand lookup for an unqualified `using enum E;` ([enum.udecl]/1, CWG 2877): type-only in
  // spec/clang modes, and ordinary lookup (non-types hide types) in gcc mode.
  std::optional<QualType> using_enum_type(NameId name);
  static bool is_type(ast::DeclKind k) {
    return k == ast::DeclKind::class_ || k == ast::DeclKind::enum_ || k == ast::DeclKind::type_alias;
  }
  // The class that class `c` is a member of ([class.nest]), none at namespace scope.
  EntityId enclosing_class(EntityId c) const {
    if (c == EntityId::none) return EntityId::none;
    const EntityRec& r = w_->entities.rec(c);
    if (r.kind != EntityKind::class_ || r.templ != EntityId::none || r.scope == EntityId::none) return EntityId::none;
    const EntityRec& s = w_->entities.rec(r.scope);
    return s.kind == EntityKind::class_ ? r.scope : EntityId::none;
  }
  // Whether class `c` encloses the class whose member is being analysed ([class.nest]).
  bool encloses_current_class(EntityId c) const {
    for (EntityId k = enclosing_class(current_class); k != EntityId::none; k = enclosing_class(k))
      if (k == c) return true;
    return false;
  }
  // An incomplete enclosing class exposes its preceding declarations, then its bases.
  // A non-static member needs an object unless it is a data member in an unevaluated operand.
  std::optional<Ex> enclosing_prefix_value(EntityId cls, NameId name, u32 begin, u32 end, u32 name_at,
                                           bool unqualified, bool callee, std::optional<u32> address_at = {});
  static bool is_non_template_tag(const ast::DeclSema& decl) {
    return (decl.kind == ast::DeclKind::class_ || decl.kind == ast::DeclKind::enum_) &&
           !(decl.flags & decl_flag::template_);
  }

  bool names_direct_class(NameId name, QualType type) {
    if (std::ranges::contains(block_types_, name, &BlockType::name)) return false;
    const auto directly_named = class_name(name, /*elaborated=*/false, /*aliases=*/false);
    return directly_named && same_unqualified(*directly_named, type);
  }

  struct ClassBodyType {
    bool found = false;
    std::optional<QualType> type;
  };

  enum class ClassBodyLookup {
    unqualified, qualified, enum_qualifier, qualified_enum, before_template_parameter, class_prefix
  };

  // [class.pre], [class.member.lookup]: a class's injected name stops the search on
  // that branch. In a nonvirtual graph, distinct base branches cannot dominate one another.
  ClassBodyType class_body_type(EntityId naming, const ClassRec& root, NameId name, std::optional<u32> type_at,
                               ClassBodyLookup lookup = ClassBodyLookup::unqualified, bool type_only = false);

  ClassBodyType enclosing_prefix_type(EntityId cls, NameId name, std::optional<u32> at, bool type_only) {
    if (auto member = q_->enclosing_earlier_member(cls, name, type_only)) {
      const bool names_type = is_type(member->kind) && !(member->flags & decl_flag::template_);
      return {true, names_type ? std::optional{member->type} : std::nullopt};
    }
    return class_body_type(cls, ClassRec{}, name, at, ClassBodyLookup::class_prefix, type_only);
  }

  // A class template's nondependent bases are searched before the template parameter scope.
  std::optional<QualType> class_template_parameter_type(const ast::TemplateParamSema& parameter, NameId name, u32 at) {
    const auto before = diag_count();
    const auto hit = class_body_type(current_class, ClassRec{}, name, at, ClassBodyLookup::before_template_parameter);
    if (hit.found) {
      if (has_error_since(before)) return std::nullopt;
      if (!hit.type) diag(at, Diag::not_a_type, name);
      return hit.type;
    }
    if (auto argument = bound_arg(parameter)) return argument->type;
    return parameter.type;
  }
  // A typedef-name the implementation declares where nothing else does (docs/EXTENSIONS.md):
  // `__builtin_va_list`, the type used by the target's C headers and libycxx's `va_list`, and
  // `__int128_t` and `__uint128_t`, which the macOS SDK's `mach/arm/_structs.h` uses.
  std::optional<QualType> builtin_type_name(NameId name) {
    const std::string_view spelling = ins().names.spelling(name);
    if (spelling == "__builtin_va_list") return w_->builtin_va_list();
    if (spelling == "__int128_t") return builtin(Builtin::int128);
    if (spelling == "__uint128_t") return builtin(Builtin::unsigned_int128);
    return std::nullopt;
  }
  bool conflicts_with_imported_alias(std::span<const ast::DeclSema> imported, QualType type) {
    return std::ranges::any_of(imported, [&](const ast::DeclSema& decl) {
      return decl.kind == ast::DeclKind::type_alias &&
             ((decl.flags & decl_flag::template_) || types().canonical(decl.type) != types().canonical(type));
    });
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
        }) || std::ranges::any_of(block_usings_, [&](const BlockUsing& used) {
          return used.depth == depth && used.name == name && conflicts_with_imported_alias(used.decls, type);
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
    if (q_->earlier_member(name)) return true;
    const auto bases = class_prefix_bases(current_class);
    return !bases.empty() &&
           (static_initializer_has_own_name(name) || find_base_member(bases, name, /*unqualified=*/true));
  }

  // The class template unqualified lookup finds for `name`, which a `<` after it begins the
  // arguments of ([temp.names]/3); none if it finds anything else.
  std::optional<ast::DeclSema> class_template(NameId name) { return named_template(name, ast::DeclKind::class_); }
  // The class template `name` names where its name alone can be a placeholder for a deduced class
  // type ([dcl.type.class.deduct]); none for anything else, a template template parameter included.
  EntityId deducible_template(NameId name) {
    if (template_param(name)) return EntityId::none;
    auto c = class_template(name);
    return c ? c->entity : EntityId::none;
  }
  // A template-name that names a type with its arguments ([temp.names]): a class template, or an
  // alias template ([temp.alias]).
  std::optional<ast::DeclSema> type_template(NameId name) {
    if (auto block = block_type_template(name); block.found) return block.declaration;
    if (injected_class_template(name) != EntityId::none) return class_template(name);
    bool found_member = false;
    if (auto m = own_member_alias_template(name, found_member); found_member) return m;
    if (auto c = class_template(name)) return c;
    return named_template(name, ast::DeclKind::type_alias);
  }

  struct BlockTypeTemplate {
    bool found = false;
    std::optional<ast::DeclSema> declaration;
  };

  // A block declaration, including one introduced by a using-declaration, hides the class
  // and namespace scopes ([basic.lookup.unqual], [namespace.udecl]).
  BlockTypeTemplate block_type_template(NameId name) {
    for (std::size_t depth = scopes_.size(); depth-- > 0;) {
      const auto names_here = [=](const auto& binding) { return binding.depth == depth && binding.name == name; };
      if (std::ranges::contains(scopes_[depth], name, [](const auto& local) { return local.first; }) ||
          std::ranges::any_of(block_types_, names_here) || std::ranges::any_of(block_aliases_, names_here))
        return {true, std::nullopt};
      for (const auto& [scope, e] : using_enums_) {
        if (scope != depth) continue;
        if (const EnumRec* record = enum_rec(e); record && std::ranges::contains(record->enumerators, name, &EnumeratorRec::name))
          return {true, std::nullopt};
      }
      if (const auto used = std::ranges::find_if(block_usings_ | std::views::reverse, names_here);
          used != block_usings_.rend()) {
        if (used->type_index) {
          const auto& declaration = used->decls[*used->type_index];
          if (declaration.flags & decl_flag::template_)
            return {true, with_template_defaults(declaration, used->decls)};
        }
        return {true, std::nullopt};
      }
    }
    return {};
  }

  std::optional<ast::DeclSema> namespace_type_template(EntityId ns, NameId name) {
    if (auto c = namespace_class_template(ns, name)) return c;
    return namespace_template(ns, name, ast::DeclKind::type_alias);
  }
  // [basic.lookup.qual], [namespace.qual] apply ordinary lookup before a functional
  // conversion is known to name a type. Distinct declarations can be introduced by
  // different using-directives even when one of them is a type template.
  std::optional<ast::DeclSema> namespace_expression_type_template(EntityId ns, NameId name, u32 at) {
    const auto found = q_->namespace_members(ns, name);
    const auto type = std::ranges::find_if(found, [](const ast::DeclSema& d) {
      return (d.flags & decl_flag::template_) && (d.kind == ast::DeclKind::class_ || d.kind == ast::DeclKind::type_alias);
    });
    if (type == found.end()) return std::nullopt;
    if (std::ranges::any_of(found, [&](const ast::DeclSema& d) { return d.entity != type->entity; })) {
      diag(at, Diag::ambiguous_name, name);
      return std::nullopt;
    }
    return with_template_defaults(*type, found);
  }
  // The variable template unqualified lookup finds for `name` ([temp.pre]/1), likewise.
  std::optional<ast::DeclSema> variable_template(NameId name, std::optional<u32> at = std::nullopt,
                                                 bool initializer = false) {
    return named_template(name, ast::DeclKind::variable, at, initializer);
  }
  // The template whose name `name` is in the current class, or none. In a class template, or a
  // specialization of one, its injected-class-name followed by `<` is the template's name
  // ([temp.local]/1): `A<U>` in `A` names the specialization `A<U>`. The class's constructors
  // share the name but are not found by lookup ([class.ctor]/1), so they do not hide it. The only
  // member that can, a non-static data member of a class without a user-declared constructor
  // ([class.mem.general]), makes `A < 2` a comparison.
  EntityId injected_class_template(NameId name) {
    if (current_class == EntityId::none) return EntityId::none;
    const EntityRec& c = w_->entities.rec(current_class);
    if (c.templ == EntityId::none || c.name != name) return EntityId::none;
    if (const ClassRec* r = class_rec(current_class))
      return std::ranges::contains(r->fields, name, &FieldRec::name) ? EntityId::none : c.templ;
    if (auto m = q_->earlier_member(name); m && m->kind == ast::DeclKind::data_member) return EntityId::none;
    return c.templ;
  }

  // The template of kind `kind` unqualified lookup finds for `name`, with the default arguments of
  // every earlier declaration of it ([temp.param]/12).
  std::optional<ast::DeclSema> named_template(NameId name, ast::DeclKind kind,
                                             std::optional<u32> at = std::nullopt, bool initializer = false);

  // [temp.param] combines default arguments from prior declarations of the same template.
  ast::DeclSema with_template_defaults(ast::DeclSema found, std::span<const ast::DeclSema> decls) {
    std::vector<ast::TemplateParamSema> params = ins().seqs.elements(found.template_params);
    bool merged = false;
    for (const ast::DeclSema& d : decls) {
      if (d.entity != found.entity) continue;
      std::vector<ast::TemplateParamSema> other = ins().seqs.elements(d.template_params);
      for (std::size_t i = 0; i < params.size() && i < other.size(); ++i)
        if (!params[i].has_default && other[i].has_default) {
          params[i].has_default = 1;
          params[i].default_ = other[i].default_;
          params[i].rank_type_default = other[i].rank_type_default;
          merged = true;
        }
    }
    if (merged) found.template_params = ins().intern_seq(std::span<const ast::TemplateParamSema>(params));
    return found;
  }

  // The class template named `name` that is a member of namespace `ns` ([namespace.qual]).
  std::optional<ast::DeclSema> namespace_class_template(EntityId ns, NameId name) {
    return namespace_template(ns, name, ast::DeclKind::class_);
  }
  // The template of kind `kind` named `name` that is a member of namespace `ns`.
  std::optional<ast::DeclSema> namespace_template(EntityId ns, NameId name, ast::DeclKind kind,
                                                  std::optional<u32> at = std::nullopt) {
    const auto decls = q_->namespace_members(ns, name);
    for (const ast::DeclSema& d : decls) {
      if (d.kind != kind || !(d.flags & decl_flag::template_)) continue;
      if (at && std::ranges::any_of(decls, [&](const auto& other) { return other.entity != d.entity; })) {
        diag(*at, Diag::ambiguous_name, name);
        return std::nullopt;
      }
      return d;
    }
    return std::nullopt;
  }

  std::optional<ast::DeclSema> namespace_concept_for_expression(EntityId ns, NameId name, u32 at) {
    auto decls = q_->namespace_members(ns, name);
    auto concept_decl = std::ranges::find(decls, ast::DeclKind::concept_, &ast::DeclSema::kind);
    if (concept_decl == decls.end()) return std::nullopt;
    // Qualified lookup through using-directives can find distinct declarations ([namespace.qual]/4).
    if (std::ranges::any_of(decls, [&](const ast::DeclSema& d) {
          return d.entity != concept_decl->entity;
        })) {
      diag(at, Diag::ambiguous_name, name);
      return std::nullopt;
    }
    return *concept_decl;
  }

  // The specialization of class template `templ` for `args` ([temp.spec.general]): its entity is
  // the template and the canonical arguments (§B1.5). With dependent arguments it is a dependent
  // type ([temp.dep.type]).
  EntityId class_specialization(EntityId templ, std::span<const TemplateArg> args, Fp128 constraints = {}) {
    return templates().specialize_class(templ, args, constraints);
  }

  bool namespace_scoped(EntityId entity) const {
    const EntityId scope = w_->entities.rec(entity).scope;
    return scope == EntityId::none || w_->entities.rec(scope).kind == EntityKind::namespace_;
  }

  // Clang separates the canonical dependent type from its first written qualification.
  TemplateArg canonical_clang_class_argument(TemplateArg arg);

  // `X<args>` naming a class template specialization ([temp.names], [temp.arg]), or the type an
  // alias template specialization stands for: its type with the arguments substituted
  // ([temp.alias]/2).
  std::optional<QualType> class_template_id(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, u32 at,
                                           bool directly_named = true, bool* alias_constraint_failed = nullptr,
                                           bool replay_partial_alias = false, bool record_spelling = true);

  std::optional<QualType> substitute_alias(const ast::DeclSema& tmpl, std::span<const TemplateArg> args,
                                           const Bindings& bindings, u32 at, AliasSubstitution use,
                                           bool replay_partial_alias = false);

  // [temp.names]/9: naming a specialization requires satisfaction, even in an unevaluated
  // operand. Check direct, concrete type arguments in ordinary code until constraints can replay.
  bool check_named_type_constraints(const ast::DeclSema& tmpl, std::size_t written,
                                    std::span<const TemplateArg> args, const Bindings& bindings, u32 at) {
    if (tmpl.constraints == ConstId::none || in_requires_body_ || templated() || written != args.size() ||
        !std::ranges::all_of(args, [&](const TemplateArg& arg) {
          return arg.kind == ast::TemplateArgKind::type && !dependent(arg);
        }))
      return true;
    auto t = templates();
    bool hard_error = false;
    auto sat = named_constraint_satisfaction(t, tmpl.constraints, bindings, tmpl.kind == ast::DeclKind::type_alias, hard_error);
    if (hard_error) return false;
    if (sat.value_or(true)) return true;
    diag(at, Diag::constraints_not_satisfied, w_->entities.rec(tmpl.entity).name);
    return false;
  }

  std::optional<bool> named_constraint_satisfaction(Templates& t, ConstId c, const Bindings& bindings,
                                                    bool bind_lazily, bool& hard_error) {
    // [temp.constr.op]: an operand that satisfaction skips must not substitute its mapping.
    if (bind_lazily && ins().values.is<ast::VConstraint>(c)) {
      auto v = ins().values.get<ast::VConstraint>(c);
      bool any_unknown = false;
      for (ConstId operand : ins().seqs.elements(v.operands)) {
        auto sat = named_constraint_satisfaction(t, operand, bindings, bind_lazily, hard_error);
        if (hard_error) return std::nullopt;
        if (!sat) any_unknown = true;
        else if (*sat == (v.op == 1)) return *sat;
      }
      if (any_unknown) return std::nullopt;
      return v.op == 0;
    }
    auto normal = t.substitute_constraint(c, bindings);
    if (!normal) return false;
    auto sat = q_->satisfied(*normal);
    hard_error = q_->satisfaction_failed(*normal);
    return sat;
  }

  // `v<args>` naming a variable template specialization ([temp.pre], [temp.inst]/7): an lvalue of
  // the specialization, of the template's type with the arguments substituted. Its entity is the
  // template and the canonical arguments (§B1.5), as a function template specialization's is.
  Ex variable_template_id(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, u32 begin, u32 end);
  Ex dependent_variable_template_id(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, u32 begin, u32 end,
                                    bool qualified = false, bool global = false, std::span<const NameId> qualifier = {}) {
    auto full = template_arguments(tmpl, args, begin);
    if (!full) return {NodeId::none, begin, end};
    const auto converted = ins().intern_seq(std::span<const TemplateArg>(*full));
    const bool gcc = compatibility() == ast::Compatibility::gcc;
    auto value = ins().intern_value(ast::VDepVariable{tmpl.entity, converted,
                                                     gcc ? converted : ins().intern_seq(args),
                                                     u8(!gcc && qualified), u8(!gcc && global), ins().intern_seq(qualifier)});
    return literal(types().referee(tmpl.type), value, ValueCategory::lvalue, begin, end);
  }
  // The initializer is replayed with bound arguments. Check the written arguments here without
  // forming a specialization while an argument's integral expression still needs evaluation.
  Ex replayed_variable_template_id(const ast::DeclSema& tmpl, std::span<const TemplateArg> args,
                                   std::span<const Ex> deferred, u32 begin, u32 end) {
    const NameId name = w_->entities.rec(tmpl.entity).name;
    const auto params = ins().seqs.elements(tmpl.template_params);
    Templates t = templates();
    if (!check_template_expansions(t, params, args, begin)) return {NodeId::none, begin, end};
    std::vector<std::optional<TemplateArg>> out(params.size());
    const auto omitted = std::span(params).subspan(std::min(args.size(), params.size()));
    if (!t.explicit_into(params, args, out, /*defer_values=*/true) ||
        std::ranges::any_of(omitted, [](const auto& p) { return !p.pack && !p.has_default; })) {
      diag(begin, Diag::template_args_mismatch, name);
      return {NodeId::none, begin, end};
    }
    return dependent_node(deferred, begin, end, std::nullopt, ValueCategory::lvalue, expr_flag::type_dependent);
  }
  // The specialization of variable template `templ` for `args` ([temp.spec.general]).
  EntityId variable_specialization(EntityId templ, std::span<const TemplateArg> args, Fp128 constraints = {}) {
    return templates().specialize_variable(templ, args, constraints);
  }

  // The arguments of a template-id for `tmpl` ([temp.arg]): each of its parameter's kind, values
  // converted to the parameter's type, defaults for the rest; none, diagnosed, if they do not fit.
  std::optional<std::vector<TemplateArg>> template_arguments(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, u32 at) {
    std::vector<ast::TemplateParamSema> params = ins().seqs.elements(tmpl.template_params);
    NameId name = w_->entities.rec(tmpl.entity).name;
    bool type_template = tmpl.kind == ast::DeclKind::type_alias || tmpl.kind == ast::DeclKind::class_;
    Templates t = type_template ? templates_for_use(at) : templates();
    if (!check_template_expansions(t, params, args, at)) return std::nullopt;
    std::vector<std::optional<TemplateArg>> out(params.size());
    if (!t.explicit_into(params, args, out)) {
      diag(at, Diag::template_args_mismatch, name);
      return std::nullopt;
    }
    t.immediate_default_constant = [this, at](EntityId function, std::span<const TemplateArg> operands, QualType target) {
      return evaluate_immediate_template_default(function, operands, target, at);
    };
    const auto before = diag_count();
    auto full = t.complete(tmpl, params, out);
    if (!full && !has_error_since(before)) diag(at, Diag::template_args_mismatch, name);
    return full;
  }

  bool check_template_expansions(Templates& t, std::span<const ast::TemplateParamSema> params,
                                 std::span<const TemplateArg> args, u32 at) {
    // An expansion only for a pack parameter: for others it is not supported yet (#88).
    for (std::size_t i = 0; i < args.size() && i < params.size() && !params[i].pack; ++i)
      if (t.is_expansion(args[i])) {
        diag(at, Diag::not_supported, lex::Tk::ellipsis);
        return false;
      }
    return true;
  }

  // `using enum E;` in a block ([enum.udecl]): E's enumerators are found in this scope.
  void using_enum(QualType t, u32 at_, std::optional<NameId> name = std::nullopt) {
    if (!types().is_enum(t)) {
      diag(at_, Diag::not_a_type, name.value_or(no_name()));
      return;
    }
    const EntityId enumeration = types().enum_of(t);
    if (!q_->require_enum_definition(enumeration)) return;
    const EnumRec* rec = enum_rec(enumeration);
    if (!rec || !rec->defined) {
      diag(at_, Diag::incomplete_type, t);
      return;
    }
    using_enums_.push_back({scopes_.size() - 1, enumeration});
  }
  void replay_block_using(NameId name, std::vector<ast::DeclSema> decls, std::optional<std::size_t> type_index) {
    block_usings_.push_back({scopes_.size() - 1, name, std::move(decls), type_index});
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
  struct DefaultSourceObservations {
    bool line = false;
    EntityId descriptor_type = EntityId::none;
  };
  struct DefaultSourceScope {
    Sema& sema;
    DefaultSourceObservations observed;
    DefaultSourceObservations* saved;
    explicit DefaultSourceScope(Sema& sema, bool capture = true)
        : sema(sema), saved(std::exchange(sema.default_source_observer_, capture ? &observed : nullptr)) {}
    ~DefaultSourceScope() { sema.default_source_observer_ = saved; }
  };
  engine::Loc source_position(u32 at) {
    return q_->source_loc({std::to_underlying(item_), loc_base_ + at}, at);
  }
  u32 source_line_at(u32 at) { return q_->presumed_line(source_position(at)); }
  ast::Compatibility compatibility() const { return q_->compatibility(); }
  std::optional<Diag> unresolved_default_source(const Ex& expression);
  // Whether this is a templated context: a template's definition or one of its instantiations.
  bool templated() const { return !template_scopes.empty() || instantiating(); }
  // A generic lambda can still be in its definition while its enclosing function is instantiated.
  bool in_template_definition() const {
    return std::ranges::any_of(template_scopes | std::views::join, [&](const ast::TemplateParamSema& p) {
      return !template_args.at(p.depth, p.index);
    });
  }
  bool in_function_or_lambda_body() const { return in_function_body || !lambdas_.empty(); }
  bool check_exception_construct(u32 at, lex::Tk keyword) {
    if (q_->exceptions_enabled() || (in_function_or_lambda_body() && in_template_definition()))
      return true;
    diag(at, Diag::exceptions_disabled, keyword);
    return false;
  }
  struct RequiresBodyScope {
    bool& context;
    bool saved;
    explicit RequiresBodyScope(Sema& sema) : context(sema.in_requires_body_), saved(std::exchange(context, true)) {}
    ~RequiresBodyScope() { context = saved; }
  };
  // An atomic constraint's expression where its constraint-expression is written: it is read
  // again with its parameter mapping bound when its satisfaction is checked ([temp.constr.atomic]),
  // so what needs the template arguments can wait for that.
  struct ConstraintAtomScope {
    bool& context;
    bool saved;
    explicit ConstraintAtomScope(Sema& sema) : context(sema.in_constraint_atom_), saved(std::exchange(context, true)) {}
    ~ConstraintAtomScope() { context = saved; }
  };
  // The concept unqualified lookup finds for `name`.
  std::optional<ast::DeclSema> concept_named(NameId name) { return named_template(name, ast::DeclKind::concept_); }
  // An ordinary unqualified type-constraint on a placeholder ([basic.lookup.unqual], [temp.param]).
  std::optional<EntityId> placeholder_concept(NameId name, u32 at);
  // The type-constraints of the template-head being read, in normal form ([temp.constr.decl]/3).
  std::vector<ConstId> head_constraints;
  // Atoms in a lambda constraint replay in its own declarator scope.
  ast::SeqId<ast::AtomLambdaScope> atom_lambda_scope{};
  struct LambdaConstraintScope {
    Sema& sema;
    ast::SeqId<ast::AtomLambdaScope> saved_scope;

    LambdaConstraintScope(Sema& s, std::span<const ast::ParamSema> parameters)
        : sema(s), saved_scope(s.atom_lambda_scope) {
      ast::AtomLambdaScope scope;
      if (!sema.template_scopes.empty())
        scope.head = sema.ins().intern_seq(std::span<const ast::TemplateParamSema>(sema.template_scopes.back()));
      scope.parameters = sema.ins().intern_seq(parameters);
      sema.atom_lambda_scope = sema.ins().intern_seq(std::span<const ast::AtomLambdaScope>(&scope, 1));
    }
    ~LambdaConstraintScope() { sema.atom_lambda_scope = saved_scope; }
  };

  // An atomic constraint ([temp.constr.atomic]): the expression at [begin, end) of this part, whose
  // parameter mapping takes the innermost head's parameters among `names` (the identifiers it
  // contains) to themselves, the others to none.
  ConstId atom(u32 begin, u32 end, std::span<const NameId> names, std::span<const PackRef> fold_packs = {});
  // `C<args>` in normal form: C's with the arguments substituted into its atoms' mappings
  // ([temp.constr.normal]/1.4); none, diagnosed, when they do not fit or a mapping cannot be formed.
  // Memoized for the unit: a concept-id's normal form depends only on the concept and its
  // arguments, and what satisfaction makes of it is the program's first answer anyway
  // ([temp.constr.atomic]/3). A member template's type-constraint is formed again in each
  // specialization of its class that is instantiated (#1653). What diagnosed something is not kept,
  // so each use says it again.
  std::optional<ConstId> concept_constraint(const ast::DeclSema& c, std::span<const TemplateArg> args, u32 at) {
    auto* memo = q_->concept_normal_forms();
    const auto key = memo ? std::pair(std::to_underlying(c.entity), ins().intern_seq(args).index) : std::pair(0u, 0u);
    if (memo)
      if (auto it = memo->find(key); it != memo->end()) return it->second;
    const std::size_t before = diag_count();
    auto full = template_arguments(c, args, at);
    if (!full) return std::nullopt;
    std::vector<ast::TemplateParamSema> params = ins().seqs.elements(c.template_params);
    auto normal = templates().substitute_constraint(c.constraints, Bindings::of(params.empty() ? 0 : params[0].depth, *full));
    if (memo && normal && diag_count() == before) memo->emplace(key, *normal);
    return normal;
  }
  // Check the immediately-declared constraint on the type deduced for the placeholder itself
  // ([dcl.type.auto.deduct], [temp.param]), before publishing the completed declarator type.
  bool check_placeholder_constraint(QualType placeholder, QualType deduced, u32 at);
  ConstId concept_parameter_id(const ast::TemplateParamSema& p, std::span<const TemplateArg> args) {
    const auto parameter = bound_arg(p).value_or(
        TemplateArg{TemplateArgKind::template_, p.type, ConstId::none, {}, EntityId::none, true});
    return ins().intern_value(ast::VConceptParameter{parameter, ins().intern_seq(args), p.name});
  }

  Ex concept_parameter_expression(const ast::TemplateParamSema& p, std::span<const TemplateArg> args,
                                  u32 begin, u32 end, bool written) {
    const auto expression = concept_parameter_id(p, args);
    const auto boolean = builtin(Builtin::bool_);
    if (written) return literal(boolean, expression, ValueCategory::prvalue, begin, end);
    auto normal = templates().substitute_constraint(expression, {});
    if (!normal) return {NodeId::none, begin, end};
    auto sat = q_->satisfied(*normal);
    if (q_->satisfaction_failed(*normal)) return {NodeId::none, begin, end};
    if (!sat) return dependent_node({}, begin, end, boolean);
    return bool_literal(*sat, begin, end);
  }

  // A concept-id as an expression ([temp.names]/8): whether it is satisfied, a `bool` prvalue;
  // value-dependent while its arguments are.
  Ex concept_id_expr(const ast::DeclSema& c, std::span<const TemplateArg> args, u32 begin, u32 end,
                     bool preserve_value = false) {
    QualType b = builtin(Builtin::bool_);
    if (std::ranges::any_of(args, [&](const TemplateArg& a) { return dependent(a); })) {
      if (!preserve_value || !std::ranges::all_of(args, [&](const TemplateArg& a) {
            return templates().structural_operand(a);
          })) return dependent_node({}, begin, end, b);
      if (!template_arguments(c, args, begin)) return {NodeId::none, begin, end};
      auto value = ins().intern_value(ast::VDepConcept{c.entity, ins().intern_seq(args)});
      return literal(b, value, ValueCategory::prvalue, begin, end);
    }
    auto nf = concept_constraint(c, args, begin);
    if (!nf) return {NodeId::none, begin, end};
    auto sat = q_->satisfied(*nf);
    if (!sat) return dependent_node({}, begin, end, b);
    if (q_->satisfaction_failed(*nf)) return {NodeId::none, begin, end};
    return bool_literal(*sat, begin, end);
  }

  std::optional<ConstId> evaluate_dependent_concept(EntityId entity, std::span<const TemplateArg> args, u32 at) {
    auto c = q_->template_decl(entity);
    if (!c) return std::nullopt;
    auto nf = concept_constraint(*c, args, at);
    if (!nf) return std::nullopt;
    auto sat = q_->satisfied(*nf);
    if (!sat || q_->satisfaction_failed(*nf)) return std::nullopt;
    return w_->values.integer(builtin(Builtin::bool_), *sat);
  }

  // ------------------------------------------------------------ type-trait built-ins (docs/EXTENSIONS.md)

  // The type-trait built-in recognized for `name`.
  const lex::TypeTraitInfo* type_trait(NameId name) { return lex::type_trait(ins().names.spelling(name)); }
  // Whether a class type is a union, without requiring its definition.
  std::optional<bool> is_union_type(QualType t) {
    if (!types().is_class(t)) return false;
    if (member_introduction && member_introduction->sema.entity == types().class_of(t))
      return member_introduction->is_union;
    return Templates(*w_, classes()).is_union_class(types().class_of(t));
  }
  // `__trait(T)` that transforms a type ([meta.trans]): the type it gives.
  // A pack-index-specifier `name...[index]` ([dcl.type.pack.index]): `name` names a template type
  // parameter pack and `index` is an integral constant, converted to std::size_t for the choice
  // and kept in its own type in the specifier, as the index of a symbol is. The selected element
  // once the pack is bound and the index known, the dependent specifier until then.
  std::optional<QualType> pack_index_type(NameId name, u32 at, ConstId index, u32 index_at, ConstId written);
  std::optional<QualType> trait_type(lex::TypeTrait t, QualType arg, u32 at) {
    if (t == lex::TypeTrait::underlying_type && deferred_enum_layout(arg))
      return types().transform(u8(t), arg);
    auto r = templates().transform(t, arg);
    if (!r) diag(at, Diag::trait_not_applicable, arg);
    return r;
  }
  engine::Loc type_trait_loc(u32 at) {
    return q_->source_loc({std::to_underlying(item_), loc_base_ + at}, at);
  }
  // A type trait with dependent operands in an atomic constraint's expression is evaluated when
  // the atom is read again for its satisfaction ([temp.constr.atomic]), and the classes it needs
  // complete are judged there; none outside an atom.
  std::optional<Ex> trait_at_satisfaction(engine::Loc loc, u32 begin, u32 end) {
    if (!in_constraint_atom_) return std::nullopt;
    q_->record_dependent_type_trait_operand(loc);
    return dependent_node({}, begin, end, builtin(Builtin::bool_));
  }
  // The innermost array element must be complete or cv void ([meta.unary.prop]). Only classes
  // need a copyability fact after peeling extents ([basic.types.general], [class.prop]).
  Ex trivial_copyability_expr(QualType operand, u32 begin, u32 end);

  // [depr.meta.types]: both traits check the innermost element, including in unbounded arrays.
  Ex trivial_or_pod_expr(ast::DepOp op, NameId name, QualType operand, u32 begin, u32 end)
    pre(op == ast::DepOp::is_trivial || op == ast::DepOp::is_pod)
  {
    const Types& ts = types();
    const QualType type = ts.canonical(operand);
    const engine::Loc loc = type_trait_loc(begin);
    const auto fail = [&](Diag d, QualType reported) -> Ex {
      diag_at(loc, d, reported, name);
      return {NodeId::none, begin, end};
    };
    if (ts.is_dependent(type)) {
      if (q_->in_type_trait_pattern())
        if (auto value = templates().unary_trait_value(op, type)) {
          q_->record_dependent_type_trait_operand(loc);
          return literal(builtin(Builtin::bool_), *value, ValueCategory::prvalue, begin, end);
        }
      if (current_function != EntityId::none && templated()) {
        q_->record_dependent_type_trait_operand(loc);
        return dependent_node({}, begin, end, builtin(Builtin::bool_));
      }
      if (auto at_satisfaction = trait_at_satisfaction(loc, begin, end)) return *at_satisfaction;
      return fail(Diag::trivial_or_pod_unsupported, operand);
    }

    const QualType element = ts.innermost_array_element(type);
    if (ts.is_reference(element) || ts.is_function(element) || ts.is_void(element))
      return bool_literal(false, begin, end);
    if (ts.is_enum(element)) {
      const EntityId e = ts.enum_of(element);
      if (auto fixed = q_->defining_enum_is_fixed(e)) {
        if (!*fixed) return fail(Diag::trivial_or_pod_incomplete, element);
      } else if (const EnumRec* record = classes().find_enum(e)) {
        if (!record->fixed && !record->defined) return fail(Diag::trivial_or_pod_incomplete, element);
      } else {
        return fail(Diag::trivial_or_pod_unsupported, operand);
      }
    }
    if (ts.is_scalar(element) || ts.is_floating(element)) return bool_literal(true, begin, end);
    if (ts.is_class(element)) {
      const EntityId cls = ts.class_of(element);
      const auto demand = q_->type_trait_at_demand(cls, loc);
      if (!(demand ? *demand : q_->is_complete_at_source(cls))) {
        if (q_->type_trait_instantiation_failed(cls)) return {NodeId::none, begin, end};
        return fail(Diag::trivial_or_pod_incomplete, element);
      }
      // The class rules depend on eligible special members (#1303, #1318).
      return fail(Diag::trivial_or_pod_unsupported, element);
    }
    return fail(Diag::trivial_or_pod_unsupported, operand);
  }

  // The trait's domain requires the innermost array element to be complete or cv void
  // ([meta.unary.prop]). Its class facts are read only after completing the operand at that point.
  Ex standard_layout_expr(QualType operand, u32 begin, u32 end);

  // Array types are implicit-lifetime types regardless of their element types
  // ([basic.types.general], [meta.unary.prop]).
  Ex implicit_lifetime_expr(QualType operand, u32 begin, u32 end);

  // Array operands query the innermost element, even when an extent is unknown
  // ([meta.unary.prop]). Completeness is checked before looking for representation facts.
  Ex unique_object_representations_expr(QualType operand, u32 begin, u32 end);

  // In this domain every destructible type has trivial, nonthrowing destruction ([meta.unary.prop]).
  Ex destructibility_expr(ast::DepOp op, NameId name, QualType operand, u32 begin, u32 end);

  // [meta.unary.prop]: each base must also be empty. A nonempty contributor
  // settles the result even when another part of the hierarchy is unknown.
  std::optional<bool> class_is_empty(EntityId cls);

  Ex empty_expr(QualType operand, u32 begin, u32 end);

  // Arrays are aggregate types even when incomplete ([meta.unary.prop], [dcl.init.aggr]).
  // An owning replay completes a substituted class at the instantiation demand.
  Ex aggregate_trait_expr(QualType type, u32 begin, u32 end);

  struct ConstructibilityOperand {
    QualType type;
    u32 at;
  };

  enum class ExceptionTypeTraits { ordinary, checking_pattern, dependent };
  ExceptionTypeTraits exception_type_traits = ExceptionTypeTraits::ordinary;
  bool in_discarded_constraint_operand = false;

  bool defer_exception_type_trait(ast::DepOp op) {
    if (exception_type_traits == ExceptionTypeTraits::ordinary ||
        (op != ast::DepOp::is_nothrow_constructible && op != ast::DepOp::is_nothrow_assignable))
      return false;
    exception_type_traits = ExceptionTypeTraits::dependent;
    return true;
  }

  // A written exact move beats a copy taking an lvalue reference. Require that proof from
  // the recorded declarations before reading the winner's exception specification. Other
  // overload sets or implicit/defaulted moves need more object-model facts ([meta.unary.prop]).
  std::optional<bool> written_move_nothrow(QualType owner, bool constructing);

  // The owning replay supplies packs in written order and the demand point for their elements.
  // Validate every concrete operand's domain before deciding the predicate ([meta.unary.prop]).
  Ex constructibility_expr(lex::TypeTrait trait, NameId name, std::span<const ConstructibilityOperand> written, u32 begin, u32 end)
    pre(trait == lex::TypeTrait::is_constructible || trait == lex::TypeTrait::is_nothrow_constructible ||
        trait == lex::TypeTrait::is_trivially_constructible)
  {
    const Types& ts = types();
    const engine::Loc loc = type_trait_loc(begin);
    const auto [op, unsupported] = [trait] {
      switch (trait) {
      case lex::TypeTrait::is_constructible:
        return std::pair{ast::DepOp::is_constructible, Diag::constructibility_unsupported};
      case lex::TypeTrait::is_nothrow_constructible:
        return std::pair{ast::DepOp::is_nothrow_constructible, Diag::nothrow_constructibility_unsupported};
      case lex::TypeTrait::is_trivially_constructible:
        return std::pair{ast::DepOp::is_trivially_constructible, Diag::trivial_constructibility_unsupported};
      default: std::unreachable();
      }
    }();
    const auto fail = [&](Diag d, const auto&... args) -> Ex {
      diag_at(loc, d, args...);
      return {NodeId::none, begin, end};
    };
    Templates t = templates();
    std::vector<ConstructibilityOperand> args;
    for (const ConstructibilityOperand& operand : written) {
      if (ts.kind(operand.type) != TypeKind::pack_expansion ||
          !q_->replaying_type_trait_operand(type_trait_loc(operand.at))) {
        args.push_back(operand);
        continue;
      }
      TemplateArg expansion = t.type_arg(operand.type);
      const auto diagnostics = diag_count();
      auto elements = t.substitute_expansion(expansion, template_args);
      if (!elements) {
        if (has_error_since(diagnostics)) return {NodeId::none, begin, end};
        TemplateArg pattern = t.pattern_of(expansion);
        if (!t.expansion_length(pattern, template_args)) return fail(Diag::pack_length_mismatch);
        return fail(Diag::constructibility_substitution, pattern.type, name);
      }
      for (const TemplateArg& element : *elements) args.push_back({element.type, operand.at});
    }

    bool dependent = false;
    for (const ConstructibilityOperand& operand : args) {
      QualType type = ts.canonical(operand.type);
      if (ts.is_dependent(type)) {
        dependent = true;
        continue;
      }
      // Unknown-bound arrays are admitted on their own. Known-bound arrays need a complete element.
      while (ts.is_array(type) && ts.rec(type).bound_value != ConstId::none)
        type = ts.canonical(ts.rec(type).inner);
      // An enumeration without a fixed underlying type is incomplete in its own enumerator list.
      if (ts.is_enum(type)) {
        const EnumRec* defining = q_->defining_enum(ts.enum_of(type));
        if (defining && !defining->fixed) return fail(Diag::constructibility_incomplete, operand.type, name);
      }
      if (ts.is_class(type)) {
        const EntityId cls = ts.class_of(type);
        const auto demand = q_->type_trait_at_demand(cls, type_trait_loc(operand.at));
        if (!(demand ? *demand : q_->is_complete_at_source(cls))) {
          if (q_->type_trait_instantiation_failed(cls)) return {NodeId::none, begin, end};
          return fail(Diag::constructibility_incomplete, operand.type, name);
        }
      }
    }
    if (dependent) {
      if (q_->in_type_trait_pattern()) {
        std::vector<TemplateArg> pattern;
        for (const ConstructibilityOperand& operand : args) pattern.push_back(t.type_arg(operand.type));
        if (auto value = t.replayed_type_trait_pattern(op, pattern)) {
          for (const ConstructibilityOperand& operand : args)
            if (ts.is_dependent(operand.type)) q_->record_dependent_type_trait_operand(type_trait_loc(operand.at));
          return literal(builtin(Builtin::bool_), *value, ValueCategory::prvalue, begin, end);
        }
      }
      if (in_constraint_atom_ || defer_exception_type_trait(op)) {
        for (const ConstructibilityOperand& operand : args)
          if (ts.is_dependent(operand.type)) q_->record_dependent_type_trait_operand(type_trait_loc(operand.at));
        return dependent_node({}, begin, end, builtin(Builtin::bool_));
      }
      const bool pack = std::ranges::any_of(args, [&](const ConstructibilityOperand& arg) {
        return ts.kind(arg.type) == TypeKind::pack_expansion;
      });
      return fail(pack ? Diag::constructibility_pack : Diag::constructibility_dependent, name);
    }

    // In this bounded domain every viable initialization is trivial and nonthrowing.
    const QualType target = ts.canonical(args.front().type);
    if (ts.is_function(target) || ts.is_void(target)) return bool_literal(false, begin, end);
    if (args.size() == 1) {
      if (ts.is_scalar(target)) return bool_literal(true, begin, end);
      if (ts.is_reference(target)) return bool_literal(false, begin, end);
    }
    const std::span<const ConstructibilityOperand> arguments(args);
    // A scalar is direct-initialized from one expression of arithmetic, pointer or
    // `std::nullptr_t` type by a standard conversion, as copy-initialization would, and also
    // `bool` from a null pointer value ([dcl.init.general]/16.9, [conv.bool]).
    const auto scalar = [&](QualType t) {
      t = ts.canonical(t);
      return ts.is_arithmetic(t) || ts.is_pointer(t) || ts.is(t, Builtin::nullptr_t);
    };
    if (scalar(target) && std::ranges::all_of(arguments.subspan(1), [&](const ConstructibilityOperand& arg) {
          return scalar(ts.referee(ts.canonical(arg.type)));
        })) {
      if (args.size() != 2) return bool_literal(false, begin, end);
      const QualType from = object_unqualified(ts.referee(ts.canonical(args[1].type))), to = object_unqualified(target);
      if (ts.is(from, Builtin::nullptr_t) && ts.is(to, Builtin::bool_)) return bool_literal(true, begin, end);
      switch (scalar_conversion(from, to, loc)) {
        case PointerConversion::no: return bool_literal(false, begin, end);
        case PointerConversion::yes: return bool_literal(true, begin, end);
        case PointerConversion::failed: return {NodeId::none, begin, end};
        case PointerConversion::unmodelled: break;
      }
    }
    if (op == ast::DepOp::is_nothrow_constructible && ts.is_class(target) && Types::cv(target) == Cv::none && args.size() == 2) {
      const TypeRec source = ts.rec(args[1].type);
      if (source.kind == TypeKind::reference && source.rvalue_ref && ts.canonical(source.inner) == target)
        if (auto answer = written_move_nothrow(target, /*constructing=*/true)) return bool_literal(*answer, begin, end);
    }
    return fail(unsupported, args.front().type);
  }
  std::optional<Ex> replayed_binary_type_trait(ast::DepOp op, QualType first, QualType second, u32 begin, u32 end) {
    if (!q_->in_type_trait_pattern()) return std::nullopt;
    Templates t = templates();
    const std::array operands{t.type_arg(first), t.type_arg(second)};
    if (auto value = t.replayed_type_trait_pattern(op, operands))
      return literal(builtin(Builtin::bool_), *value, ValueCategory::prvalue, begin, end);
    return std::nullopt;
  }
  // In the trait's unevaluated expression, declval gives a modifiable arithmetic lvalue only
  // for a non-const lvalue reference. Built-in arithmetic assignment is trivial and nonthrowing
  // ([meta.unary.prop], [expr.assign]).
  Ex assignability_expr(ast::DepOp op, NameId name, QualType left, QualType right, u32 begin, u32 end);
  // Whether `base` is a base of `derived` along exactly one path, and that path public: an
  // unambiguous, accessible base from outside both classes ([class.derived], [class.access.base]).
  // `unknown` when a class on the way is not defined or, with `ordinary_layouts`, is a union or
  // needs virtual support. The base-clause parser admits only nonvirtual bases. Count paths
  // regardless of access: a private path can still make a public one ambiguous. Two arrivals at
  // a class suffice to prove ambiguity for any base reached through it.
  enum class BasePath : u8 { none, unique_public, unknown };
  BasePath base_path(EntityId derived, EntityId base, bool ordinary_layouts);
  // The base's record when it is a unique public base and every class on the way has an
  // ordinary layout; the by-value and reference conversions model only those.
  const ClassRec* unique_public_base(EntityId derived, EntityId base) {
    return base_path(derived, base, true) == BasePath::unique_public ? classes().find(base) : nullptr;
  }

  // The source of the copy or move is the base subobject, not a newly constructed Derived
  // ([meta.rel], [over.best.ics]). With no written constructors or subobjects in Base, its
  // implicit special members are public and there are no converting constructors to compare.
  bool supports_base_value_conversion(const ClassRec& base, QualType from, bool source_lvalue) {
    using enum CopyOperation;
    const auto& operations = base.trivial_copyability;
    if (base.closure || base.user_ctors || !base.bases.empty() || !base.fields.empty() || base.unapplied_layout ||
        Types::is_volatile(from) || !base.destructor_usable || base.destructor != EntityId::none ||
        operations.destructor != eligible_trivial)
      return false;
    if (!source_lvalue && !Types::is_const(from) && operations.move_constructor != ineligible)
      return operations.move_constructor == eligible_trivial;
    return operations.copy_constructor == eligible_trivial && !base.copy_deleted && base.defaulted_copy_const;
  }

  // Whether a prvalue of pointer or `std::nullptr_t` type `from` converts implicitly to the
  // non-reference type `to`. Class, array and function targets, and pointers whose pointee arrays
  // or member pointers differ (#1399), are not modelled; `failed` when Derived's instantiation
  // failed, which was diagnosed there.
  enum class PointerConversion : u8 { no, yes, unmodelled, failed };
  PointerConversion object_pointer_conversion(QualType from, QualType to, engine::Loc loc);
  // Whether `a` and `b` are similar ([conv.qual]/2), for the pointer levels qualification
  // conversions walk: none when an array or a member pointer below them may make them so.
  std::optional<bool> similar_types(QualType a, QualType b) {
    const Types& ts = types();
    a = ts.canonical(a);
    b = ts.canonical(b);
    while (ts.is_pointer(a) && ts.is_pointer(b)) {
      a = ts.canonical(ts.pointee(a));
      b = ts.canonical(ts.pointee(b));
    }
    if (same_unqualified(a, b)) return true;
    if ((ts.is_array(a) && ts.is_array(b)) || (ts.is_member_pointer(a) && ts.is_member_pointer(b))) return std::nullopt;
    return false;
  }
  // Whether a prvalue of the arithmetic, pointer or `std::nullptr_t` type `from` converts
  // implicitly to the type `to` of the same kinds ([conv]). `from` comes from `declval`, never a
  // null pointer constant ([conv.ptr]/1).
  PointerConversion scalar_conversion(QualType from, QualType to, engine::Loc loc) {
    const Types& ts = types();
    if (ts.is_pointer(from) || ts.is(from, Builtin::nullptr_t)) return object_pointer_conversion(from, to, loc);
    return ts.is_arithmetic(to) ? PointerConversion::yes : PointerConversion::no;
  }
  // Whether the reference type `reference` to an arithmetic, pointer or `std::nullptr_t` type
  // binds to an lvalue (else xvalue) of such a type `source` ([dcl.init.ref]/5).
  PointerConversion scalar_reference_binding(QualType source, bool lvalue, QualType reference, engine::Loc loc) {
    using enum PointerConversion;
    Types& ts = types();
    const TypeRec r = ts.rec(reference);
    const QualType target = ts.canonical(r.inner);
    // cv1 T1 is reference-compatible with cv2 T2 when a pointer to cv2 T2 converts to a pointer to
    // cv1 T1 by a standard conversion sequence ([dcl.init.ref]/4); for these types, by a
    // qualification conversion.
    const QualType from_pointer = ts.pointer(source), to_pointer = ts.pointer(target);
    const bool compatible = qualification_convertible(from_pointer, to_pointer);
    if (!r.rvalue_ref && lvalue && compatible) return yes;           // /5.1
    if (!r.rvalue_ref && object_cv(target) != Cv::const_) return no;  // /5.2
    if (!lvalue && compatible) return yes;                            // /5.3
    // /5.4: otherwise T1 shall not be reference-related to T2 (similar, for these types; CWG2352),
    // and the source is converted to a prvalue of T1 and materialized.
    const auto related = similar_types(from_pointer, to_pointer);
    if (!related) return unmodelled;
    const bool clang_pointer_temporary = compatibility() == ast::Compatibility::clang &&
                                         !lvalue && r.rvalue_ref && ts.is_pointer(source) && ts.is_pointer(target);
    if (*related && !clang_pointer_temporary) return no;
    return scalar_conversion(object_unqualified(source), object_unqualified(target), loc);
  }
  // An operand of type `type` and category `category` with no value: what a type trait's
  // `declval<T>()` or a conversion function's result gives the initialization rules, which ask
  // only for those ([declval], [meta.rel]).
  Ex opaque_operand(QualType type, ValueCategory category, u32 at_) {
    const ExprInfo info{type, category, 0};
    return node(ast::Dependent{info, b_->list({})}, at_, at_, info);
  }
  // `declval<T>()`: an lvalue for an lvalue reference or function T, else an xvalue.
  Ex declval_operand(QualType t, u32 at_) {
    const Types& ts = types();
    const QualType object = ts.canonical(ts.referee(t));
    const bool lvalue = (ts.is_reference(t) && !ts.rec(t).rvalue_ref) || ts.is_function(object);
    return opaque_operand(object, lvalue ? ValueCategory::lvalue : ValueCategory::xvalue, at_);
  }
  // A failure that makes an initialization ill-formed in its immediate context: no viable,
  // an ambiguous, a deleted or an inaccessible function, an abstract class object, no
  // conversion, or a reference that cannot bind. A type trait asking for that initialization
  // is false ([meta.rel], [meta.unary.prop]).
  static bool immediate_initialization_failure(engine::DiagId id) {
    for (Diag d : {Diag::no_viable_constructor, Diag::no_viable_function, Diag::ambiguous_call,
                   Diag::ambiguous_constructor, Diag::deleted_constructor, Diag::deleted_function,
                   Diag::private_member, Diag::protected_member, Diag::inaccessible_base, Diag::abstract_object,
                   Diag::no_conversion, Diag::bind_to_lvalue, Diag::bind_to_rvalue, Diag::bind_drops_qualifiers})
      if (id == diag_id(d)) return true;
    return false;
  }
  // Performs `initialize` in an unevaluated context as a trial and keeps none of its
  // diagnostics. `no` when it is ill-formed in its immediate context, and `unmodelled` on
  // anything else, such as a limit of ycxx's or a failure whose diagnostic a replay already
  // reported (`diag` does not repeat it).
  template <class F>
  PointerConversion trial_initialization(F&& initialize) {
    using enum PointerConversion;
    const auto before = diag_count();
    ++unevaluated;
    const Ex result = initialize();
    --unevaluated;
    std::vector<engine::DiagId> errors;
    for (const engine::Diagnostic& d : std::span(*diags_).subspan(before))
      if (auto kind = sema_diag_of(d.id); kind && message_of(*kind).severity == lex::Severity::error) errors.push_back(d.id);
    truncate_diags(before);
    if (result.ok() && errors.empty()) return yes;
    if (!errors.empty() && std::ranges::all_of(errors, immediate_initialization_failure)) return no;
    return unmodelled;
  }
  // Whether `cls` or one of its bases declares a conversion function; true where a class on
  // the way has no record.
  bool declares_conversion_functions(EntityId cls) {
    const NameId name = ins().names.intern(lex::conversion_function_name);
    std::vector<EntityId> pending{cls};
    std::set<EntityId> visited;
    while (!pending.empty()) {
      const EntityId current = pending.back();
      pending.pop_back();
      if (!visited.insert(current).second) continue;
      const ClassRec* record = classes().find(current);
      if (!record || !record->defined) return true;
      if (std::ranges::contains(record->members, name, &MemberDeclRec::name) ||
          std::ranges::contains(record->introduced, name, &MemberDeclRec::name))
        return true;
      for (const BaseRec& edge : record->bases) pending.push_back(edge.cls);
    }
    return false;
  }
  // [over.match.copy]/1.2, [over.match.conv]: the conversion functions of the class of `object`
  // that initialize an object of the non-reference type `target`. For a class target, those
  // yielding it or a class derived from it; for another, those whose result converts to it by a
  // standard conversion sequence. Explicit ones only for direct-initialization of a non-class
  // target, through an exact match. Candidates rank by the implicit object parameter, then by
  // that second standard conversion sequence ([over.match.best.general]/2.2). None without a
  // viable candidate; `no` when the best is ambiguous, deleted or not public (access is checked
  // from a context unrelated to both types, [meta.rel]). Conversion function templates and those
  // of base classes are unmodelled.
  std::optional<PointerConversion> conversion_function_initialization(const Ex& object, QualType target, bool direct);
  // `__is_convertible(From, To)` through a user-defined conversion ([conv.general]/3,
  // [over.best.ics], [class.conv]): the copy-initialization of a `To` from `declval<From>()`,
  // where one of the types is a class and they are not reference-related. None outside that
  // domain, or where a class is incomplete or a union, for the standard conversions above.
  std::optional<PointerConversion> user_convertibility(QualType from, QualType to, engine::Loc loc, u32 at_);
  // Reference binding follows the category of declval<From>() ([meta.rel], [dcl.init.ref]).
  Ex convertibility_expr(ast::DepOp op, NameId name, QualType source, QualType target, u32 begin, u32 end);
  // `__reference_converts_from_temporary(T, U)` and `__reference_constructs_from_temporary(T, U)`
  // ([meta.unary.prop]): T is a reference and `T t = VAL<U>;` (or `T t(VAL<U>)`) binds t to a temporary,
  // VAL<U> a prvalue for a non-reference U. For the operands convertibility supports, arithmetic
  // and reference-related classes, copy- and direct-initialization agree: the temporary is a
  // converted value or a materialized prvalue ([dcl.init.ref]/5). A dependent operand is decided
  // when the enclosing template is instantiated.
  Ex reference_temporary_expr(ast::DepOp op, NameId name, QualType target, QualType source, u32 begin, u32 end);
  // [meta.rel] uses existence of a base, regardless of access or the number of paths. With
  // `virtual_only`, a virtual base class: one some class in the graph names with a virtual
  // base-specifier ([class.mi]), not a nonvirtual base of a virtual base. None when a class on
  // the way is not defined and no answer was found without it.
  std::optional<bool> reaches_base(EntityId derived, EntityId base, bool virtual_only = false) {
    const Classes& records = classes();
    std::vector<EntityId> pending{derived};
    std::set<EntityId> visited;
    bool complete_graph = true;
    while (!pending.empty()) {
      const EntityId current = pending.back();
      pending.pop_back();
      if (current == base && !virtual_only) return true;
      if (!visited.insert(current).second) continue;
      const ClassRec* record = records.find(current);
      if (!record || !record->defined) {
        complete_graph = false;
        continue;
      }
      for (const BaseRec& edge : record->bases) {
        if (virtual_only && edge.virtual_ && edge.cls == base) return true;
        pending.push_back(edge.cls);
      }
    }
    return complete_graph ? std::optional(false) : std::nullopt;
  }
  // A class-relation trait with a dependent operand ([meta.rel]) is decided once its operands are
  // known, with completeness judged there ([temp.point]): when its class-template base or
  // variable-template initializer is replayed, when its function template body is analysed again
  // for an instantiation ([temp.inst]), or when its atomic constraint's satisfaction is checked.
  // None elsewhere.
  std::optional<Ex> dependent_class_relation(ast::DepOp op, QualType first, QualType second, u32 begin, u32 end) {
    const engine::Loc loc = type_trait_loc(begin);
    if (auto pattern = replayed_binary_type_trait(op, first, second, begin, end)) {
      // Either operand becoming concrete at replay can change which classes need completion.
      q_->record_dependent_type_trait_operand(loc);
      return *pattern;
    }
    if (current_function != EntityId::none && templated()) {
      q_->record_dependent_type_trait_operand(loc);
      return dependent_node({}, begin, end, builtin(Builtin::bool_));
    }
    return trait_at_satisfaction(loc, begin, end);
  }
  Ex base_of_expr(QualType base_type, QualType derived_type, u32 begin, u32 end);
  // `__builtin_is_virtual_base_of(Base, Derived)` and `__is_pointer_interconvertible_base_of(Base,
  // Derived)` ([meta.rel]) ignore cv. A nonclass or union operand gives false. Only pointer
  // interconvertibility returns true for the same incomplete nonunion class. Other nonunion
  // class queries need a complete Derived.
  std::expected<std::pair<EntityId, EntityId>, Ex> class_relation_operands(NameId name, QualType base_type,
                                                                           QualType derived_type, bool same,
                                                                           u32 begin, u32 end) {
    const Types& ts = types();
    const QualType base = Types::unqualified(ts.canonical(base_type));
    const QualType derived = Types::unqualified(ts.canonical(derived_type));
    const engine::Loc loc = type_trait_loc(begin);
    if (!ts.is_class(base) || !ts.is_class(derived)) return std::unexpected(bool_literal(false, begin, end));
    const auto base_union = is_union_type(base), derived_union = is_union_type(derived);
    if (base_union == true || derived_union == true) return std::unexpected(bool_literal(false, begin, end));
    if (!base_union || !derived_union) {
      diag_at(loc, Diag::class_relation_unsupported, name, base_type, derived_type);
      return std::unexpected(Ex{NodeId::none, begin, end});
    }
    const EntityId b = ts.class_of(base), d = ts.class_of(derived);
    if (b == d && same) return std::unexpected(bool_literal(true, begin, end));
    const auto complete_at_demand = q_->type_trait_at_demand(d, loc);
    if (!(complete_at_demand ? *complete_at_demand : q_->is_complete_at_source(d))) {
      if (!q_->type_trait_instantiation_failed(d)) diag_at(loc, Diag::class_relation_incomplete, derived_type, name);
      return std::unexpected(Ex{NodeId::none, begin, end});
    }
    if (b == d) return std::unexpected(bool_literal(false, begin, end));
    return std::pair{b, d};
  }
  // A class is not its own virtual base. The graph has no virtual edges until #310.
  Ex virtual_base_of_expr(NameId name, QualType base_type, QualType derived_type, u32 begin, u32 end) {
    if (types().is_dependent(base_type) || types().is_dependent(derived_type)) {
      if (auto deferred = dependent_class_relation(ast::DepOp::is_virtual_base_of, base_type, derived_type, begin, end))
        return *deferred;
      diag_at(type_trait_loc(begin), Diag::class_relation_dependent, name);
      return {NodeId::none, begin, end};
    }
    auto operands = class_relation_operands(name, base_type, derived_type, false, begin, end);
    if (!operands) return operands.error();
    if (auto result = reaches_base(operands->second, operands->first, true)) return bool_literal(*result, begin, end);
    diag_at(type_trait_loc(begin), Diag::class_relation_unsupported, name, base_type, derived_type);
    return {NodeId::none, begin, end};
  }
  // A standard-layout object is pointer-interconvertible with each of its base class subobjects
  // ([basic.compound]), and a standard-layout class has no base type twice ([class.prop]), so
  // the derivation is unambiguous. Access does not matter.
  Ex pointer_interconvertible_base_of_expr(NameId name, QualType base_type, QualType derived_type, u32 begin, u32 end) {
    const engine::Loc loc = type_trait_loc(begin);
    if (types().is_dependent(base_type) || types().is_dependent(derived_type)) {
      if (auto deferred = dependent_class_relation(ast::DepOp::is_pointer_interconvertible_base_of, base_type,
                                                   derived_type, begin, end))
        return *deferred;
      diag_at(loc, Diag::class_relation_dependent, name);
      return {NodeId::none, begin, end};
    }
    auto operands = class_relation_operands(name, base_type, derived_type, true, begin, end);
    if (!operands) return operands.error();
    const auto [b, d] = *operands;
    std::optional<bool> result = reaches_base(d, b);
    if (result == true) result = classes().standard_layout(d);
    if (result) return bool_literal(*result, begin, end);
    diag_at(loc, Diag::class_relation_unsupported, name, base_type, derived_type);
    return {NodeId::none, begin, end};
  }

  // An enumeration's underlying type, once known: while its definition is read, too.
  std::optional<QualType> enum_underlying(EntityId e) {
    for (const EnumRec* record : {q_->defining_enum(e), classes().find_enum(e)})
      if (record && record->underlying) return types().canonical(*record->underlying);
    return std::nullopt;
  }
  // The non-static data members of a standard-layout class: all are declared in one class of its
  // hierarchy ([class.prop]), itself or a base.
  const std::vector<FieldRec>* standard_layout_members(EntityId cls) {
    const ClassRec* record = classes().find(cls);
    if (!record || !record->fields.empty()) return record ? &record->fields : nullptr;
    for (const BaseRec& base : record->bases) {
      const std::vector<FieldRec>* members = standard_layout_members(base.cls);
      if (!members || !members->empty()) return members;
    }
    return &record->fields;
  }
  // Whether cv1 T1 and cv2 T2 are layout-compatible types ([basic.types.general]): the same type,
  // enumerations with the same underlying type ([dcl.enum]), or layout-compatible standard-layout
  // classes ([class.mem.general]). None when a fact it needs is not known.
  std::optional<bool> layout_compatible(QualType first, QualType second);
  // `__is_layout_compatible(T, U)` ([meta.rel]): each operand must be complete, cv void or an
  // array of unknown bound.
  Ex layout_compatible_expr(NameId name, QualType first, QualType second, u32 begin, u32 end);

  // The owning replay checks class completeness at the point of instantiation
  // ([meta.unary.prop]). Among these traits, only finality requires a complete union.
  Ex class_property_trait_expr(ast::DepOp op, QualType type, u32 begin, u32 end)
    pre(op == ast::DepOp::is_polymorphic || op == ast::DepOp::is_abstract || op == ast::DepOp::is_final ||
        op == ast::DepOp::has_virtual_destructor)
  {
    type = types().canonical(type);
    const engine::Loc loc = type_trait_loc(begin);
    if (types().is_dependent(type)) {
      if (q_->in_type_trait_pattern())
        if (auto value = templates().unary_trait_value(op, type)) {
          q_->record_dependent_type_trait_operand(loc);
          return literal(builtin(Builtin::bool_), *value, ValueCategory::prvalue, begin, end);
        }
      if (auto at_satisfaction = trait_at_satisfaction(loc, begin, end)) return *at_satisfaction;
      return dependent_node({}, begin, end, builtin(Builtin::bool_));
    }
    if (!types().is_class(type)) return bool_literal(false, begin, end);
    const EntityId cls = types().class_of(type);
    const bool final_trait = op == ast::DepOp::is_final;
    if (!final_trait && is_union_type(type) == true) return bool_literal(false, begin, end);
    if (auto demand = q_->type_trait_at_demand(cls, loc)) {
      if (!*demand) {
        if (!q_->type_trait_instantiation_failed(cls)) diag_at(loc, Diag::virtual_class_trait_incomplete, type);
        return {NodeId::none, begin, end};
      }
    } else if (!require_complete(type, begin)) {
      return {NodeId::none, begin, end};
    }
    const ClassRec* record = classes().find(cls);
    if (!record) return {NodeId::none, begin, end};
    switch (op) {
    case ast::DepOp::is_polymorphic: return bool_literal(record->polymorphic, begin, end);
    case ast::DepOp::is_abstract: return bool_literal(record->is_abstract(), begin, end);
    case ast::DepOp::is_final: return bool_literal(record->is_final, begin, end);
    case ast::DepOp::has_virtual_destructor: return bool_literal(record->virtual_destructor, begin, end);
    default: std::unreachable();
    }
  }

  // Type-operand builtins ([meta.unary], [meta.rel]), value-dependent where supported.
  Ex trait_expr(lex::TypeTrait t, NameId name, std::span<const QualType> args, u32 begin, u32 end);

  // ------------------------------------------------------------ qualified names ([basic.lookup.qual])

  // What a nested-name-specifier names: a namespace (none for the global one) or a class or
  // enumeration.
  struct Scope {
    std::optional<EntityId> ns;
    std::optional<QualType> type;
  };
  static Scope global_scope() { return {EntityId::none, std::nullopt}; }
  static bool competing_qualifier(EntityId ns, std::span<const ast::DeclSema> declarations) {
    return std::ranges::any_of(declarations, [&](const ast::DeclSema& declaration) {
      return declaration.entity != ns && (declaration.kind == ast::DeclKind::namespace_ || is_type(declaration.kind));
    });
  }
  // The first component of a nested-name-specifier, by unqualified lookup ([basic.lookup.qual]/1:
  // only namespaces, types and templates).
  std::optional<Scope> scope_named(NameId n, std::optional<u32> enum_qualifier_at = std::nullopt,
                                   bool* ambiguous_namespace = nullptr, bool search_block_import_types = true);
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
    // A member alias template hides its bases' members and is no type by itself.
    if (std::ranges::contains(cr->alias_templates, n, &NestedType::name)) return std::nullopt;
    for (const BaseRec& b : cr->bases) {
      if (unqualified && b.dependent_in_pattern) continue;
      if (auto r = nested_member(b.cls, n)) return r;
    }
    return std::nullopt;
  }
  // The member alias template `n` of class `c` or of a base ([temp.mem], [class.member.lookup]):
  // from its record where it is complete, a specialization instantiated for it; or, in the class
  // being defined or instantiated, among the members declared so far ([basic.scope.class]).
  std::optional<ast::DeclSema> member_alias_template(EntityId c, NameId n) {
    if (const ClassRec* cr = class_rec(c)) {
      for (const NestedType& t : cr->alias_templates)
        if (t.name == n) return q_->template_decl(t.entity);
      if (direct_member(c, *cr, n)) return std::nullopt;  // hidden by a member that is not one
      for (const BaseRec& b : cr->bases)
        if (auto r = member_alias_template(b.cls, n)) return r;
      return std::nullopt;
    }
    if (c == current_class) {
      if (auto m = q_->earlier_member(n))  // any earlier member of the name hides the bases'
        return m->kind == ast::DeclKind::type_alias && (m->flags & decl_flag::template_) ? std::optional(*m) : std::nullopt;
      // [temp.dep.general]: a dependent base is not searched.
      for (const BaseRec& b : q_->defining_base_records(c))
        if (!b.dependent_in_pattern)
          if (auto r = member_alias_template(b.cls, n)) return r;
    }
    return std::nullopt;
  }
  static bool is_variable_template(const ast::DeclSema& d) {
    return d.kind == ast::DeclKind::variable && (d.flags & decl_flag::template_);
  }
  // The static data member template `n` of class `c` or of a base ([temp.mem],
  // [class.static.data]): from its record where it is complete, or in the class being defined or
  // instantiated, among the members declared so far ([basic.scope.class]). None when the name
  // finds anything else.
  std::optional<ast::DeclSema> member_variable_template(EntityId c, NameId n, bool unqualified = false) {
    if (class_rec(c)) {
      auto found = find_member(c, n, unqualified);
      if (!found || found->ambiguous || !found->member || found->member->is_function || !found->member->is_template)
        return std::nullopt;
      return q_->template_decl(found->member->entity);
    }
    if (c != current_class) {  // an enclosing class being defined: its members so far
      auto m = q_->enclosing_earlier_member(c, n, /*type_only=*/false);
      return m && is_variable_template(*m) ? m : std::nullopt;
    }
    if (auto m = q_->earlier_member(n))  // any earlier member of the name hides the bases'
      return is_variable_template(*m) ? std::optional(*m) : std::nullopt;
    // [temp.dep.general]: a dependent base is not searched.
    for (const BaseRec& b : q_->defining_base_records(c))
      if (!b.dependent_in_pattern)
        if (auto r = member_variable_template(b.cls, n, unqualified)) return r;
    return std::nullopt;
  }
  // Unqualified lookup of `name` as a static data member template ([basic.lookup.unqual],
  // [class.member.lookup]): of the class whose member is analysed or a base, then of the
  // enclosing classes, innermost first. None when a block-scope name, a block-scope
  // using-declaration, a template parameter or another member of the name is found first.
  std::optional<ast::DeclSema> unqualified_member_variable_template(NameId name, EntityId* naming = nullptr) {
    if (current_class == EntityId::none || template_param(name)) return std::nullopt;
    for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s)
      for (auto p = s->rbegin(); p != s->rend(); ++p)
        if (p->first == name) return std::nullopt;
    if (std::ranges::contains(block_usings_, name, &BlockUsing::name)) return std::nullopt;
    for (EntityId cls = current_class; cls != EntityId::none; cls = enclosing_class(cls)) {
      if (auto m = member_variable_template(cls, name, /*unqualified=*/true)) {
        if (naming) *naming = cls;
        return m;
      }
      const bool other = cls == current_class ? names_member(name)
                         : class_rec(cls)     ? find_member(cls, name, /*unqualified=*/true).has_value()
                                              : q_->enclosing_earlier_member(cls, name, /*type_only=*/false).has_value();
      if (other) return std::nullopt;
    }
    return std::nullopt;
  }
  // Whether static data member template `tmpl`, named unqualified as `name` at `at`, is
  // accessible here ([class.access]); diagnosed if not.
  void unqualified_variable_template_access(const ast::DeclSema& tmpl, NameId name, u32 at) {
    if (!(tmpl.flags & decl_flag::member) || current_class == EntityId::none) return;
    EntityId naming = EntityId::none;
    const auto found = unqualified_member_variable_template(name, &naming);
    if (!found || found->entity != tmpl.entity) return;
    const ClassPrefixScope prefix(*this, current_class, static_initializer_name_);
    check_member_template_access(naming, tmpl.entity, name, at);
  }
  // `object.v<args>` or `pointer->v<args>` naming a static data member template v of the
  // object's class ([expr.ref], [temp.mem]): its declaration, none otherwise.
  std::optional<ast::DeclSema> object_variable_template(const Ex& object, bool arrow, NameId name) {
    if (!object.ok() || dependent(object)) return std::nullopt;
    QualType t = object.type();
    if (arrow) {
      if (!types().is_pointer(t)) return std::nullopt;
      t = types().pointee(t);
    }
    if (!types().is_class(t)) return std::nullopt;
    return member_variable_template(types().class_of(t), name);
  }
  // The specialization `object.v<args>` names: the object expression is evaluated, then
  // discarded, as for any static data member ([expr.ref]/2).
  Ex object_variable_template_id(const Ex& object, bool arrow, const ast::DeclSema& tmpl,
                                 std::span<const TemplateArg> args, u32 name_at, u32 end) {
    const u32 begin = object.begin;
    QualType t = arrow ? types().pointee(object.type()) : object.type();
    const NameId name = w_->entities.rec(tmpl.entity).name;
    check_member_template_access(types().class_of(t), tmpl.entity, name, name_at);
    if (std::ranges::any_of(args, [&](const TemplateArg& a) { return dependent(a); }))
      return dependent_node({object}, begin, end);
    Ex ref = variable_template_id(tmpl, args, name_at, end);
    if (!ref.ok()) return {NodeId::none, begin, end};
    ExprInfo info = ref.info;
    Ex receiver = discarded(object);
    if (!receiver.ok()) return {NodeId::none, begin, end};
    Ex effects = convert_node(receiver, ast::ConversionKind::to_void, builtin(Builtin::void_));
    return node(ast::OrderedBinary{info, ast::OrderedOp::comma, at(effects, begin), at(ref, begin)}, begin, end, info);
  }
  // `C::v<args>` naming a static data member template of class C ([temp.mem]): its declaration.
  // A dependent C other than the current instantiation is known only once it is substituted.
  std::optional<ast::DeclSema> scope_variable_template(const Scope& s, NameId name) {
    if (!s.type || !types().is_class(*s.type)) return std::nullopt;
    const EntityId c = types().class_of(*s.type);
    if (types().is_dependent(*s.type) && c != current_class) return std::nullopt;
    return member_variable_template(c, name);
  }
  // Unqualified lookup skips the current class's dependent direct bases even during replay.
  // A non-template member or an ambiguous lookup still hides outer declarations
  // ([basic.lookup.unqual], [class.member.lookup], [temp.res.general]).
  std::optional<ast::DeclSema> own_member_alias_template(NameId n, bool& found) {
    if (current_class == EntityId::none || template_param(n)) return std::nullopt;
    std::optional<Found> member;
    if (class_rec(current_class)) {
      member = find_member(current_class, n, /*unqualified=*/true);
    } else {
      if (auto m = q_->earlier_member(n)) {
        found = true;
        if (m->kind == ast::DeclKind::type_alias && (m->flags & decl_flag::template_)) return *m;
        return std::nullopt;
      }
      member = find_base_member(q_->defining_base_records(current_class), n, /*unqualified=*/true);
    }
    found = member.has_value();
    if (!member || member->ambiguous || member->nested_type == EntityId::none || member->enumerator != EntityId::none ||
        w_->entities.rec(member->nested_type).kind != EntityKind::alias_template) return std::nullopt;
    return q_->template_decl(member->nested_type);
  }
  std::optional<QualType> member_type(EntityId c, NameId n) {
    if (class_rec(c)) {
      if (auto r = nested_member(c, n)) return r->first.type;
      return std::nullopt;
    }
    if (c == current_class)
      if (auto m = q_->earlier_member(n, /*type_only=*/true); m && !(m->flags & decl_flag::template_)) return m->type;
    for (EntityId cls = current_class; cls != EntityId::none; cls = enclosing_class(cls))
      if (enclosing_class(cls) == c && w_->entities.rec(cls).name == n) return types().class_type(cls);
    if (encloses_current_class(c)) return enclosing_prefix_type(c, n, std::nullopt, /*type_only=*/true).type;
    return std::nullopt;
  }

  std::optional<QualType> direct_member_introduction(EntityId ns, NameId n) {
    if (member_introduction && member_introduction->name == n &&
        w_->entities.rec(member_introduction->sema.entity).scope == ns &&
        q_->namespace_members(ns, n, /*type_only=*/false, /*direct_only=*/true).empty())
      return member_introduction->sema.type;
    return std::nullopt;
  }

  std::optional<Scope> scope_member(const Scope& s, NameId n, bool assume_dependent_type = true,
                                    bool* ambiguous_namespace = nullptr);
  // Whether `S::n` names a function or variable, which hides a type of that name in its scope
  // ([basic.scope.hiding]/2): `::stat(path, &buf)` calls the function.
  bool scope_names_object(const Scope& s, NameId n) {
    if (s.ns) {
      if (direct_member_introduction(*s.ns, n)) return false;
      return std::ranges::any_of(q_->namespace_members(*s.ns, n), [](const ast::DeclSema& d) {
        return d.kind == ast::DeclKind::function || d.kind == ast::DeclKind::variable;
      });
    }
    if (s.type && types().is_class(*s.type))
      if (const ClassRec* c = class_rec(types().class_of(*s.type)))
        return std::ranges::any_of(c->members, [&](const MemberDeclRec& m) { return m.name == n; }) ||
               std::ranges::contains(c->introduced, n, &MemberDeclRec::name);
    return false;
  }
  // The ordinary lookup result can hide an inherited type from a template argument.
  bool scope_names_static_data_member(const Scope& s, NameId n) {
    if (!s.type || !types().is_class(*s.type) || types().is_dependent(*s.type)) return false;
    auto found = find_member(types().class_of(*s.type), n);
    return found && found->member && found->member->is_static && !found->member->is_function;
  }
  // Whether the type `S::n`, named at `at`, is accessible here: a nested type of a class
  // ([class.access]/4); diagnosed if not. Ordinary functional conversions check directly
  // declared aliases and typedefs of non-template records.
  bool type_member_access(const Scope& s, NameId n, u32 at, bool direct_alias_only = false) {
    if (!s.type || !types().is_class(*s.type) || types().is_dependent(*s.type)) return true;
    EntityId naming = types().class_of(*s.type);
    if (direct_alias_only) {
      const EntityRec& record = w_->entities.rec(naming);
      if (record.kind != EntityKind::class_ || record.templ != EntityId::none) return true;
    }
    if (!direct_alias_only && encloses_current_class(naming) && !class_rec(naming)) {
      const auto before = diag_count();
      enclosing_prefix_type(naming, n, at, /*type_only=*/true);
      return !has_error_since(before);
    }
    auto r = nested_member(naming, n);
    if (!r) return true;
    auto [t, c] = *r;
    if (direct_alias_only && (c != naming || w_->entities.rec(t.entity).kind != EntityKind::type_alias ||
                              types().is_dependent(t.type))) return true;
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
    for (const DeferredAccess& d : deferred_access_) {
      if (d.complete_destruction) check_destructor_access(types().class_type(d.cls), d.at);
      else check_access(d.cls, d.member, d.name, d.at);
    }
    deferred_access_.clear();
    for (const auto& check : deferred_injected_access_) check();
    deferred_injected_access_.clear();
    current_function = saved;
  }
  // The function `N::name` of type `t` declared in namespace `ns`, if any ([namespace.memdef]/3:
  // a qualified friend refers to an earlier declaration).
  std::optional<EntityId> namespace_function(EntityId ns, NameId name, QualType t) {
    for (const ast::DeclSema& d : q_->namespace_members(ns, name))
      if (d.kind == ast::DeclKind::function && types().canonical(d.type) == types().canonical(t)) return d.entity;
    return std::nullopt;
  }
  ClassBodyType qualified_injected_type(const Scope& scope, NameId name, std::optional<u32> at, bool type_only) {
    if (!scope.type || !types().is_class(*scope.type) || types().is_dependent(*scope.type)) return {};
    EntityId naming = types().class_of(*scope.type);
    const EntityRec& entity = w_->entities.rec(naming);
    if (entity.kind != EntityKind::class_ || entity.templ != EntityId::none) return {};
    const ClassRec* cls = class_rec(naming);
    const bool enclosing = encloses_current_class(naming);
    if (!cls && naming != current_class && !enclosing) return {};
    if (naming == current_class && (!cls || !cls->defined)) {
      if (q_->earlier_member(name, type_only)) return {};
    }
    if (enclosing) {
      if (entity.name != name)
        return cls && cls->defined ? class_body_type(naming, *cls, name, at, ClassBodyLookup::class_prefix, type_only)
                                   : enclosing_prefix_type(naming, name, at, type_only);
      if ((!cls || !cls->defined) && q_->enclosing_earlier_member(naming, name, type_only)) return {};
    }
    const ClassRec incomplete;
    return class_body_type(naming, cls ? *cls : incomplete, name, at, ClassBodyLookup::qualified, type_only);
  }

  // Qualified namespace lookup for an elaborated type specifier skips non-types.
  std::optional<QualType> namespace_elaborated_type(EntityId ns, NameId name, std::optional<u32> at,
                                                  ast::DeclKind kind) {
    if (kind == ast::DeclKind::class_ && member_introduction && member_introduction->name == name &&
        w_->entities.rec(member_introduction->sema.entity).scope == ns)
      return member_introduction->sema.type;
    std::optional<ast::DeclSema> result;
    for (const ast::DeclSema& d : q_->namespace_members(ns, name, /*type_only=*/true)) {
      if (result && result->entity != d.entity) {
        if (kind == ast::DeclKind::enum_ && types().canonical(result->type) == types().canonical(d.type)) {
          if (d.kind == kind) result = d;
          continue;
        }
        if (at) diag(*at, Diag::ambiguous_name, name);
        return std::nullopt;
      }
      result = d;
    }
    if (result && kind == ast::DeclKind::enum_ && result->kind == ast::DeclKind::type_alias && at)
      diag(*at, Diag::elaborated_typedef, name);
    if (!result || result->kind != kind || (result->flags & decl_flag::template_))
      return std::nullopt;
    return result->type;
  }

  // [basic.lookup.elab]: look past non-types, then require an enum declaration at the terminal name.
  std::optional<QualType> qualified_elaborated_enum(const Scope& scope, NameId name, u32 at) {
    if (scope.ns) return namespace_elaborated_type(*scope.ns, name, at, ast::DeclKind::enum_);
    auto type = enum_path_member(scope, name, at, ClassBodyLookup::qualified_enum);
    return type && types().is_enum(*type) ? type : std::nullopt;
  }

  std::optional<Scope> qualified_enum_scope(const Scope& scope, NameId name, u32 at) {
    if (scope.ns) {
      if (auto type = direct_member_introduction(*scope.ns, name)) return Scope{std::nullopt, *type};
      return qualifier_scope(name, at, q_->qualifier_decls(name, scope.ns, {}));
    }
    auto type = enum_path_member(scope, name, at, ClassBodyLookup::enum_qualifier);
    return type ? std::optional{Scope{std::nullopt, *type}} : std::nullopt;
  }

  std::optional<Scope> qualifier_scope(NameId name, u32 at, std::span<const ast::DeclSema> declarations) {
    std::optional<Scope> result;
    for (const auto& declaration : declarations) {
      Scope found;
      if (declaration.kind == ast::DeclKind::namespace_) found.ns = declaration.entity;
      else if (is_type(declaration.kind) && !(declaration.flags & decl_flag::template_))
        found.type = types().canonical(declaration.type);
      else continue;
      if (result && (result->ns != found.ns || result->type != found.type)) {
        diag(at, Diag::ambiguous_name, name);
        return std::nullopt;
      }
      result = found;
    }
    return result;
  }

  std::optional<QualType> enum_path_member(const Scope& scope, NameId name, u32 at, ClassBodyLookup lookup) {
    if (!scope.type || !types().is_class(*scope.type) || types().is_dependent(*scope.type)) return std::nullopt;
    const EntityId naming = types().class_of(*scope.type);
    const ClassRec* cls = class_rec(naming);
    if (!cls && naming != current_class) return std::nullopt;
    if (naming == current_class && (!cls || !cls->defined))
      if (auto member = q_->earlier_member(name, /*type_only=*/true)) {
        if (lookup == ClassBodyLookup::qualified_enum && member->kind == ast::DeclKind::type_alias) {
          diag(at, Diag::elaborated_typedef, name);
          return std::nullopt;
        }
        return member->type;
      }
    const ClassRec incomplete;
    return class_body_type(naming, cls ? *cls : incomplete, name, at, lookup, /*type_only=*/true).type;
  }

  // `S::n` naming a type.
  std::optional<QualType> scope_type(const Scope& s, NameId n, bool assume_dependent_type = true) {
    auto m = scope_member(s, n, assume_dependent_type);
    if (!m || !m->type) return std::nullopt;
    if (s.type && types().is_class(*s.type) && unscoped_member_enumerator(types().class_of(*s.type), n))
      return std::nullopt;
    return m->type;
  }
  // Ordinary qualified lookup still applies to a typename-specifier ([temp.res.general]).
  std::optional<QualType> namespace_conversion_type(const Scope& s, NameId name, u32 at);
  // The immediate-call lookup already hid tags that share a binding scope with a function.
  bool ambiguous_namespace_call(std::span<const ast::DeclSema> found, NameId name, u32 name_at) {
    auto function = [](const ast::DeclSema& d) { return d.kind == ast::DeclKind::function; };
    if (std::ranges::none_of(found, function) || std::ranges::all_of(found, function)) return false;
    diag(name_at, Diag::ambiguous_name, name);
    return true;
  }

  // `S::n` as an expression ([expr.prim.id.qual]).
  Ex scope_expr(const Scope& s, NameId n, u32 begin, u32 name_at, u32 end, std::optional<u32> address_at = {},
                bool check_all_bases = false, bool immediate_call = false) {
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
      const EntityId cls = types().class_of(*s.type);
      if (!is_complete(*s.type)) {
        if (cls == current_class && in_class_assertion()) {
          if (auto r = incomplete_class_member(n, begin, end, name_at, /*unqualified=*/false, immediate_call, address_at))
            return *r;
          diag(name_at, Diag::no_member, n, *s.type);
          return {NodeId::none, begin, end};
        }
        if (encloses_current_class(cls))
          if (auto r = enclosing_prefix_value(cls, n, begin, end, name_at, /*unqualified=*/false, immediate_call, address_at))
            return *r;
        if (auto r = qualified_class_prefix_value(cls, n, begin, end)) return *r;
        if (!type_trait_failed(*s.type)) diag(begin, Diag::incomplete_type, *s.type);
        return {NodeId::none, begin, end};
      }
      if (auto r = class_member(cls, n, begin, end, name_at, address_at, false, check_all_bases)) return *r;
      diag(name_at, Diag::no_member, n, *s.type);
      return {NodeId::none, begin, end};
    }
    if (namespace_lambda && namespace_lambda->name == n && namespace_lambda->scope == *s.ns)
      return undeduced_namespace_lambda(n, name_at, end);
    if (defining_unnamed_enum_scope == s.ns)
      for (const OwnDecl& d : own)
        if (d.name == n && d.enum_initializer != ConstId::none)
          return literal(d.sema.type, d.enum_initializer, ValueCategory::prvalue, begin, end);
    auto found = q_->namespace_members(*s.ns, n, /*type_only=*/false, /*direct_only=*/false, immediate_call);
    if (immediate_call && ambiguous_namespace_call(found, n, name_at)) return {NodeId::none, begin, end};
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
    // A local class's members are in the blocks around it too ([namespace.udir]/2, [class.local]).
    for (const OuterBlock* b = outer(); b; b = b->outer.get()) r.insert(r.end(), b->directives.begin(), b->directives.end());
    return r;
  }
  void using_declaration(const Scope& s, NameId n, u32 at_);
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
    if (!type_trait_failed(t)) diag(at, Diag::incomplete_type, t);
    return false;
  }
  bool type_trait_failed(QualType t) {
    const QualType element = types().innermost_array_element(t);
    return types().is_class(element) && q_->type_trait_instantiation_failed(types().class_of(element));
  }

  // [basic.def], [basic.lval]: objects other than base subobjects cannot have abstract class type.
  bool require_concrete_object(QualType t, u32 at) {
    if (!q_->may_have_virtual_layouts()) return true;
    const QualType element = types().innermost_array_element(t);
    const ClassRec* record = types().is_class(element) ? class_rec(types().class_of(element)) : nullptr;
    if (!record || !record->is_abstract()) return true;
    diag(at, Diag::abstract_object, element);
    return false;
  }

  // Clang permits a call's abstract result in contexts that would otherwise materialize it.
  bool require_concrete_call_result(QualType t, u32 at) {
    return compatibility() == ast::Compatibility::clang || require_concrete_object(t, at);
  }

  bool require_concrete_parameters(const TypeRec& f, u32 at) {
    return std::ranges::all_of(f.params, [&](QualType p) { return require_concrete_object(p, at); });
  }

  bool require_supported_virtual_layout(QualType t, u32 at) {
    if (!q_->may_have_virtual_layouts()) return true;
    const QualType element = types().innermost_array_element(t);
    // Layout can be the first use that requires a class specialization to be complete.
    const ClassRec* record = types().is_class(element) ? class_rec(types().class_of(element)) : nullptr;
    if (!record || !record->virtual_layout_unsupported) return true;
    if (record->polymorphic && !record->member_declarations_valid) return true;  // diagnosed at the member
    diag(at, Diag::virtual_layout, t);
    return false;
  }

  bool has_peer_virtual_layout(QualType t) {
    if (!q_->may_have_virtual_layouts() || !types().is_class(t)) return false;
    const ClassRec* record = class_rec(types().class_of(t));
    return record && record->peer_virtual_layout != PeerVirtualLayout::none;
  }

  bool has_peer_automatic_layout(QualType t) {
    if (!q_->may_have_virtual_layouts() || !types().is_class(t)) return false;
    const ClassRec* record = class_rec(types().class_of(t));
    return record && (record->peer_virtual_layout == PeerVirtualLayout::root ||
                      record->peer_automatic_primary_base != EntityId::none);
  }

  bool require_supported_virtual_signature(const TypeRec& f, u32 at, bool call = false) {
    return (call ? require_concrete_call_result(f.inner, at) : require_concrete_object(f.inner, at)) &&
           require_concrete_parameters(f, at) &&
           require_supported_virtual_layout(f.inner, at) && require_supported_virtual_parameters(f, at);
  }

  bool require_supported_virtual_parameters(const TypeRec& f, u32 at) {
    return std::ranges::all_of(f.params, [&](QualType p) {
      return require_supported_virtual_layout(p, at);
    });
  }

  // [class.dtor]: use the class selected by the receiver and its destructor name.
  Ex explicit_destroy(Ex object, bool arrow, NameId name, u32 end, u32 op_at, u32 name_at);

  // [expr.ref]: `object.name`, or `object->name` with `arrow`.
  // Whether `object.name` names a member function template ([temp.names]/3): the object's class
  // has one of that name.
  bool names_member_template(const Ex& object, bool arrow, NameId name) {
    if (!object.ok() || dependent(object)) return false;
    QualType t = object.type();
    if (arrow) {
      if (!types().is_pointer(t)) return false;  // The actual member access diagnoses this receiver.
      t = types().pointee(t);
    }
    if (!types().is_class(t)) return false;
    const ClassRec* c = class_rec(types().class_of(t));
    if (c && std::ranges::any_of(std::array{std::span(c->members), std::span(c->introduced)} | std::views::join,
                                 [&](const MemberDeclRec& m) { return m.name == name && m.is_template; }))
      return true;
    // Or one of a base ([class.member.lookup]).
    auto found = c ? find_member(types().class_of(t), name) : std::nullopt;
    return found && !found->ambiguous && found->member && found->member->is_template;
  }
  // Explicit template arguments for the function templates `e` names, a member or an overload set
  // ([temp.arg.explicit]).
  void explicit_template_args(const Ex& e, std::vector<TemplateArg> args) {
    if (e.ok() && (is_bound_member(e) || types().is(e.type(), Builtin::overload_set)))
      explicit_args_[std::to_underlying(e.id)] = std::move(args);
  }
  bool class_has_member_template(EntityId c, NameId name) {
    const ClassRec* r = class_rec(c);
    if (!r && c != EntityId::none && c == current_class) {  // the class being defined: its members so far
      std::vector<ast::DeclSema> functions;
      const auto last = q_->earlier_member(name, /*type_only=*/false, &functions);
      return std::ranges::any_of(functions, [](const ast::DeclSema& d) { return (d.flags & decl_flag::template_) != 0; }) ||
             (last && is_variable_template(*last));
    }
    return r && std::ranges::any_of(std::array{std::span(r->members), std::span(r->introduced)} | std::views::join,
                                    [&](const MemberDeclRec& m) { return m.name == name && m.is_template; });
  }
  // Whether `S::name` names a function template ([temp.names]/3): a member template of class S,
  // or a function template of namespace S.
  bool scope_names_template(const Scope& sc, NameId name) {
    if (sc.ns) return namespace_template(*sc.ns, name, ast::DeclKind::function).has_value();
    return sc.type && types().is_class(*sc.type) && class_has_member_template(types().class_of(*sc.type), name);
  }
  bool namespace_names_call_template(EntityId ns, NameId name) {
    return std::ranges::any_of(q_->namespace_members(ns, name, false, false, true), [](const ast::DeclSema& d) {
      return d.kind == ast::DeclKind::function && (d.flags & decl_flag::template_);
    });
  }
  // A class lvalue reached through an existing pointer or reference. Prvalues and direct
  // references to class storage still use the ordinary layout guard.
  bool peer_object_lvalue(Ex object) {
    if (object.cat() != ValueCategory::lvalue) return false;
    for (;;) {
      if (auto p = as<ast::Paren>(object)) {
        object.id = p->operand.id;
      } else if (auto c = as<ast::ImplicitConversion>(object); c && c->conversion == ast::ConversionKind::derived_to_base) {
        object.id = c->operand.id;
      } else {
        break;
      }
    }
    if (auto local = as<ast::LocalRef>(object)) return types().is_reference(local_declared_type(locals_[local->local]));
    if (auto decl = as<ast::DeclRef>(object)) return decl->reference;
    if (auto unary = as<ast::Unary>(object)) return unary->op == ast::UnaryOp::deref;
    if (auto field = as<ast::Member>(object)) return field->reference;
    return as<ast::Call>(object).has_value() || as<ast::MemberCall>(object).has_value();
  }

  bool default_constructed_peer_object(Ex object) {
    while (const auto paren = as<ast::Paren>(object)) object.id = paren->operand.id;
    if (const auto local = as<ast::LocalRef>(object))
      if (const auto declaration = as<ast::LocalVar>({locals_[local->local].decl}))
        return as<ast::PeerRootInit>({declaration->init.id}).has_value();
    return false;
  }

  Ex member(Ex object, bool arrow, NameId name, u32 end, u32 op_at, u32 name_at,
            std::optional<NameId> qualifier = {}, bool retain_class_parameter = false);

  QualType field_access_type(const FieldRec& field, QualType object) {
    if (types().is_reference(field.type)) return types().referee(field.type);
    Cv cv = Types::cv(object);
    if (field.mutable_) cv = Types::is_volatile(object) ? Cv::volatile_ : Cv::none;
    return types().add_cv(field.type, cv);
  }

  // ------------------------------------------------------------ initialization ([dcl.init])

  static bool is_aggregate_class(const ClassRec& c) {
    // [dcl.init.aggr]/1: no user-declared constructors or virtual functions; a closure type is none
    // ([expr.prim.lambda.closure]/1).
    return !c.user_ctors && !c.closure && !c.polymorphic;
  }

  // An aggregate ([dcl.init.aggr]): an array or a class with the required properties.
  bool is_aggregate(QualType t) {
    if (types().is_array(t)) return true;
    if (!types().is_class(t)) return false;
    const ClassRec* c = class_rec(types().class_of(t));
    return !c || is_aggregate_class(*c);
  }
  bool has_user_ctors(QualType t) {
    const ClassRec* c = types().is_class(t) ? class_rec(types().class_of(t)) : nullptr;
    return c && (c->user_ctors || c->closure || c->dynamic);
  }
  // A class whose implicit default constructor is a function of its own: a dynamic class, or one
  // with a dynamic subobject (#309).
  bool has_implicit_constructor(QualType t) {
    const ClassRec* c = types().is_class(t) ? class_rec(types().class_of(t)) : nullptr;
    return c && c->implicit_default_ctor != EntityId::none;
  }

  // [dcl.init.general]: the supported aggregate elements need no cleanup during unwinding.
  bool can_parenthesize_aggregate(QualType t, std::span<const Ex> args, bool allow_base_copy);

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
    EntityId default_member = EntityId::none;  // the field whose initializer was written
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
        r.push_back({types().add_cv(f.type, Types::cv(t)), f.name, f.init, f.dmi ? f.entity : EntityId::none});
        if (c->union_) break;  // [dcl.init.aggr]/16: a union's list initializes its first member
      }
    }
    return r;
  }
  bool is_union(QualType t) {
    const ClassRec* c = types().is_class(t) ? class_rec(types().class_of(t)) : nullptr;
    return c && c->union_;
  }

  bool ordinary_default_member_owner(EntityId c) {
    const EntityRec& entity = w_->entities.rec(c);
    return entity.kind == EntityKind::class_ && entity.templ == EntityId::none && entity.name != no_name() &&
           (entity.scope == EntityId::none || w_->entities.rec(entity.scope).kind == EntityKind::namespace_);
  }

  // [class.base.init], [dcl.init.aggr]: a destination with only ordinary scalar fields
  // needs no subobject destruction if an initializer exits by throwing.
  bool can_replay_default_members(QualType t) {
    const bool required_member_initializer = static_constexpr_initializer_ && constant_reference_context && lambdas_.empty();
    if (!types().is_class(t) || (templated() && !in_function_body && !required_member_initializer)) return false;
    EntityId c = types().class_of(t);
    if (!ordinary_default_member_owner(c)) return false;
    const ClassRec* rec = class_rec(c);
    return rec && !rec->union_ && !rec->closure && !rec->polymorphic && rec->bases.empty() &&
           rec->destructor_usable && rec->destructor == EntityId::none &&
           std::ranges::all_of(rec->fields, [&](const FieldRec& f) {
             return types().is_scalar(f.type) && !types().is_member_pointer(f.type) && f.bit_width == 0;
           }) && std::ranges::any_of(rec->fields, &FieldRec::dmi);
  }

  struct StaticConstexprInitializerScope {
    Sema& sema;
    bool saved;
    StaticConstexprInitializerScope(Sema& s, bool enabled)
        : sema(s), saved(std::exchange(s.static_constexpr_initializer_, enabled)) {}
    ~StaticConstexprInitializerScope() { sema.static_constexpr_initializer_ = saved; }
  };

  struct DefaultMemberScope {
    Sema& sema;
    bool saved;
    DefaultMemberScope(Sema& s, bool enabled) : sema(s), saved(std::exchange(s.replay_default_members_, enabled)) {}
    ~DefaultMemberScope() { sema.replay_default_members_ = saved; }
  };

  // Whether initializing a `t` runs a default member initializer.
  // (or a constructor).
  bool has_member_inits(QualType t) {
    TypeRec r = types().rec(t);
    if (r.kind == TypeKind::array) return has_member_inits(r.inner);
    if (r.kind != TypeKind::class_) return false;
    if (const ClassRec* c = class_rec(r.entity)) {
      if (c->user_ctors || c->implicit_default_ctor != EntityId::none) return true;
      for (const BaseRec& b : c->bases)
        if (has_member_inits(types().class_type(b.cls))) return true;
      for (const FieldRec& f : c->fields)
        if (f.init != ConstId::none || f.dmi || (!c->union_ && has_member_inits(f.type))) return true;
    }
    return false;
  }

  // Value-initialization ([dcl.init.general]) of an object of type `t`. An aggregate whose
  // members have default member initializers gets them (the implicit default constructor's).
  Ex value_init(QualType t, u32 begin, u32 end) {
    if (types().is_dependent(t)) return dependent_node({}, begin, end);
    if (!require_concrete_object(t, begin) || !require_supported_virtual_layout(t, begin)) return {NodeId::none, begin, end};
    if (is_data_member_pointer(t)) return null_member_pointer(t, begin, end);
    if (types().is_member_pointer(t)) {
      diag(begin, Diag::member_pointer_values);
      return {NodeId::none, begin, end};
    }
    if (has_user_ctors(t) || has_implicit_constructor(t)) return construct(t, {}, begin, end);  // the default constructor
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
    if (!require_concrete_object(t, begin) || !require_supported_virtual_layout(t, begin)) return {NodeId::none, begin, end};
    if (has_user_ctors(t) || has_implicit_constructor(t)) return construct(t, {}, begin, end, Construction::default_init);
    if (has_member_inits(t)) return member_inits(t, false, begin, end);
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    return node(ast::DefaultInit{info}, begin, end, info);
  }

  // The sole constructor work in this profile is installing the external vtable address point.
  // The directly written field has no initializer and the implicit destructor is trivial.
  Ex peer_default_init(QualType t, u32 begin, u32 end) {
    if (!has_peer_automatic_layout(t) || Types::cv(types().canonical(t)) != Cv::none ||
        current_function == EntityId::none || current_class != EntityId::none || !lambdas_.empty() ||
        templated() || unevaluated || immediate_context || q_->is_constexpr(current_function))
      return {};
    const EntityId cls = types().class_of(t);
    const auto& entity = w_->entities.rec(cls);
    const auto* record = classes().find(cls);
    if (!record || entity.linkage != ast::Linkage::external || !record->destructor_usable || record->default_deleted ||
        record->fields.size() != 1) return {};
    if (record->peer_automatic_primary_base == EntityId::none &&
        std::ranges::count(record->members, true, &MemberDeclRec::is_virtual) != 2) return {};
    const auto& field = record->fields.front();
    if (record->access_of(field.entity) != ast::Access::public_ || field.dmi || field.init != ConstId::none ||
        Types::is_const(types().canonical(field.type))) return {};

    const ExprInfo info{t, ValueCategory::prvalue, 0};
    return node(ast::PeerRootInit{info, virtual_table_entity(*w_, cls)}, begin, end, info);
  }

  bool is_defaulted_tag_initializer(Ex init) {
    while (auto paren = as<ast::Paren>(init)) init.id = paren->operand.id;
    if (!(as<ast::ValueInit>(init) || as<ast::DefaultInit>(init)) || !types().is_class(init.type())) return false;
    const ClassRec* rec = class_rec(types().class_of(init.type()));
    return rec && rec->trivial_defaulted_default_ctor && rec->fields.empty() && rec->bases.empty() &&
           rec->destructor == EntityId::none;
  }
  // An aggregate's elements each from its default member initializer, else value- or
  // default-initialized.
  Ex member_inits(QualType t, bool value, u32 begin, u32 end) {
    const bool replay = (value || replay_default_members_) && can_replay_default_members(t);
    std::vector<Element> elems = elements(t);
    if (replay && std::ranges::any_of(class_rec(types().class_of(t))->fields, [&](const FieldRec& f) {
          return !f.dmi && (Types::is_const(types().canonical(f.type)) ||
                            (!value && Types::is_const(types().canonical(t))));
        })) {
      diag(begin, Diag::deleted_constructor, t);
      return {NodeId::none, begin, end};
    }
    std::vector<Ex> out;
    for (const Element& e : elems) {
      out.push_back(element_default(e, value, replay, begin, end));
      if (!out.back().ok()) return {NodeId::none, begin, end};
    }
    return list_of(t, std::move(out), begin, end);
  }
  Ex element_default(const Element& e, bool value, bool replay, u32 begin, u32 end,
                     EntityId required_base = EntityId::none);
  Ex list_of(QualType t, std::vector<Ex> out, u32 begin, u32 end) {
    std::vector<Child> cs;
    for (const Ex& e : out) cs.push_back({e.id, e.begin >= begin ? e.begin - begin : 0});
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    Ex r = node(ast::InitList{info, b_->list(cs)}, begin, end, info);
    list_elems_[std::to_underlying(r.id)] = std::move(out);
    return r;
  }

  // List-initialization of an object of type `t` from `b` ([dcl.init.list]/3).
  Ex list_init(QualType t, const Braced& b, Construction form = Construction::direct_list,
               bool extend_reference = false);

  // The elements of aggregate `t` from the clauses of `b` starting at `next`, which advances past
  // those used. Called for a braced list, and for brace elision ([dcl.init.aggr]/16), where the
  // sub-aggregate takes as many clauses as it has elements.
  Ex aggregate(QualType t, const Braced& b, std::size_t& next, u32 begin, u32 end);

  // [dcl.init.general]: copy-initialize each element from the corresponding expression.
  // Omitted elements use their default member initializers or are value-initialized.
  Ex parenthesized_aggregate(QualType t, std::span<const Ex> args, u32 begin, u32 end);
  // The elements of an `InitList` this frame built, as analysed.
  std::span<const Ex> list_elements(const Ex& list) const {
    auto it = list_elems_.find(std::to_underlying(list.id));
    if (it == list_elems_.end()) return {};
    return it->second;
  }

  // Keep the written origin and the brace-relative child locations. The normalized list
  // also contains synthesized initializers, so its elements alone cannot prove this form.
  void note_explicit_aggregate_prvalue(QualType written, const Braced& braces, const Ex& init, u32 begin) {
    if (!init.ok() || !types().is_class(written) || Types::cv(types().canonical(written)) != Cv::none ||
        init.begin != braces.begin || init.end != braces.end || !as<ast::InitList>(init) ||
        braces.clauses.size() != list_elements(init).size() ||
        std::ranges::any_of(braces.clauses, [](const Braced::Clause& c) { return c.list || c.designated; })) return;
    explicit_aggregate_prvalues_.insert_or_assign(begin, init);
  }
  const Ex* explicit_aggregate_prvalue(const Ex& e) const {
    auto found = explicit_aggregate_prvalues_.find(e.begin);
    if (found == explicit_aggregate_prvalues_.end() || found->second.begin != e.begin + 1 ||
        found->second.id != e.id || found->second.end != e.end)
      return nullptr;
    return &found->second;
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
  bool narrows(const Ex& from, QualType to);

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
    // supported format covers 128-bit integers).
    if (!v || !ins().values.is<ast::VInt>(*v)) return true;
    IntShape sa = shape(w_->target, a);
    if (a == Builtin::bool_) sa = {1, false};
    ast::Bits128 x = ast::wrap(ast::int_bits(ins(), *v), sa.bits, sa.is_signed);
    ast::Bits128 magnitude = sa.is_signed && ast::is_negative(x) ? ast::wide::neg(x) : x;
    if (ast::wide::is_zero(magnitude)) return false;
    u32 significant = ast::wide::bit_width(magnitude) - ast::wide::countr_zero(magnitude);
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
    if (!(flags & local_flag::parameter) && name != no_name() && declared_template_param(name)) diag(at, Diag::redeclared_local, name);
    if (name != no_name() && check) {
      const std::size_t depth = scopes_.size() - 1;
      bool clash = std::ranges::any_of(scopes_.back(), [&](auto& p) { return p.first == name; }) ||
                   std::ranges::any_of(block_types_, [&](auto& t) { return t.depth == depth && t.name == name; }) ||
                   std::ranges::any_of(block_usings_, [&](const BlockUsing& used) {
                     return used.depth == depth && used.name == name &&
                            std::ranges::any_of(used.decls, [](const ast::DeclSema& decl) {
                              return is_type(decl.kind) && !is_non_template_tag(decl);
                            });
                   });
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
  void set_binding_type(u32 local, QualType referenced) {
    if (types().is_reference(referenced)) {
      locals_[local].type = referenced;
      locals_[local].flags |= ast::local_flag::binding_reference_member;
    } else {
      locals_[local].type = types().lvalue_ref(referenced);
    }
  }
  // A local usable in constant expressions ([expr.const]): constexpr, or const integral with a
  // constant initializer; its value.
  void set_local_value(u32 local, ConstId v) { local_values_[local] = v; }
  void set_local_value_dependent(u32 local) { locals_[local].flags |= local_flag::value_dependent; }
  std::span<const std::optional<ConstId>> local_values() const { return local_values_; }
  const ast::LocalRec& local(u32 i) const { return locals_[i]; }
  // [dcl.type.decltype]/1: a binding gives its referenced type; its reference slot is internal.
  QualType local_declared_type(const ast::LocalRec& local) {
    if ((local.flags & ast::local_flag::structured_binding) && !(local.flags & ast::local_flag::binding_reference_member))
      return types().referee(local.type);
    return local.type;
  }
  std::span<const ast::LocalRec> locals() const { return locals_; }
  // Names used only for declarator or requires-expression checking leave no runtime locals.
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
  bool replaying_type_default = false;
  bool replaying_alias_type_arguments = false;
  // The template parameter `name` declares in the template parameter scopes enclosing this point,
  // whether or not a class member hides it: what [temp.local]/6 forbids redeclaring.
  const ast::TemplateParamSema* declared_template_param(NameId name) const {
    for (auto s = template_scopes.rbegin(); s != template_scopes.rend(); ++s)
      for (const ast::TemplateParamSema& p : *s)
        if (p.name == name) return &p;  // an unnamed parameter's name is never looked up
    return nullptr;
  }
  // The template parameter unqualified lookup finds for `name`. The template parameter scope of a
  // class template, or of a definition of its member outside it, is searched after the class's
  // scope with its nondependent bases ([temp.local]/7), so a member of either hides a parameter of
  // a head enclosing the class. A member template's own head, inside the class, is searched first.
  const ast::TemplateParamSema* template_param(NameId name) {
    const ast::TemplateParamSema* p = declared_template_param(name);
    return p && p->depth < enclosing_class_heads() && class_scope_declares(name) ? nullptr : p;
  }
  // How many template-heads enclose the class whose scope this is, its own included: the depths of
  // the parameters its scope is inside. An explicit specialization has none of its own.
  u16 enclosing_class_heads() {
    u16 n = 0;
    for (EntityId c = current_class; c != EntityId::none;) {
      const EntityRec& r = w_->entities.rec(c);
      if (r.kind != EntityKind::class_) break;
      if (r.templ != EntityId::none && !q_->explicit_specialization(c)) ++n;
      c = r.scope;
    }
    return n;
  }
  // Whether a class scope before the template parameter scope declares `name`, including
  // each enclosing class's nondependent bases ([temp.local]/7).
  bool class_scope_declares(NameId name) {
    for (EntityId cls = current_class; cls != EntityId::none; cls = enclosing_class(cls)) {
      const ClassRec* cr = class_rec(cls);
      if (cr && cr->defined) {
        if (direct_member(cls, *cr, name)) return true;
      } else if (cls == current_class ? q_->earlier_member(name).has_value()
                                     : q_->enclosing_earlier_member(cls, name, /*type_only=*/false).has_value()) {
        return true;
      }
      const auto bases = cr && cr->defined ? std::span<const BaseRec>(cr->bases) : q_->defining_base_records(cls);
      if (std::ranges::any_of(bases, [&](const BaseRec& b) { return !b.dependent_in_pattern && base_declares(b.cls, name); }))
        return true;
    }
    return false;
  }
  bool base_declares(EntityId c, NameId name) {
    if (w_->entities.rec(c).name == name) return true;
    const ClassRec* cr = class_rec(c);
    if (!cr) return false;
    if (direct_member(c, *cr, name)) return true;
    return std::ranges::any_of(cr->bases, [&](const BaseRec& b) { return base_declares(b.cls, name); });
  }
  // `TT<args>` of template template parameter `p` ([temp.names]): bound, the specialization of its
  // argument; otherwise a dependent type.
  std::optional<QualType> template_param_id(const ast::TemplateParamSema& p, std::span<const TemplateArg> args, u32 at);
  // The template template parameter `name` names here, if it does.
  const ast::TemplateParamSema* template_template_param(NameId name) {
    const ast::TemplateParamSema* p = template_param(name);
    return p && p->kind == ast::TemplateArgKind::template_ && !p->concept_template ? p : nullptr;
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
  // `C::M` or `Q::template M` as a template template argument ([temp.arg.template]/1,
  // [temp.names]/4): C's member alias template; with Q dependent, Q's member template once Q is
  // known, which needs the keyword.
  std::optional<TemplateArg> member_template_name_arg(const Scope& s, NameId name, bool keyword,
                                                      std::optional<u32> access_at) {
    if (!s.type) return std::nullopt;
    const QualType q = *s.type;
    const bool dependent = types().is_dependent(q);
    if (types().is_class(q) && (!dependent || types().class_of(q) == current_class))
      if (auto d = member_alias_template(types().class_of(q), name)) {
        if (access_at) member_template_access(q, d->entity, name, *access_at);
        return TemplateArg{ast::TemplateArgKind::template_, QualType{}, ConstId::none, {},
                           templates().template_argument_entity(*d)};
      }
    if (!dependent || !keyword) return std::nullopt;
    return TemplateArg{ast::TemplateArgKind::template_, types().dep_template(q, name, ast::DepNameForm::template_name),
                       ConstId::none};
  }
  // `C::M<args>` or `Q::template M<args>` naming a member alias template's specialization
  // ([temp.mem], [temp.names]/4): its declaration, or none with `dependent` set when Q is
  // dependent and the keyword says M is a template.
  std::optional<ast::DeclSema> member_template_scope(const Scope& s, NameId name, bool keyword, bool& dependent) {
    dependent = false;
    if (!s.type) return std::nullopt;
    const QualType q = *s.type;
    const bool dep = types().is_dependent(q);
    if (types().is_class(q) && (!dep || types().class_of(q) == current_class))
      if (auto d = member_alias_template(types().class_of(q), name)) return d;
    // A dependent class whose pattern declares M, the current instantiation named outside its
    // class (`Y<T>::M<args>` in a member's definition, [temp.names]/3), needs no keyword.
    dependent = dep && (keyword || (types().is_class(q) && member_alias_template(types().class_of(q), name)));
    return std::nullopt;
  }
  // Whether member alias template `templ`, named as `Q::M` at `at`, is accessible here
  // ([class.access]/4); diagnosed if not, or deferred as a nested type's check is.
  void member_template_access(QualType q, EntityId templ, NameId n, u32 at) {
    if (!types().is_class(q) || types().is_dependent(q)) return;
    check_member_template_access(types().class_of(q), templ, n, at);
  }
  // Unqualified lookup can find a member template before the naming class is complete
  // ([class.access.general], [class.access.base]). Its declaration still supplies the access.
  void unqualified_type_template_access(const ast::DeclSema& tmpl, NameId n, u32 at) {
    if (current_class == EntityId::none || tmpl.kind != ast::DeclKind::type_alias || !(tmpl.flags & decl_flag::member))
      return;
    const ClassPrefixScope prefix(*this, current_class, static_initializer_name_);
    check_member_template_access(current_class, tmpl.entity, n, at);
  }
  void check_member_template_access(EntityId naming, EntityId templ, NameId n, u32 at) {
    const EntityId owner = w_->entities.rec(templ).scope;
    const ClassRec* cr = class_rec(owner);
    if (!cr) return;
    if (defer_type_access && (!accessible(owner, cr->access_of(templ), naming) || !accessible_naming_base(naming, owner))) {
      deferred_access_.push_back({owner, templ, n, at});
      return;
    }
    check_access(owner, templ, n, at, naming);
  }
  // `typename Q::template M<args>` with Q dependent ([temp.dep.type]): known once Q is.
  QualType dependent_member_template_id(QualType q, NameId name, std::span<const TemplateArg> args) {
    return types().dep_template(q, name, ast::DepNameForm::template_id, args);
  }
  std::optional<TemplateArg> template_name_arg(std::optional<EntityId> ns, NameId name,
                                                std::optional<u32> at = std::nullopt);
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
  bool pattern_names_pack() const { return !pattern_packs_.back().empty(); }
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
  // After one element of an expansion is read in an instantiation (begun with `begin_pattern`):
  // the packs the expansion names are bound to that element, so only the others are the enclosing
  // operand's. Without this, `h(g(xs...))` reported `xs` as unexpanded in `h`'s argument.
  void end_element(const std::vector<PackRef>& expanded) {
    std::vector<PackRef> packs = std::move(pattern_packs_.back());
    pattern_packs_.pop_back();
    for (const PackRef& r : packs)
      if (!std::ranges::contains(expanded, r)) note_pack(r);
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
  std::optional<std::vector<TemplateArg>> expand_bound(std::span<const TemplateArg> args, bool expand_empty = false) {
    Templates t = templates();
    bool any = false;
    std::vector<TemplateArg> out;
    for (const TemplateArg& a : args) {
      auto n = t.is_expansion(a) ? t.expansion_length(t.pattern_of(a), template_args) : std::nullopt;
      if (!n || (!*n && !expand_empty)) {
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
  std::vector<ast::TemplateTypeSpelling> template_type_spellings;

  Templates templates();
  Templates templates_for_use(u32 at) {
    auto t = templates();
    t.member_constant = [this, at](QualType qualifier, NameId name) {
      return qualified_member_constant(qualifier, name, at);
    };
    t.concept_constant = [this, at](EntityId entity, std::span<const TemplateArg> args) {
      return evaluate_dependent_concept(entity, args, at);
    };
    t.call_constant = [this, at](EntityId templ, std::span<const TemplateArg> args) {
      return evaluate_dependent_call(templ, args, at);
    };
    t.alias_type = [this, at](const ast::DeclSema& alias, std::span<const TemplateArg> args, const Bindings& b,
                            AliasSubstitution use) {
      return substitute_alias(alias, args, b, at, use);
    };
    t.demand_variable = [this, at](EntityId spec, QualType type) {
      demands.push_back({spec, ast::Loc{item_, loc_base_ + at}, type});
    };
    return t;
  }
  bool dependent(const Ex& e) {
    return e.ok() && ((e.info.flags & expr_flag::dependent) || types().is_dependent(e.type()));
  }
  // `Builtin::dependent` also stands in for types of expressions whose values alone depend on
  // a template argument. Only a structural type or its expression's producer proves dependence.
  bool has_proven_dependent_type(const Ex& e) {
    return (e.info.flags & expr_flag::type_dependent) ||
           (types().is_dependent(e.type()) && templates().structural_operand(e.type()));
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
  // a template parameter, else of the dependent type. Preserve any decay-copy that still
  // needs replay before the expression can supply a canonical value.
  Ex dependent_node(std::span<const Ex> ops, u32 begin, u32 end, std::optional<QualType> t = std::nullopt,
                    ValueCategory cat = ValueCategory::prvalue, u8 flags = 0) {
    std::vector<Child> cs;
    for (const Ex& o : ops)
      if (o.ok()) {
        cs.push_back({o.id, o.begin >= begin ? o.begin - begin : 0});
        flags |= o.info.flags & expr_flag::deferred_decay_copy;
      }
    ExprInfo info{t ? *t : builtin(Builtin::dependent), cat, u8(expr_flag::dependent | flags)};
    return node(ast::Dependent{info, b_->list(cs)}, begin, end, info);
  }
  inline Ex dependent_node(std::initializer_list<Ex> ops, u32 begin, u32 end, std::optional<QualType> t = std::nullopt,
                    ValueCategory cat = ValueCategory::prvalue, u8 flags = 0) {
    return dependent_node(std::span<const Ex>(ops.begin(), ops.size()), begin, end, t, cat, flags);
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
  bool is_template_name(NameId name);

  // A template-id naming a function template ([temp.names]): the overload set of `name`, with its
  // explicitly specified template arguments ([temp.arg.explicit]).
  Ex template_id(NameId name, std::vector<TemplateArg> args, u32 begin, u32 end) {
    if (auto v = variable_template(name)) {
      unqualified_variable_template_access(*v, name, begin);
      return variable_template_id(*v, args, begin, end);
    }
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

  std::optional<Ex> dependent_immediate_template_call(Ex callee, std::span<const TemplateArg> args, u32 end,
                                                      bool allow_class_result);

  std::optional<Ex> dependent_immediate_call(Ex callee, std::span<const Ex> args, u32 end, bool template_default);

  Ex dependent_call_value(EntityId callee, QualType type, std::span<const TemplateArg> args, u32 begin, u32 end) {
    const auto result = Types::unqualified(types().canonical(type));
    auto value = ins().intern_value(ast::VDepCall{result, callee, ins().intern_seq(args)});
    return literal(type, value, ValueCategory::prvalue, begin, end);
  }

  std::optional<TemplateArg> evaluate_dependent_call(EntityId function, std::span<const TemplateArg> args, u32 at,
                                                      bool diagnose = false);

  std::optional<TemplateArg> evaluate_immediate_template_default(EntityId function, std::span<const TemplateArg> args,
                                                                 QualType target, u32 at) {
    auto result = evaluate_dependent_call(function, args, at, /*diagnose=*/true);
    if (!result) return std::nullopt;
    auto converted = templates().converted_value(*result, target);
    if (!converted) {
      if (types().is_integral(result->type) && types().is_integral(target))
        diag(at, Diag::template_argument_narrowing, result->value, result->type, target);
      else
        diag(at, Diag::no_conversion, result->type, target);
    }
    return converted;
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
    bool static_ = false;
    // Its noexcept-specifier ([expr.prim.lambda.closure]/4, [except.spec]): non-throwing, or in a
    // template's definition, a condition that only the template's instantiations decide.
    bool nonthrowing = false;
    bool noexcept_dependent = false;
    std::optional<QualType> trailing;
    // A generic lambda's template-head, explicit and invented ([expr.prim.lambda.closure]/7): the
    // parser pushed it onto `template_scopes`, and pops it after the body.
    std::vector<ast::TemplateParamSema> head;
    ConstId constraints = ConstId::none;
    ast::WrittenConstraint head_constraint{};
    ast::WrittenConstraint trailing_constraint{};
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
    bool static_ = false;
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
    NodeId pending_result = NodeId::none;  // the checked functional cast at the current return
    bool polymorphic_result = false;
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
  Ex captured(u32 ctx, u32 key, NameId name, u32 begin, u32 end);
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
  std::optional<u32> add_capture(LambdaFrame& f, u32 ctx, u32 key, NameId name, bool by_ref, Ex src);
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
  // The type an init-capture declares, as `auto name = init;` or `auto& name = init;` would
  // ([expr.prim.lambda.capture]/6), for the lambda-declarator, which is in its scope
  // ([basic.scope.lambda]); none for any other capture, or where the deduction fails (the
  // lambda's own analysis says why).
  std::optional<QualType> init_capture_type(const CaptureSpec& c) {
    using K = CaptureSpec::Kind;
    if ((c.kind != K::init_copy && c.kind != K::init_ref) || !c.init.ok()) return std::nullopt;
    QualType p = types().placeholder(false);
    if (c.kind == K::init_ref) p = types().lvalue_ref(p);
    const auto before = diag_count();
    auto t = deduce_placeholder(p, c.init, c.init.begin);
    truncate_diags(before);
    return t;
  }
  bool open_lambda(LambdaSpec spec);
  // What owns the closure types of the lambdas in a declaration part, outside every function body
  // (docs/design/closures.md, decision 3): the variable whose initializer is analysed; or, for a
  // constraint-expression, which is no declaration's, the scope around it, with a discriminator
  // that tells its closures from those of other constraints and of other substitutions.
  struct ClosureOwner {
    EntityId scope = EntityId::none;
    ast::Linkage linkage = ast::Linkage::internal;
    Fp128 discriminator{};
  };
  std::optional<ClosureOwner> closure_owner;
  // The owner of a closure type of a lambda in an unevaluated operand of a declaration part that
  // no `closure_owner` covers (#1549): TU-local, in the class or namespace around it, like a
  // constraint's (docs/design/closures.md, "Closures of declarations" 2). Its discriminator tells
  // the item's closures from other items' and each substitution's from the others'
  // ([expr.prim.lambda.closure]/1, [temp.alias]/5): the template arguments bound.
  ClosureOwner unevaluated_closure_owner() {
    // The global namespace is `EntityId::none`.
    const EntityId around = current_class != EntityId::none ? current_class : q_->enclosing_namespace();
    Fp128 d = ast::mix(ast::seed("ycxx.sema.unevaluated-closure"), ins().items.fp(item_));
    d = ast::mix(d, u64(loc_base_));
    for (const auto& level : template_args.levels) {
      d = ast::mix(d, u64(level.size()));
      for (const auto& a : level) d = a ? w_->entities.mix_arg(ast::mix(d, u64(1)), *a) : ast::mix(d, u64(0));
    }
    return ClosureOwner{around, ast::Linkage::internal, d};
  }
  // The part being analysed is no function's body: its generic lambdas are instantiated in it
  // (`GenericClosure::part`).
  bool declaration_part = false;
  // An alias template's type-id read again with dependent arguments: its closure types are
  // patterns, which each specialization's replace (#1549).
  bool closure_patterns = false;
  // How many alias template specializations named here made closure types (#1549): an alias
  // template naming one is read again for each of its own specializations ([temp.alias]/5).
  u32 closure_alias_uses = 0;
  struct ClosureOwnerScope {
    Sema& sema;
    std::optional<ClosureOwner> saved;
    ClosureOwnerScope(Sema& s, std::optional<ClosureOwner> owner) : sema(s), saved(std::exchange(s.closure_owner, owner)) {}
    ClosureOwnerScope(const ClosureOwnerScope&) = delete;
    ~ClosureOwnerScope() { sema.closure_owner = saved; }
  };
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
  u32 decltype_unevaluated = 0;  // the depth of the innermost decltype operand, or zero
  bool checking_virtual_body = false;

  // Ends the body of the innermost lambda (`body`, from `body_begin`, the lambda's tokens ending
  // at `end`): its return type deduced, its closure's record published, and the lambda-expression,
  // a prvalue of the closure type, in the enclosing context.
  Ex close_lambda(Ex body, u32 body_begin, u32 end);
  // The static invoker's body ([expr.prim.lambda.closure]/9): the function call operator of a
  // closure object called with its own parameters, `{ return Closure{}(params...); }`; its locals
  // are the operator's parameters, the first `nparams` of the operator's locals.
  NodeId invoker_tree(EntityId cls, EntityId member, QualType call_type, u32 at_);
  bool has_generic_lambdas() const { return !generic_closures_.empty(); }
  // Whether this is a template's definition (or a generic lambda's), not an instantiation.
  // A generic lambda's definition is one in an instantiation too: its own parameters are unbound.
  bool in_generic_definition() const {
    return std::ranges::any_of(lambdas_, [](const LambdaFrame& f) { return !f.head.empty(); });
  }
  bool in_pattern() const {
    return in_generic_definition() || (templated() && !instantiating() && !satisfying_atom);
  }
  // The satisfaction of an atomic constraint is checked ([temp.constr.atomic]): its parameter
  // mapping binds the parameters it names, perhaps none.
  bool satisfying_atom = false;
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
  std::optional<GenericCall> generic_call(const MemberDeclRec& m, std::span<const TemplateArg> args, u32 at_,
                                          bool demand_definition = true);

  // Begins a specialization of a generic lambda's function call operator, `spec` of type
  // `spec_type` for `args`: the closure's frame rebuilt from its record (every capture is a field
  // by name), the head bound, its parameters declared. The enclosing function's constants are the
  // current context's locals.
  void open_generic(const GenericClosure& g, EntityId spec, QualType spec_type, std::span<const TemplateArg> args);
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

  struct ClosureTarget {
    EntityId entity;
    QualType type;
  };

  // A static lambda returns its operator; another captureless lambda returns an invoker
  // ([expr.prim.lambda.closure]). A nonthrowing function can convert to a throwing pointer.
  std::optional<ClosureTarget> closure_invoker(QualType from, QualType to) {
    if (!types().is_class(from) || !types().is_pointer(to)) return std::nullopt;
    const ClassRec* r = class_rec(types().class_of(from));
    if (!r || !r->closure) return std::nullopt;
    const auto invoker = ins().names.intern("__invoke");
    const auto call = ins().names.intern("operator()");
    const auto target = Types::unqualified(to);
    for (const MemberDeclRec& m : r->members)
      if (m.is_static && !m.is_template && (m.name == invoker || m.name == call) &&
          (types().canonical(types().pointer(m.type)) == types().canonical(target) ||
           types().function_pointer_conversion(m.type, types().pointee(target))))
        return ClosureTarget{m.entity, m.type};
    return std::nullopt;
  }

  QualType invoker_type(QualType member) {
    const auto call = types().rec(member);
    return types().function(call.inner, call.params, call.variadic, call.linkage, Cv::none, ast::RefQual::none,
                            call.fn_noexcept);
  }

  struct GenericInvoker {
    EntityId closure;
    GenericCall call;
    QualType type;
    std::vector<ast::TemplateParamSema> head;
    std::vector<TemplateArg> args;
    bool static_call;
  };

  // [temp.deduct.conv], [expr.prim.lambda.closure]: the target function pointer
  // fixes the call-operator template arguments. Its result may require instantiation.
  std::optional<GenericInvoker> generic_invoker(QualType from, QualType to, u32 at_, bool demand_definition);

  ast::SeqId<ast::TemplateParamSema> invoker_head(std::span<const ast::TemplateParamSema> original) {
    std::vector<ast::TemplateParamSema> head(original.begin(), original.end());
    for (auto& parameter : head) {
      parameter.type = rebased(parameter.type, original);
      parameter.depth = 0;
    }
    return ins().intern_seq(std::span<const ast::TemplateParamSema>(head));
  }

  EntityId define_generic_invoker(const GenericInvoker& selected, u32 at_);
  Ex invoker_address(ClosureTarget invoker, u32 begin, u32 end) {
    ExprInfo fi{invoker.type, ValueCategory::lvalue, 0};
    Ex f = node(ast::DeclRef{fi, invoker.entity}, begin, end, fi);
    return convert_node(f, ast::ConversionKind::function_to_pointer, types().pointer(invoker.type));
  }

  EntityId define_closure_conversion(ClosureTarget invoker, u32 at_, std::span<const ast::TemplateParamSema> head);

  // The conversion returns the invoker's address after the closure object has been evaluated.
  Ex invoker_pointer(Ex e, ClosureTarget invoker, std::span<const ast::TemplateParamSema> head = {}) {
    const auto target = w_->entities.rec(invoker.entity);
    if (unevaluated == 0 && !require_supported_virtual_signature(types().rec(invoker.type), e.begin))
      return {NodeId::none, e.begin, e.end};
    if (unevaluated == 0 && compatibility() != ast::Compatibility::spec) {
      const QualType object_type = types().add_cv(types().class_type(target.scope), Cv::const_);
      const Ex receiver = bind_reference(e, types().lvalue_ref(object_type), e.begin);
      if (!receiver.ok()) return receiver;
      const ExprInfo object_pointer{types().pointer(object_type), ValueCategory::prvalue, 0};
      const Ex address = node(ast::Unary{object_pointer, ast::UnaryOp::address_of, at(receiver, e.begin)}, e.begin, e.end,
                              object_pointer);
      const EntityId conversion = define_closure_conversion(invoker, e.begin, head);
      const ExprInfo result{types().pointer(invoker.type), ValueCategory::prvalue, 0};
      return node(ast::MemberCall{result, conversion, at(address, e.begin), b_->list({})}, e.begin, e.end, result);
    }
    Ex p = invoker_address(invoker, e.begin, e.end);
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
  static NodeId closure_member_body(const ast::ClosureDef& closure, EntityId member) {
    if (member == closure.call) return closure.body;
    return member == closure.invoker ? closure.invoker_body : closure.conversion_body;
  }
  static ClosureMember closure_member_def(World& w, const ast::ClosureDef& c, EntityId f, NodeId body,
                                          std::vector<ast::LocalRec> locals) {
    ast::Interners& in = w.ins;
    const bool call = c.call == f;
    const bool conversion = c.conversion == f;
    std::size_t n = w.types.rec(c.call_type).params.size();
    if (!call) locals.resize(std::min(n, locals.size()));
    std::vector<ast::ParamSema> params;
    for (std::size_t k = 0; k < n && k < locals.size(); ++k) params.push_back({EntityId::none, locals[k].type, locals[k].name, NodeId::none, 0});
    ast::DeclSema d{};
    d.entity = f;
    d.type = call ? c.call_type : w.entities.rec(f).type;
    d.kind = ast::DeclKind::function;
    d.linkage = w.entities.rec(f).linkage;
    d.flags = u16(decl_flag::member | decl_flag::definition | decl_flag::inline_ | decl_flag::constexpr_ |
                  (!conversion && (!call || (c.flags & closure_flag::static_call)) ? decl_flag::static_ : 0));
    d.attributes = in.intern_seq(std::span<const NodeId>{});
    d.constraint = NodeId::none;
    d.constraints = ConstId::none;
    d.contracts = NodeId::none;
    d.params = in.intern_seq(std::span<const ast::ParamSema>(params));
    d.template_params = in.intern_seq(std::span<const ast::TemplateParamSema>{});
    ast::BodySema b{body, in.intern_seq(std::span<const ast::LocalRec>(locals)), in.intern_seq(std::span<const ast::ExpansionRec>{}),
                    in.intern_seq(std::span<const u32>{}), QualType{}, 0, in.intern_seq(std::span<const ast::ClosureDef>{})};
    std::optional<QualType> this_type;
    if (conversion || (call && !(c.flags & closure_flag::static_call))) {
      QualType ct = w.types.class_type(c.closure);
      if (conversion || !(c.flags & closure_flag::mutable_)) ct = w.types.add_cv(ct, Cv::const_);
      this_type = w.types.pointer(ct);
    }
    return {d, b, this_type};
  }
  static bool closure_member(const World& w, EntityId f) {
    const EntityRec* r = w.entities.find(f);
    if (!r || r->scope == EntityId::none) return false;
    const EntityRec* c = w.entities.find(r->scope);
    return c && c->kind == EntityKind::class_ && !c->local_class && w.ins.names.spelling(c->name).empty();
  }

  // ------------------------------------------------------------ the function being defined

  void set_return_type(QualType t) {
    return_type_ = t;
    declared_return_ = t;
    return_deduced_ = false;
    return_seen_ = false;
  }
  QualType return_type() const { return return_type_; }
  bool in_function_body = false;  // BodyAnalysis, including any enclosed lambda bodies
  bool variadic_function_body = false;  // BodyAnalysis's function, excluding nested lambda bodies
  // A declared return type holding a placeholder ([dcl.spec.auto.general]/3): each return
  // statement deduces it, and the first one's deduction is the return type from then on (/11).
  bool deduces_return() const { return w_->types.has_placeholder(declared_return_); }
  bool return_deduced() const { return return_deduced_; }
  // Whether a return statement was seen, deducing or failing to.
  bool return_seen() const { return return_seen_; }
  // Inside a discarded statement or a dependent `if constexpr`: returns deduce nothing.
  u32 no_return_deduction = 0;

  bool checks_lambda_result_expression() {
    return !lambdas_.empty() && lambdas_.back().head.empty() &&
           w_->entities.rec(lambdas_.back().call).templ == EntityId::none &&
           types().kind(declared_return_) == TypeKind::placeholder && no_return_deduction == 0;
  }

  // [stmt.return], [expr.prim.id.unqual]: copy-initialize an explicit class result from an
  // eligible local name as an xvalue. Parentheses preserve the category of the named operand.
  Ex init_explicit_return(Ex e, QualType type) {
    if (e.ok() && types().is_class(type) && e.cat() == ValueCategory::lvalue && !dependent(e)) {
      Ex name = e;
      while (auto paren = as<ast::Paren>(name)) name.id = paren->operand.id;
      if (auto ref = as<ast::LocalRef>(name)) {
        const auto& local = locals_[ref->local];
        const bool eligible_type = types().is_reference(local.type)
                                       ? types().rec(local.type).rvalue_ref && types().is_object(e.type())
                                       : types().is_object(local.type);
        if (local.entity == EntityId::none && !(local.flags & ast::local_flag::structured_binding) &&
            eligible_type && cv_within(object_cv(e.type()), Cv::const_))
          e = convert_node(e, ast::ConversionKind::qualification, e.type(), ValueCategory::xvalue);
      }
    }
    return convert(e, type, e.begin);
  }

  Ex init_deduced_return(Ex e, QualType type) {
    if (e.ok() && !lambdas_.empty()) {
      LambdaFrame& frame = lambdas_.back();
      if (std::exchange(frame.pending_result, NodeId::none) == e.id && !e.glvalue() && initializes(type, e)) {
        // [dcl.init.general]: this same-class prvalue initializes the result directly.
        frame.polymorphic_result = true;
        return e;
      }
    }
    return convert(e, type, e.begin);
  }

  // What `return e;` (`e` none for `return;` or the end of the body) deduces: the return type,
  // or none after a diagnosed error. In a template's definition a type-dependent `e` deduces
  // nothing; each instantiation does ([dcl.spec.auto.general]/12).
  std::optional<QualType> deduce_return(const Ex* e, u32 at_);
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
  QualType with_noexcept(QualType fn) {
    TypeRec r = types().rec(fn);
    return types().function(r.inner, r.params, r.variadic, r.linkage, r.fn_cv, r.fn_ref, true);
  }
  QualType with_return(QualType fn, QualType ret) {
    TypeRec r = types().rec(fn);
    return types().function(ret, r.params, r.variadic, r.linkage, r.fn_cv, r.fn_ref, r.fn_noexcept);
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
    // Meaningful only when not `dependent`. Initialized anyway: the scope is copied into
    // `switches`, and copying an indeterminate value is erroneous ([basic.indet]; #726).
    QualType type{};
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
  // A constexpr function-template specialization can become immediate when its body
  // instantiates ([expr.const.imm]). The inference belongs to this body only.
  bool can_escalate_immediate = false;
  bool escalated_immediate = false;
  // Manifestly constant-evaluated initializers and assertions permit these references.
  // The ordinary immediate-invocation evaluator still performs its existing folding.
  bool constant_reference_context = false;
  // A namespace initializer can consume the pointer during trial constant evaluation.
  bool trial_initializer = false;
  EntityId folded_immediate_result = EntityId::none;
  struct ImmediateReferenceScope {
    Sema& sema;
    bool saved_context;
    bool saved_trial;
    EntityId saved_result;
    ImmediateReferenceScope(Sema& sema, bool allowed, bool trial = false)
        : sema(sema), saved_context(std::exchange(sema.constant_reference_context, allowed)),
          saved_trial(std::exchange(sema.trial_initializer, trial)),
          saved_result(std::exchange(sema.folded_immediate_result, EntityId::none)) {}
    ~ImmediateReferenceScope() {
      sema.constant_reference_context = saved_context;
      sema.trial_initializer = saved_trial;
      sema.folded_immediate_result = saved_result;
    }
  };

  u8 immediate_reference_flags(bool immediate) {
    if (!immediate || immediate_context || constant_reference_context || unevaluated ||
        (escalated_immediate && lambdas_.empty())) return 0;
    has_immediate_addresses_ = true;
    return expr_flag::immediate_address;
  }

  bool needs_immediate_reference_check() const { return has_immediate_addresses_; }

  // Constant initialization can consume an immediate function reference and produce an
  // ordinary value. A non-constexpr object still cannot retain an immediate pointer
  // ([expr.const.const]); a reference variable is not itself such an object.
  void permit_constant_initializer(u32 declaration, QualType target, ConstId value) {
    if (!has_immediate_addresses_ || value == ConstId::none) return;
    if ((compatibility() == ast::Compatibility::spec && types().is_reference(target)) || !immediate_result_entity(value))
      constant_immediate_initializers_.insert(declaration);
  }

  bool is_consteval(EntityId f) const { return consteval_.contains(std::to_underlying(f)); }

  bool escalate_immediate() {
    if (!can_escalate_immediate || !lambdas_.empty() || constant_reference_context) return false;
    escalated_immediate = true;
    return true;
  }

  // A call of an immediate function outside an immediate function context is an immediate
  // invocation: it must be a constant expression, and it is replaced by its value ([expr.const]).
  // `call` calls a consteval function or constructor.
  Ex immediate_invocation(Ex call);

  void retain_immediate_functions(const Sema& replay) {
    consteval_.insert(replay.consteval_.begin(), replay.consteval_.end());
    if (folded_immediate_result == EntityId::none) folded_immediate_result = replay.folded_immediate_result;
  }

  std::optional<EntityId> immediate_result_entity(ConstId value) {
    if (ins().values.is<ast::VEntity>(value)) {
      const auto entity = ins().values.get<ast::VEntity>(value).entity;
      if (is_consteval(entity) || q_->is_immediate_function(entity)) return entity;
    } else if (ins().values.is<ast::VLvalue>(value)) {
      return immediate_result_entity(ins().values.get<ast::VLvalue>(value).base);
    } else if (ins().values.is<ast::VAggregate>(value)) {
      for (const auto element : ins().seqs.elements(ins().values.get<ast::VAggregate>(value).elems))
        if (auto entity = immediate_result_entity(element)) return entity;
    }
    return std::nullopt;
  }

  bool check_immediate_initializer(ConstId value, u32 at) {
    if (auto entity = immediate_result_entity(value)) {
      diag(at, Diag::immediate_address, w_->entities.rec(*entity).name);
      return false;
    }
    return true;
  }

  // An immediate function may be named within an immediate invocation ([expr.const]). Wait
  // until the completed tree, where a folded invocation no longer contains the address operand.
  void check_immediate_addresses(NodeId root, u32 offset = 0) {
    if (!has_immediate_addresses_ || root == NodeId::none || escalated_immediate) return;
    auto check = [&](this const auto& self, const ast::Red& node) -> void {
      if (node.is<ast::ConstantProbe>()) return;
      if (node.is<ast::DefaultMemberInit>()) return;  // checked where its source was replayed
      if ((node.is<ast::Declarator>() || node.is<ast::LocalVar>() || node.is<ast::StaticVar>()) &&
          constant_immediate_initializers_.contains(node.offset())) return;
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
    // An earlier enumerator's value in the active list when publication waits for its type.
    ConstId enum_initializer = ConstId::none;
  };
  std::vector<OwnDecl> own;
  // Defaults, including inherited ones, of functions already declared in this item.
  std::map<EntityId, std::vector<NodeId>> own_function_defaults;
  struct MemberIntroduction {
    NameId name;
    ast::DeclSema sema;
    bool is_union;
  };
  std::optional<MemberIntroduction> member_introduction;
  std::optional<EntityId> defining_unnamed_enum_scope;
  std::optional<std::size_t> nested_enum_members_begin;

  // [basic.scope.enum]: earlier enumerators hide names in the enclosing class.
  const OwnDecl* earlier_nested_enumerator(NameId name) const {
    if (!nested_enum_members_begin) return nullptr;
    const auto enumerators = std::span(own).subspan(*nested_enum_members_begin);
    const auto member = std::ranges::find(enumerators, name, &OwnDecl::name);
    return member == enumerators.end() ? nullptr : &*member;
  }

  // ------------------------------------------------------------ member functions ([class.mfct])

  // The enclosing class of a declaration or definition, and the type of `this` when the context
  // supplies a current object ([expr.prim.this]).
  EntityId current_class = EntityId::none;
  std::optional<QualType> this_type;
  // The function whose body is being analysed, none outside one: a friend of a class may name its
  // private members ([class.friend]/2).
  EntityId current_function = EntityId::none;

  // ------------------------------------------------------------ local classes ([class.local])

  // The block a local class whose member is being analysed is in; none elsewhere. A replay of
  // such a member's part (a default member initializer, a default argument) asks its questions.
  mutable std::shared_ptr<const OuterBlock> outer_block;
  const OuterBlock* outer() const {
    if (!outer_block && !outer_asked_) outer_block = q_->local_outer_block();
    outer_asked_ = true;
    return outer_block.get();
  }
  // Whether `c` is a local class: a class whose scope is a function ([class.local]).
  bool is_local_class(EntityId c) const {
    if (c == EntityId::none) return false;
    const EntityRec& r = w_->entities.rec(c);
    return r.kind == EntityKind::class_ && r.local_class;
  }
  // The enclosing block, for a local class defined here.
  // The function a local class defined here is local to: the enclosing function, or the innermost
  // lambda's function call operator ([class.local], docs/design/closures.md decision 3).
  EntityId local_scope() const { return lambdas_.empty() ? current_function : lambdas_.back().call; }
  // The type a block-scope declaration of `name` in the innermost block gives it, if any.
  std::optional<QualType> innermost_block_type(NameId name) const {
    for (const BlockType& t : block_types_)
      if (t.depth + 1 == scopes_.size() && t.name == name) return t.type;
    return std::nullopt;
  }
  OuterBlock outer_block_here();
  // The name `name` of the block enclosing the local class `cls` whose member is analysed, as an
  // expression: a static local names its variable; an automatic one is no odr-use only in an
  // unevaluated operand, or as a constant's value ([basic.def.odr]/5, /10; [class.local]/1).
  std::optional<Ex> outer_block_expr(const OuterBlock& b, NameId name, u32 begin, u32 end, bool callee);
  // The outermost class enclosing class `c` ([class.nest]), `c` itself at namespace or block scope.
  EntityId outermost_class(EntityId c) const {
    while (enclosing_class(c) != EntityId::none) c = enclosing_class(c);
    return c;
  }
  // Unqualified lookup past the classes enclosing a local class's member: the blocks enclosing
  // the local classes, each followed by the classes of its function ([basic.lookup.unqual]).
  std::optional<Ex> outer_block_value(NameId name, u32 begin, u32 end, bool callee) {
    EntityId last = outermost_class(current_class);
    for (const OuterBlock* b = outer(); b && is_local_class(last); b = b->outer.get()) {
      if (auto r = outer_block_expr(*b, name, begin, end, callee)) return r;
      last = EntityId::none;
      for (EntityId k = b->cls; k != EntityId::none; k = enclosing_class(k)) {
        last = k;
        if (class_rec(k)) {
          if (auto r = class_member(k, name, begin, end, begin, std::nullopt, /*unqualified=*/true)) return *r;
        } else if (auto r = enclosing_prefix_value(k, name, begin, end, begin, /*unqualified=*/true, callee)) {
          return *r;  // a class being defined: its members declared so far
        }
        if (q_->dependent_member_using(k, name)) return dependent_node({}, begin, end);
      }
    }
    return std::nullopt;
  }
  std::optional<QualType> outer_block_class_name(NameId name, bool elaborated, bool aliases, bool& hidden) {
    EntityId last = outermost_class(current_class);
    for (const OuterBlock* b = outer(); b && is_local_class(last); b = b->outer.get()) {
      if (auto t = outer_block_type(*b, name, elaborated, aliases, hidden)) return t;
      if (hidden) return std::nullopt;
      last = EntityId::none;
      for (EntityId k = b->cls; k != EntityId::none; k = enclosing_class(k)) {
        last = k;
        if (w_->entities.rec(k).name == name) return types().class_type(k);  // its injected-class-name
        if (const ClassRec* cr = class_rec(k); cr && cr->defined) {
          if (auto r = nested_member(k, name, true)) return r->first.type;
        } else if (auto hit = enclosing_prefix_type(k, name, std::nullopt, elaborated); hit.found) {
          // A class being defined: its members declared so far ([basic.scope.class]).
          if (!hit.type) hidden = true;
          return hit.type;
        }
      }
    }
    return std::nullopt;
  }
  // The same for a type-name: the type, or none after `hidden` when a non-type hides it.
  std::optional<QualType> outer_block_type(const OuterBlock& b, NameId name, bool elaborated, bool aliases,
                                           bool& hidden) const {
    for (const OuterBlock::Name& n : b.names) {
      if (n.name != name) continue;
      if (n.kind == OuterBlock::Name::Kind::type) {
        if (!aliases) {
          hidden = true;
          return std::nullopt;
        }
        return n.type;
      }
      if (n.kind == OuterBlock::Name::Kind::using_) {
        if (!elaborated && std::ranges::any_of(n.decls, [](const ast::DeclSema& d) { return !is_type(d.kind); })) {
          hidden = true;
          return std::nullopt;
        }
        if (n.type_template) {
          hidden = true;
          return std::nullopt;
        }
        if (n.type != QualType{}) {
          if (n.type_alias && !aliases) {
            hidden = true;
            return std::nullopt;
          }
          return n.type;
        }
        if (elaborated) continue;
        hidden = true;
        return std::nullopt;
      }
      if (elaborated) continue;
      hidden = true;
      return std::nullopt;
    }
    return std::nullopt;
  }
  // A static initializer or assertion looks up names in the class prefix and its bases.
  struct ClassPrefixScope {
    Sema& sema;
    EntityId saved;
    std::optional<NameId> saved_name;
    ClassPrefixScope(Sema& s, EntityId owner, std::optional<NameId> name = std::nullopt)
        : sema(s), saved(std::exchange(s.class_prefix_owner_, owner)),
          saved_name(std::exchange(s.static_initializer_name_, name)) {}
    ~ClassPrefixScope() {
      sema.class_prefix_owner_ = saved;
      sema.static_initializer_name_ = saved_name;
    }
  };

  // The bases of `current_class` while its complete record is not published yet.
  std::span<const BaseRec> completing_bases;
  std::span<const BaseRec> class_prefix_bases(EntityId owner) {
    if (owner == class_prefix_owner_ || encloses_current_class(owner)) return q_->defining_base_records(owner);
    return {};
  }
  const ast::DeclSema* own_initializer_static(NameId name) {
    for (const OwnDecl& d : own | std::views::reverse)
      if (d.name == name && d.sema.kind == ast::DeclKind::variable && d.sema.storage == ast::Storage::static_ &&
          w_->entities.rec(d.sema.entity).scope == current_class) return &d.sema;
    return nullptr;
  }
  bool static_initializer_has_own_name(NameId name) {
    return w_->entities.rec(current_class).name == name || name == static_initializer_name_ || own_initializer_static(name);
  }
  struct NamespaceLambda {
    EntityId scope;
    NameId name;
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
  bool accessible(EntityId c, ast::Access a, EntityId naming = EntityId::none);
  // Whether member `m` of class `c`, named `name` at `at` in class `naming`, is accessible here;
  // diagnosed if not.
  bool check_access(EntityId c, EntityId m, NameId name, u32 at, EntityId naming = EntityId::none,
                    bool check_all_bases = false) {
    const ClassRec* cr = class_rec(c);
    if (!cr) return true;
    ast::Access a = cr->access_of(m);
    if (!accessible(c, a, naming)) {
      diag(at, a == ast::Access::protected_ ? Diag::protected_member : Diag::private_member, name, types().class_type(c));
      return false;
    }
    if (!accessible_naming_base(naming, c, check_all_bases)) {
      diag(at, Diag::inaccessible_base, types().class_type(c), types().class_type(naming));
      return false;
    }
    return true;
  }

  // Member access through a class's base graph ([class.access.base]). Class prefix lookup
  // can use the defining bases before the class is complete. A static member can be shared
  // through several subobjects, unlike a base conversion.
  bool accessible_naming_base(EntityId naming, EntityId owner, bool check_all_bases = false) {
    if (naming == owner || naming == EntityId::none ||
        (!check_all_bases && w_->entities.rec(naming).templ == EntityId::none &&
         class_prefix_bases(naming).empty())) return true;
    auto path = [&](this auto&& next, EntityId from) -> bool {
      if (from == owner) return true;
      const ClassRec* rec = class_rec(from);
      const auto bases = rec ? std::span<const BaseRec>(rec->bases)
                             : (from == current_class ? q_->defining_base_records(from) : class_prefix_bases(from));
      return std::ranges::any_of(bases, [&](const BaseRec& base) {
        return accessible(from, base.access) && next(base.cls);
      });
    };
    return path(naming);
  }

  Ex this_expr(u32 begin, u32 end) {
    if (!this_type || (!lambdas_.empty() && lambdas_.back().static_)) {
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
  std::optional<Found> direct_member(EntityId c, const ClassRec& cr, NameId name);
  std::optional<Found> find_member(EntityId c, NameId name, bool unqualified = false) {
    const ClassRec* cr = class_rec(c);
    if (!cr) return std::nullopt;
    if (auto member = direct_member(c, *cr, name)) return member;
    return find_base_member(cr->bases, name, unqualified);
  }
  std::optional<Found> find_base_member(std::span<const BaseRec> bases, NameId name, bool unqualified) {
    std::optional<Found> r;
    for (const BaseRec& b : bases) {
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

  // Ordinary type lookup is blocked by an unscoped enumerator. Qualifier and
  // elaborated-type lookup still use the separate type lookup path.
  bool unscoped_member_enumerator(EntityId c, NameId name, bool unqualified = false) {
    if (auto member = find_member(c, name, unqualified)) return member->enumerator != EntityId::none;
    if (c == current_class)
      if (auto member = q_->earlier_member(name)) return member->kind == ast::DeclKind::enumerator;
    return false;
  }

  // The member functions of `c` named `name`: an overload set ([over.match.funcs]), with the
  // base members its using-declarations introduce ([namespace.udecl]/1).
  std::vector<const MemberDeclRec*> member_functions(EntityId c, NameId name) {
    std::vector<const MemberDeclRec*> r;
    if (const ClassRec* cr = class_rec(c)) {
      for (const MemberDeclRec& m : cr->members)
        if (m.name == name && m.is_function && !m.is_ctor && !m.is_dtor) r.push_back(&m);
      for (const MemberDeclRec& m : cr->introduced)
        if (m.name == name) r.push_back(&m);
    }
    return r;
  }
  // Whether `m` is a member of `c` that one of its using-declarations introduced.
  bool introduced_member(EntityId c, const MemberDeclRec* m) {
    const ClassRec* cr = class_rec(c);
    return cr && std::ranges::any_of(cr->introduced, [&](const MemberDeclRec& r) { return &r == m; });
  }
  const MemberDeclRec* member_decl(EntityId f) {
    EntityId c = w_->entities.rec(f).scope;
    if (const ClassRec* cr = c == EntityId::none ? nullptr : class_rec(c))
      for (const MemberDeclRec& m : cr->members)
        if (m.entity == f) return &m;
    return nullptr;
  }

  EntityId source_location_class() {
    const auto std_scope = scope_member(global_scope(), ins().names.intern("std"), false);
    if (!std_scope || !std_scope->ns) return EntityId::none;
    const auto found = scope_member(*std_scope, ins().names.intern("source_location"), false);
    if (!found || !found->type || !types().is_class(*found->type)) return EntityId::none;
    return types().class_of(*found->type);
  }

  bool ordinary_immediate_member(EntityId function, QualType result) {
    if (function == EntityId::none) return false;
    const auto& entity = w_->entities.rec(function);
    if (entity.kind != EntityKind::function || entity.scope == EntityId::none || entity.templ != EntityId::none)
      return false;
    const auto& owner = w_->entities.rec(entity.scope);
    if (owner.kind != EntityKind::class_ || owner.templ != EntityId::none) return false;
    const auto* member = member_decl(function);
    if (!member || !member->is_consteval || member->is_virtual || member->is_template) return false;
    // The pinned library's static immediate member returns a one-pointer record.
    // Its aggregate constant uses the same interpreted result storage as other record prvalues.
    if (w_->target.triple == ast::x86_64_linux.triple && member->is_static && types().is_class(result) &&
        ins().names.spelling(entity.name) == "current" && types().class_of(result) == entity.scope &&
        source_location_class() == entity.scope) return true;
    return supported_immediate_member_result(result);
  }

  // Only the checked integral, enum and void result representations enter these paths.
  bool supported_immediate_member_result(QualType result) {
    if (types().is_void(result)) return true;
    if (types().is_enum(result)) {
      const auto* enumeration = enum_rec(types().enum_of(result));
      if (!enumeration || !enumeration->underlying) return false;
      result = *enumeration->underlying;
    }
    const auto builtin = types().builtin_of(result);
    return builtin && ast::is_integral(*builtin) && ast::width_of(w_->target, *builtin) <= 64;
  }

  // A member function named without a call yet: `a.f`, `p->f`, `f` or `S::f`, bound to its object
  // (none when there is none to bind, as in a static member function). Only a call may follow
  // ([expr.ref]/6.3.2).
  Ex bound_member(EntityId c, NameId name, std::optional<Ex> object, bool arrow, u32 begin, u32 end, u32 name_at,
                  EntityId naming = EntityId::none, bool qualified = false, bool explicit_receiver = false) {
    return bind_members(c, member_functions(c, name), object, arrow, begin, end, name_at, naming, qualified,
                        explicit_receiver);
  }

  Ex bind_members(EntityId c, std::vector<const MemberDeclRec*> cands, std::optional<Ex> object, bool arrow,
                  u32 begin, u32 end, u32 name_at, EntityId naming = EntityId::none, bool qualified = false,
                  bool explicit_receiver = false) {
    ExprInfo info{builtin(Builtin::bound_member), ValueCategory::prvalue, 0};
    Ex r = node(ast::Member{info, cands.front()->entity, ~u32{0}, arrow ? 1u : 0u, 0, object ? at(*object, begin) : Child{}},
                begin, end, info);
    bound_[std::to_underlying(r.id)] = {object, std::move(cands), c, name_at, naming, qualified, explicit_receiver};
    return r;
  }

  Ex bind_prefix_member_functions(EntityId owner, NameId name, const std::vector<ast::DeclSema>& declarations,
                                  u32 begin, u32 end) {
    std::vector<MemberDeclRec> records;
    std::vector<ast::DeclSema> statics;
    for (const auto& d : declarations | std::views::reverse) {
      if (std::ranges::contains(records, d.entity, &MemberDeclRec::entity)) continue;
      const bool is_static = (d.flags & decl_flag::static_) != 0;
      records.push_back({.name = name, .entity = d.entity, .type = d.type, .is_function = true, .is_static = is_static,
                         .is_deleted = (d.flags & decl_flag::deleted) != 0,
                         .is_template = (d.flags & decl_flag::template_) != 0, .defaults = defaults_of(d),
                         .is_virtual = d.is_virtual, .is_pure = d.is_pure,
                         .constraints = (d.flags & decl_flag::template_) ? ConstId::none : trailing_constraints(d)});
      if (is_static) statics.push_back(d);
    }
    std::vector<const MemberDeclRec*> candidates;
    candidates.reserve(records.size());
    for (const MemberDeclRec& record : records) candidates.push_back(&record);
    Ex set = bind_members(owner, std::move(candidates), std::nullopt, false, begin, end, begin);
    const u32 key = std::to_underlying(set.id);
    // The map's binding owns these records at a stable address. Swapping preserves the candidate pointers.
    bound_.at(key).prefix_candidates.swap(records);
    overloads_[key] = std::move(statics);  // ordinary function addresses match static members only
    return set;
  }

  bool is_bound_member(const Ex& e) { return e.ok() && types().is(e.type(), Builtin::bound_member); }

  // [expr.prim.id.general]: a non-static data member needs no object in an unevaluated operand.
  Ex unevaluated_data_member(EntityId member, QualType type, u8 bit_width, u32 begin, u32 end) {
    ExprInfo info{types().referee(type), ValueCategory::lvalue, 0, bit_width};
    Ex e = node(ast::DeclRef{info, member}, begin, end, info);
    declared_types_[std::to_underlying(e.id)] = type;
    return e;
  }

  // The implicit member access `name` inside a member function of `c` ([class.mfct.non.static]),
  // or a qualified name `C::name`: none if `c` has no such member.
  std::optional<Ex> class_member(EntityId c, NameId name, u32 begin, u32 end, u32 name_at,
                                 std::optional<u32> address_at = {}, bool unqualified = false,
                                 bool check_all_bases = false, std::span<const BaseRec> defining_bases = {}) {
    auto f = defining_bases.empty() ? find_member(c, name, unqualified) : find_base_member(defining_bases, name, unqualified);
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
      if (!check_access(f->owner, f->nested_type, name, name_at, c, check_all_bases)) return fail;
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
    if (!(f->member && f->member->is_function) && !check_access(c, f->field ? f->field->entity : f->member->entity, name, name_at, naming, check_all_bases))
      return fail;
    if (f->field) {
      const FieldRec& field = *f->field;
      if (address_at) {
        if (field.bit_width) {
          diag(name_at, Diag::bit_field_address);
          return fail;
        }
        if (types().is_reference(field.type)) {
          diag(name_at, Diag::member_pointer_type, field.type);
          return fail;
        }
        if (naming != c) {
          diag(name_at, Diag::member_pointer_values);
          return fail;
        }
        if (class_rec(c)->access_of(field.entity) == ast::Access::protected_ && !accessible(c, ast::Access::private_)) {
          diag(name_at, Diag::member_pointer_protected, name);
          return fail;
        }
        QualType t = types().member_pointer(types().class_type(c), field.type);
        ConstId value = ins().intern_value(ast::VMemberPtr{types().canonical(t), f->index});
        return literal(t, value, ValueCategory::prvalue, *address_at, end);
      }
      if (!has_this) {
        if (unevaluated) return unevaluated_data_member(field.entity, field.type, field.bit_width, begin, end);
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
      QualType t = field_access_type(field, types().pointee(self.type()));
      if (field.mutable_ && field.bit_width && Types::is_volatile(t) && unevaluated == 0) {
        diag(name_at, Diag::volatile_bit_field);
        return fail;
      }
      ExprInfo info{t, ValueCategory::lvalue, 0, field.bit_width};
      return node(ast::Member{info, f->field->entity, f->index, 1, ref ? 1u : 0u, at(self, begin)}, begin, end, info);
    }
    const MemberDeclRec& m = *f->member;
    if (address_at && !m.is_static &&
        !std::ranges::any_of(member_functions(c, name), [](const MemberDeclRec* f) { return f->is_static; })) {
      diag(name_at, Diag::member_pointer_values);
      return fail;
    }
    // A static member, data or function, is named like a namespace-scope entity
    // ([expr.prim.id.qual]): `S::f` can have its address taken or be called. A member function
    // template is a set to deduce from, as overloads are.
    // A constrained one is a set of one, whose constraints the use checks ([over.match.viable]/3,
    // [over.over]/5).
    if (!m.is_function || (m.is_static && !m.is_template && m.constraints == ConstId::none && !m.ineligible &&
                           member_functions(c, name).size() == 1)) {
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
    // `C::f` names the function itself: a call does not dispatch ([class.virtual]/16).
    Ex set = bound_member(c, name, object, true, begin, end, name_at, naming, /*qualified=*/!unqualified);
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
      d.constraints = member->ineligible ? unsatisfiable_constraint() : member->constraints;
      statics.push_back(d);
    }
    if (!statics.empty()) overloads_[key] = std::move(statics);
    return set;
  }

  // A static data member of class `c`. Of the current instantiation of a class template, one whose
  // initializer is value-dependent is value-dependent ([temp.dep.constexpr]/2.3); of a
  // specialization, naming it instantiates its initializer ([temp.inst]/3).
  Ex static_member(EntityId c, EntityId v, QualType t, u32 begin, u32 end) {
    if (w_->entities.rec(v).kind == EntityKind::variable_template) {
      diag(begin, Diag::variable_template_without_arguments, w_->entities.rec(v).name);
      return {NodeId::none, begin, end};
    }
    bool dep = types().is_dependent(types().class_type(c)) && q_->value_dependent_member(v);
    if (!dep && !types().is_dependent(types().class_type(c)) && !q_->use_static_member(v, unevaluated != 0))
      return {NodeId::none, begin, end};
    ExprInfo info{t, ValueCategory::lvalue, dep ? expr_flag::dependent : u8(0)};
    return node(ast::DeclRef{info, v}, begin, end, info);
  }
  // An enumerator ([dcl.enum]): a prvalue of its enumeration with its value.
  Ex enumerator(EntityId e, QualType t, u32 begin, u32 end) {
    if (q_->value_dependent_member(e))
      return dependent_node({}, begin, end, Types::unqualified(t), ValueCategory::prvalue, expr_flag::unrepresented_value);
    auto v = q_->constant_value(e);
    if (!v) return {NodeId::none, begin, end};  // its enumeration was diagnosed
    return literal(Types::unqualified(t), *v, ValueCategory::prvalue, begin, end);
  }
  Ex enumerator_in_list(const OwnDecl& d, u32 begin, u32 end) {
    if (d.enum_initializer != ConstId::none)
      return literal(d.sema.type, d.enum_initializer, ValueCategory::prvalue, begin, end);
    // An unnamed enum does not have an entity before the closing brace. In its template's
    // list, an earlier enumerator with no value yet still has its in-list type.
    if (d.sema.entity == EntityId::none)
      return dependent_node({}, begin, end, d.sema.type, ValueCategory::prvalue, expr_flag::unrepresented_value);
    return enumerator(d.sema.entity, d.sema.type, begin, end);
  }
  // The enumerator `name` of enumeration `e`, if it has one.
  std::optional<Ex> find_enumerator(EntityId e, NameId name, u32 begin, u32 end) {
    if (q_->defining_enum(e)) {
      // Until the closing brace the enumerator still has its in-list type ([dcl.enum]).
      for (const OwnDecl& d : own)
        if (d.name == name && d.sema.kind == ast::DeclKind::enumerator && d.sema.entity != EntityId::none &&
            w_->entities.rec(d.sema.entity).scope == e)
          return enumerator_in_list(d, begin, end);
    } else if (!q_->require_enum_definition(e)) {
      return Ex{NodeId::none, begin, end};
    }
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

  // [class.member.lookup]: a qualified value read in the incomplete current class sees
  // declarations that already precede the use, including an unscoped enum's active prefix.
  std::optional<Ex> qualified_class_prefix_value(EntityId cls, NameId name, u32 begin, u32 end);

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

  const ClassRec* virtual_consumer(EntityId cls) {
    const auto* record = class_rec(cls);
    return record && native_virtual_consumer(*record, classes(), types(), [&](EntityId f) { return q_->written_member(f); })
               ? record : nullptr;
  }

  // A call of a member function ([expr.call], [over.match.funcs]): a static one is an ordinary
  // call; a non-static one passes its object, which must be at least as cv-qualified as the
  // function ([class.this]).
  Ex member_call(Ex callee, std::vector<Ex> args, u32 end, u32 paren_at, bool assignment_notation = false);

  // ------------------------------------------------------------ overload resolution ([over.match])

  enum class DefaultArgumentOrigin : u8 { interned, current_item };
  enum class GuideKind : u8 { none, user, constructor, constructor_template, copy };

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
    bool is_explicit = false;
    DefaultArgumentOrigin default_origin = DefaultArgumentOrigin::interned;
    bool is_consteval = false;  // an immediate function ([dcl.constexpr])
    // A non-template function's trailing requires-clause, for [over.match.best.general]/2.6.
    ConstId constraints = ConstId::none;
    // What a candidate of class template argument deduction is generated from
    // ([over.match.class.deduct]), for [over.match.best.general]/2.10-2.12.
    GuideKind guide = GuideKind::none;
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
  // ([dcl.fct.default]/4); declarations in other scopes have their own. The parameter lists
  // of this item's own declarations are drafts. Allocation calls collect those defaults
  // from `own_function_defaults` instead.
  std::vector<NodeId> found_defaults(const ast::DeclSema& d) {
    if (std::ranges::any_of(own, [&](const OwnDecl& w) { return w.sema.entity == d.entity && w.sema.params == d.params; })) return {};
    return defaults_of(d);
  }

  Candidate non_template_call_candidate(const ast::DeclSema& d, bool allocation) {
    Candidate candidate{d.entity, d.type, true, (d.flags & decl_flag::deleted) != 0};
    if (allocation) {
      if (auto draft = own_function_defaults.find(d.entity); draft != own_function_defaults.end()) {
        candidate.defaults = draft->second;
        candidate.default_origin = DefaultArgumentOrigin::current_item;
        return candidate;
      }
    }
    candidate.defaults = found_defaults(d);
    return candidate;
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
        if (function_ref && o.function_ref && sub != o.sub) return sub < o.sub;
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

  Ics ics(const Ex& a, QualType p, bool constructor_conversion = false);

  // Whether the specialization of constructor template `td` for `args` is explicit, its
  // value-dependent explicit-specifier substituted; none when that substitution fails
  // ([temp.deduct.general]).
  std::optional<bool> explicit_specialization(const ast::DeclSema& td, std::span<const TemplateArg> args) {
    std::vector<ast::TemplateParamSema> ps = ins().seqs.elements(td.template_params);
    auto atom = templates().substitute_constraint(td.explicit_condition, Bindings::of(ps.empty() ? 0 : ps[0].depth, args));
    // An invalid mapping is a substitution failure.
    if (!atom || !ins().values.is<ast::VAtom>(*atom)) return std::nullopt;
    return q_->explicit_value(*atom);
  }

  // [over.best.ics]: viability uses the parameter conversions of ordinary constructors. Access,
  // deletion, and the copy-list restriction on an explicit winner are checked after selection.
  std::optional<Ics> constructor_ics(std::span<const Ex> args, QualType t, Construction form) {
    const EntityId entity = types().class_of(t);
    const ClassRec* record = class_rec(entity);
    if (!record || !record->user_ctors || w_->entities.rec(entity).templ != EntityId::none) return std::nullopt;
    for (const MemberDeclRec& member : record->members)
      if (member.is_ctor && member.is_template) return std::nullopt;
    for (const MemberDeclRec& member : record->members) {
      if (!member.is_ctor || (form == Construction::copy && member.is_explicit)) continue;
      const TypeRec fn = types().rec(member.type);
      const Candidate c{member.entity, member.type, true, member.is_deleted, nullptr, member.defaults};
      const std::size_t required = c.required(fn.params.size());
      if (args.size() < required || (args.size() > fn.params.size() && !fn.variadic)) continue;
      if (form == Construction::copy && (fn.params.empty() || required > 1)) continue;
      if (member.ineligible || !trailing_constraints_satisfied(member.constraints)) continue;
      bool viable = true;
      for (std::size_t i = 0; i < std::min(args.size(), fn.params.size()) && viable; ++i)
        viable = ics(args[i], fn.params[i]).viable;
      if (viable) return Ics{true, Ics::user_defined};
    }
    return Ics{};
  }

  // [over.ics.list]: rank a braced argument before initializing the selected parameter.
  // A disengaged result is a list conversion this slice does not classify.
  std::optional<Ics> list_ics(const Braced& list, QualType parameter, bool scalar_list_arguments);

  static Ics implicit_object_rank(const TypeRec& function, Cv object_cv, bool object_rvalue) {
    if (!cv_within(object_cv, function.fn_cv)) return {};
    const bool rvalue_ref = function.fn_ref == ast::RefQual::rvalue;
    if (rvalue_ref && !object_rvalue) return {};
    if (function.fn_ref == ast::RefQual::lvalue && object_rvalue && function.fn_cv != Cv::const_) return {};
    // An unqualified member can bind an rvalue without competing on reference kind.
    return binding({true, 0, 0}, rvalue_ref, object_rvalue && function.fn_ref != ast::RefQual::none,
                   cv_bits(function.fn_cv) - cv_bits(object_cv));
  }

  // No answer when the conversion sequences are indistinguishable, so the caller
  // can apply candidate tie-breaks ([over.match.best.general]).
  static std::optional<bool> better_conversions(std::span<const Ics> left, std::span<const Ics> right) {
    bool some = false;
    for (std::size_t i = 0; i < left.size(); ++i) {
      if (right[i] < left[i]) return false;
      some = some || left[i] < right[i];
    }
    if (some) return true;
    return std::nullopt;
  }

  enum class OverloadFailure { no_viable, diagnosed };

  // The best viable candidate for `args` ([over.match.best]). Only the absence of a viable
  // candidate can be deferred to another initialization rule. `object_cv` is the implicit
  // object's, for non-static member functions.
  std::expected<std::size_t, OverloadFailure> resolve(
      std::span<const Candidate> cands, std::span<const Ex> args, u32 at, NameId name,
      std::optional<Cv> object_cv, std::optional<QualType> ctor_of = std::nullopt,
      std::span<const Braced* const> lists = {}, bool object_rvalue = false, bool defer_no_viable = false,
      bool scalar_list_arguments = false) {
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
        const Ics object = implicit_object_rank(fr, *object_cv, object_rvalue);
        if (!object.viable) continue;
        seq.push_back(object);
      } else {
        seq.push_back({true, 0, 0});
      }
      bool ok = true;
      for (std::size_t k = 0; k < args.size() && ok; ++k) {
        Ics c{true};
        if (k >= fixed) {
          c.rank = Ics::ellipsis;
        } else if ((!first_list || !lists[k]) && (!template_list || !types().is_dependent(fr.params[k]))) {
          c = ics(args[k], fr.params[k], !ctor_of);
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
          auto c = list_ics(*lists[k], fr.params[k], scalar_list_arguments);
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
      return std::unexpected(OverloadFailure::diagnosed);
    }
    auto report = [&](Diag fn, Diag ctor) {
      if (ctor_of) diag(at, ctor, Types::unqualified(*ctor_of));
      else diag(at, fn, name);
    };
    if (viable.empty()) {
      if (defer_no_viable) return std::unexpected(OverloadFailure::no_viable);
      report(Diag::no_viable_function, Diag::no_viable_constructor);
      return std::unexpected(OverloadFailure::diagnosed);
    }
    // F1 is better than F2 if none of its conversions is worse, and one is better, or else F1 is
    // not a template specialization and F2 is, or both are and F1's template is more specialized
    // ([over.match.best]/2).
    auto better = [&](std::size_t a, std::size_t b) {
      const std::vector<Ics>& x = viable[a].second;
      const std::vector<Ics>& y = viable[b].second;
      if (auto rank = better_conversions(x, y)) return *rank;
      const Candidate& f1 = cands[viable[a].first];
      const Candidate& f2 = cands[viable[b].first];
      if (!f1.pattern && f2.pattern) return true;
      // [over.match.best.general]/2.6: of non-template functions with the same parameter-type-lists,
      // the more constrained ([temp.constr.order]).
      if (!f1.pattern && !f2.pattern && (f1.constraints != ConstId::none || f2.constraints != ConstId::none) &&
          same_parameter_types(f1.type, f2.type)) {
        Templates t = templates();
        return t.subsumes(f1.constraints, f2.constraints) && !t.subsumes(f2.constraints, f1.constraints);
      }
      if (f1.pattern && f2.pattern) {
        Templates t = templates();
        auto [a12, a21] = t.ordering(*f1.pattern, *f2.pattern, args.size());
        if (a12 != a21) return a12;
        // [temp.func.order]/6: equally specialized templates with equivalent heads and parameters:
        // the more constrained ([temp.constr.order]).
        if (a12 && equivalent_for_constraints(*f1.pattern, *f2.pattern)) {
          ConstId c1 = f1.pattern->constraints, c2 = f2.pattern->constraints;
          const bool more = t.subsumes(c1, c2) && !t.subsumes(c2, c1);
          if (more || f1.guide == GuideKind::none || (t.subsumes(c2, c1) && !t.subsumes(c1, c2))) return more;
        }
        if (f1.guide == GuideKind::none) return false;
      } else if (f1.pattern || f1.guide == GuideKind::none) {
        return false;
      }
      // [over.match.best.general]/2.10-2.12: of the candidates of class template argument
      // deduction, one generated from a deduction-guide, then the copy deduction candidate, then one
      // generated from a non-template constructor rather than a constructor template.
      if ((f1.guide == GuideKind::user) != (f2.guide == GuideKind::user)) return f1.guide == GuideKind::user;
      if ((f1.guide == GuideKind::copy) != (f2.guide == GuideKind::copy)) return f1.guide == GuideKind::copy;
      return f1.guide == GuideKind::constructor && f2.guide == GuideKind::constructor_template;
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
      return std::unexpected(OverloadFailure::diagnosed);
    }
    std::size_t c = viable[*best].first;
    if (cands[c].deleted) {
      report(Diag::deleted_function, Diag::deleted_constructor);
      return std::unexpected(OverloadFailure::diagnosed);
    }
    return c;
  }

  // A call returning a reference to a function is an lvalue. For object references the category
  // follows the reference kind ([expr.call]).
  ExprInfo result_info(QualType r) {
    if (!types().is_reference(r)) {
      // [expr.type] removes top-level cv only from non-class, non-array prvalues.
      QualType t = types().is_class(r) || types().is_array(r) ? r : Types::unqualified(r);
      return {t, ValueCategory::prvalue, 0};
    }
    QualType t = types().referee(r);
    return {t, types().rec(r).rvalue_ref && !types().is_function(t) ? ValueCategory::xvalue : ValueCategory::lvalue, 0};
  }

  // ------------------------------------------------------------ class template argument deduction

  // The guide constructor template `ctor` of a class template with head `outer` implies
  // ([over.match.class.deduct]/1.1): the class template's parameters, then the constructor's own,
  // renumbered to follow them at the class's depth, returning `self`, the current instantiation.
  // None for a form not supported yet: a template template parameter of the constructor.
  std::optional<ast::DeclSema> constructor_template_guide(const ast::DeclSema& ctor,
                                                          std::span<const ast::TemplateParamSema> outer, QualType self);

  // The type class template argument deduction gives class template `templ` for initializing an
  // object from `args`, or the elements of `list`, by `form` ([over.match.class.deduct]): the
  // return type of the guide that overload resolution selects among the deduction-guides declared
  // before here, those its constructors imply, and the copy deduction candidate.
  // `Builtin::dependent` while an argument is type-dependent; none after a diagnosed failure at `at`.
  std::optional<QualType> deduce_class_type(EntityId templ, std::vector<Ex> args, const Braced* list,
                                            Construction form, u32 at);

  // ------------------------------------------------------------ constructors and references

  Ex construct_from_list(QualType t, const Braced& b, Construction form,
                         ConstructorDestination destination = ConstructorDestination::object) {
    std::vector<Ex> args;
    for (const Braced::Clause& c : b.clauses) {
      if (c.list || c.designated) {
        diag(c.begin, Diag::not_supported, lex::Tk::l_brace);  // nested lists as constructor arguments
        return {NodeId::none, b.begin, b.end};
      }
      args.push_back(c.expr);
    }
    return construct(t, std::move(args), b.begin, b.end, form, destination);
  }

  // Initializes a `t` by a constructor from `args` ([over.match.ctor], [dcl.init.general]/17.6):
  // the class's constructors and, where they are implicit, its default and copy constructors.
  // List-initialization prohibits narrowing and checks the explicit winner for copy-list forms.
  // Delegation and base initialization target subobjects or objects already under construction.
  // A lambda result can be checked before the result ABI is available.
  Ex construct(QualType t, std::vector<Ex> args, u32 begin, u32 end, Construction form = Construction::direct,
               ConstructorDestination destination = ConstructorDestination::object);
  // In a syntax-only constructor definition, check the implicit default initialization of
  // its direct polymorphic base. There is no complete object or native initializer to build.
  void check_polymorphic_base_default_init(QualType t, u32 at) {
    construct(t, {}, at, at, Construction::direct, ConstructorDestination::polymorphic_base);
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

  // [class.dtor]: a potentially invoked destructor must be accessible.
  bool check_destructor_access(QualType type, u32 at, Diag unusable = Diag::object_unusable_destructor,
                               bool base_subobject = false);

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
    if (!unevaluated && !check_destructor_access(t, begin)) return {NodeId::none, begin, end};
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

  // The parser proved that this functional cast is the whole operand of a deducing lambda return.
  Ex lambda_result_cast(QualType t, std::vector<Ex> args, u32 begin, u32 end) {
    const ClassRec* c = types().is_class(t) ? class_rec(types().class_of(t)) : nullptr;
    if (!c || !c->polymorphic) return functional_cast(t, std::move(args), begin, end);
    Ex result = construct(t, std::move(args), begin, end, Construction::direct, ConstructorDestination::lambda_result);
    if (result.ok() && !dependent(result)) lambdas_.back().pending_result = result.id;
    return result;
  }

  // The new runtime default path admits only these scalar operations and ordinary calls.
  // Allocation, opaque virtual dispatch and class-valued calls require separate proofs.
  Ex runtime_default_supported(Ex operand);

  // [expr.unary.noexcept], [except.spec]: inspect the selected operations and their operands
  // without interpreting the expression or requiring the bodies of its callees.
  // Without `result_destroyed`, the destruction of a prvalue's result object is not included: a
  // constructor's exception specification ([except.spec]) does not depend on its subobjects' destructors.
  Ex unary_noexcept(Ex operand, u32 begin, u32 end, bool result_destroyed = true);

  // A void prvalue ([expr.type.conv]/2): `(void)0`.
  Ex void_prvalue(u32 begin, u32 end) {
    QualType i = builtin(Builtin::int_);
    Ex zero = literal(i, w_->values.integer(i, 0), ValueCategory::prvalue, begin, end);
    return convert_node(zero, ast::ConversionKind::to_void, builtin(Builtin::void_));
  }

  // Temporary materialization ([conv.rval]) of the prvalue `v` as an object of type `t`.
  Ex materialize(Ex v, QualType t, bool extended, bool argument = false) {
    if (!v.ok()) return v;
    if (!require_concrete_call_result(t, v.begin) || !require_supported_virtual_layout(t, v.begin) ||
        !check_destructor_access(t, v.begin))
      return {NodeId::none, v.begin, v.end};
    ExprInfo info{t, ValueCategory::xvalue, 0};
    return node(ast::Materialize{info, destructor_of(t), extended ? 1u : argument ? 2u : 0u, at(v, v.begin)}, v.begin, v.end, info);
  }

  // Binds a reference of type `r` to `e` ([dcl.init.ref]): directly to an lvalue (or, for an
  // rvalue reference, an xvalue) of its type, no less qualified; a const lvalue reference or an
  // rvalue reference also to a temporary initialized from `e`, which lives as long as the
  // reference when `extend` ([class.temporary]/6).
  Ex bind_reference(Ex e, QualType r, u32 at_, bool extend = false);

  // A temporary a reference is bound to through its subobject (`const int& m = A().x`) lives as
  // long as the reference too ([class.temporary]/6): the `Materialize` under `.` and parentheses,
  // rebuilt extended.
  Ex extended(Ex e);

  // ------------------------------------------------------------ names ([basic.lookup.unqual])

  // An id-expression naming `name`, at `begin`: the innermost block-scope declaration, else the
  // namespace-scope ones. The `callee` of a call that lookup does not find is a `DepName`: the
  // call decides once it has its arguments ([temp.dep.candidate]).
  Ex name_expr(NameId name, u32 begin, u32 end, bool callee = false, bool immediate_default_call = false);

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
  // its arguments. Any other `__builtin_` name is an ordinary one.
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
  bool builtin_bit_cast(NameId name) { return ins().names.spelling(name) == lex::builtin_bit_cast_name; }
  bool builtin_offsetof(NameId name) {
    return ins().names.spelling(name) == lex::builtin_offsetof_name && !declared(name);
  }
  bool builtin_arity(std::size_t want, std::size_t actual, u32 begin) {
    if (actual == want) return true;
    diag(begin, actual < want ? Diag::too_few_arguments : Diag::too_many_arguments, u64(want), u64(actual));
    return false;
  }

  Ex builtin_addressof(Ex e, u32 begin, u32 end) {
    if (!e.ok()) return {NodeId::none, begin, end};
    if (dependent(e)) return dependent_node({e}, begin, end);
    if (e.cat() != ValueCategory::lvalue) {
      diag(begin, Diag::not_lvalue_address, e.type());
      return {NodeId::none, begin, end};
    }
    return builtin_address_of(e, begin, end, begin);
  }

  Ex builtin_value_call(ast::BuiltinOp op, QualType result, QualType param, Ex arg, u32 begin, u32 end) {
    if (dependent(arg)) return dependent_node({arg}, begin, end, result);
    arg = convert(arg, param, arg.begin);
    if (!arg.ok()) return {NodeId::none, begin, end};
    ExprInfo info{result, ValueCategory::prvalue, 0};
    return node(ast::BuiltinCall{info, op, b_->list({at(arg, begin)})}, begin, end, info);
  }

  // A builtin that is a C library function: a call of its implicit declaration, whose arguments
  // are copy-initialized parameters as in any call ([expr.call]/7). The call carries the
  // function's `constant`, so that constant evaluation reads the bytes itself (`ir::BuiltinFn`).
  Ex builtin_library_call(const StringFunction& f, std::vector<Ex> args, u32 begin, u32 end) {
    const ast::DeclSema d = w_->string_function(f, compatibility());
    const auto& signature = types().rec(d.type);
    if (!builtin_arity(signature.params.size(), args.size(), begin)) return {NodeId::none, begin, end};
    if (std::ranges::any_of(args, [](const Ex& a) { return !a.ok(); })) return {NodeId::none, begin, end};
    const QualType result = signature.inner;
    if (std::ranges::any_of(args, [&](const Ex& a) { return dependent(a); })) return dependent_node(args, begin, end, result);
    std::vector<Child> converted;
    for (std::size_t i = 0; i < args.size(); ++i) {
      const Ex arg = convert(args[i], signature.params[i], args[i].begin);
      if (!arg.ok()) return {NodeId::none, begin, end};
      converted.push_back(at(arg, begin));
    }
    q_->use_library_function(d.entity);
    const ExprInfo fn_info{d.type, ValueCategory::lvalue, 0};
    const Ex callee = node(ast::DeclRef{fn_info, d.entity}, begin, begin, fn_info);
    const ExprInfo info{result, ValueCategory::prvalue, 0};
    return node(ast::Call{info, d.entity, at(callee, begin), b_->list(converted), f.constant}, begin, end, info);
  }

  // `__builtin_operator_new(args)` and `__builtin_operator_delete(args)` (docs/EXTENSIONS.md): the
  // call of the global `operator new` or `operator delete` that overload resolution selects for
  // `args`, as `::operator new(args)` would be ([over.match.call]). Only the replaceable functions
  // qualify ([new.delete.single]), not placement or other written overloads. A dependent call is
  // checked when its instantiation is reparsed.
  Ex builtin_allocation_call(const lex::BuiltinFunctionInfo& b, std::vector<Ex> args, u32 begin, u32 end, u32 paren_at) {
    const bool allocating = b.function == lex::BuiltinFunction::operator_new;
    const auto name = allocation_name(allocating ? AllocationFunction::allocate : AllocationFunction::deallocate);
    Ex r = call(allocation_name_expr(name, begin, begin), std::move(args), end, paren_at);
    if (!r.ok() || dependent(r)) return r;
    if (const auto selected = as<ast::Call>(r); selected && selected->function != EntityId::none) {
      const auto& f = w_->entities.rec(selected->function);
      if (f.scope == EntityId::none && w_->replaceable_allocation_function(name, f.type)) return r;
    }
    diag(begin, Diag::builtin_allocation_not_replaceable, ins().names.intern(b.name));
    return {NodeId::none, begin, end};
  }

  ast::LibraryBuiltin written_library_builtin(EntityId function, const TypeRec& signature) {
    using enum ast::LibraryBuiltin;
    if (compatibility() != ast::Compatibility::gcc || function == EntityId::none || signature.variadic) return none;
    const auto& entity = w_->entities.rec(function);
    if (entity.kind != EntityKind::function || entity.lang != LangLinkage::c || entity.scope != EntityId::none ||
        entity.linkage != ast::Linkage::external) return none;
    const NameId name = entity.name;
    const auto* library = string_function_named(ins().names.spelling(name));
    if (!library || library->constant == none) return none;
    const auto expected = types().rec(w_->string_function(*library, compatibility()).type);
    if (expected.params != signature.params ||
        (expected.inner != signature.inner && (library->constant != memchr ||
         signature.inner != types().pointer(types().add_cv(builtin(Builtin::void_), Cv::const_)))))
      return none;
    // A visible user definition is an ordinary function, even with a recognized declaration.
    for (const auto& declaration : q_->namespace_members(EntityId::none, name))
      if (declaration.entity == function && (declaration.flags & decl_flag::definition)) return none;
    return library->constant;
  }

  Ex builtin_overflow_call(ast::BuiltinOp op, std::span<const Ex> args, u32 begin, u32 end);

  // Only send closed, read-only expressions to the fresh constant evaluator. In particular
  // a probe cannot instantiate a speculative callee or run a write in a constexpr frame.
  bool constant_probe_safe(const ast::Red& expression, bool runtime_invariant);

  std::optional<ConstId> constant_probe_value(Ex operand, bool runtime_invariant = false);

  Ex builtin_constant_probe(Ex operand, u32 begin, u32 end) {
    const QualType result = builtin(Builtin::int_);
    if (!operand.ok()) return {NodeId::none, begin, end};
    if (dependent(operand)) return dependent_node({operand}, begin, end, result);
    if (address_set(operand) || is_bound_member(operand)) return rvalue(operand);
    if (!operand.glvalue() && !check_destructor_access(operand.type(), operand.begin))
      return {NodeId::none, begin, end};

    // GNU constantness is narrower than the set of C++ constant values. Ordinary
    // object and function addresses are not one of the folded result categories.
    bool proved = false;
    if (const auto value = constant_probe_value(operand)) {
      const auto& values = ins().values;
      proved = values.is<ast::VInt>(*value) || values.is<ast::VFloat>(*value) || values.is<ast::VNullptr>(*value) ||
          values.is<ast::VString>(*value);
      if (values.is<ast::VLvalue>(*value)) proved = values.is<ast::VString>(values.get<ast::VLvalue>(*value).base);
    }
    const ExprInfo info{result, ValueCategory::prvalue, 0};
    return node(ast::ConstantProbe{info, at(operand, begin), proved}, begin, end, info);
  }

  QualType source_string_pointer() {
    return types().pointer(types().add_cv(builtin(Builtin::char_), Cv::const_));
  }

  // Resolve the library's actual nested type. This compiler lookup does not name it in source.
  EntityId source_location_descriptor_type() {
    const EntityId owner = source_location_class();
    if (owner == EntityId::none) return EntityId::none;
    const auto nested = member_type(owner, ins().names.intern("__impl"));
    if (!nested || !types().is_class(*nested)) return EntityId::none;
    const EntityId descriptor = types().class_of(*nested);
    const auto& entity = w_->entities.rec(descriptor);
    if (entity.scope != owner || ins().names.spelling(entity.name) != "__impl") return EntityId::none;
    const ClassRec* record = class_rec(descriptor);
    if (!record || !is_aggregate(*nested) || record->union_ || !record->bases.empty() || record->fields.size() != 4 ||
        record->unapplied_layout || q_->class_layout_uncertain(descriptor)) return EntityId::none;
    constexpr std::array<std::string_view, 4> names{"_M_file_name", "_M_function_name", "_M_line", "_M_column"};
    const QualType text = source_string_pointer(), number = builtin(Builtin::unsigned_int);
    for (std::size_t i = 0; i < names.size(); ++i) {
      const FieldRec& field = record->fields[i];
      if (ins().names.spelling(field.name) != names[i] || types().canonical(field.type) != (i < 2 ? text : number) ||
          field.mutable_ || field.bit_width || field.no_unique_address) return EntityId::none;
    }
    constexpr std::array<u64, 4> offsets{0, 8, 16, 20};
    const auto layout = class_layout(descriptor);
    return layout && layout->size == 24 && layout->align == 8 && std::ranges::equal(layout->offsets, offsets)
        ? descriptor : EntityId::none;
  }

  std::string source_function_name() {
    if (!in_function_or_lambda_body()) return {};
    const EntityId function = lambdas_.empty() ? current_function : lambdas_.back().call;
    if (function == EntityId::none) return {};
    const auto& record = w_->entities.rec(function);
    std::string name(ins().names.spelling(record.name));
    for (EntityId owner = record.scope; owner != EntityId::none; owner = w_->entities.rec(owner).scope) {
      const auto part = ins().names.spelling(w_->entities.rec(owner).name);
      if (!part.empty()) name = std::string(part) + "::" + name;
    }
    return types().spell_declarator(record.type, std::move(name));
  }

  ConstId source_location_address(EntityId descriptor, u32 at) {
    const auto source = q_->presumed_source(source_position(at));
    const QualType chars = source_string_pointer();
    auto string_address = [&](std::string_view value) {
      std::vector<u32> units;
      units.reserve(value.size() + 1);
      for (unsigned char byte : value) units.push_back(byte);
      units.push_back(0);
      const auto array = types().array(types().pointee(chars), units.size());
      return w_->values.address(chars, w_->values.string(array, units), 0);
    };
    const QualType number = builtin(Builtin::unsigned_int);
    const ConstId fields[]{string_address(source.file), string_address(source_function_name()),
                           w_->values.integer(number, source.line), w_->values.integer(number, source.column)};
    const QualType type = types().add_cv(types().class_type(descriptor), Cv::const_);
    const auto value = w_->values.aggregate(type, fields);
    const auto entity = w_->entities.entity({.kind = EntityKind::variable, .name = no_name(), .type = type,
                                             .linkage = ast::Linkage::internal, .constant_object = value});
    return w_->values.address(types().pointer(type), w_->values.entity(entity), 0);
  }

  Ex builtin_source_location(u32 begin, u32 end, u32 site) {
    const Ex fail{NodeId::none, begin, end};
    if (!w_->target.supports(ast::NativeOperation::source_location_descriptor)) {
      diag(begin, Diag::source_location_target);
      return fail;
    }
    const EntityId descriptor = source_location_descriptor_type();
    if (descriptor == EntityId::none) {
      diag(begin, Diag::source_location_descriptor);
      return fail;
    }
    const QualType type = types().pointer(types().add_cv(types().class_type(descriptor), Cv::const_));
    const ExprInfo info{type, ValueCategory::prvalue, 0};
    if (unevaluated) return node(ast::SourceLocation{info}, begin, end, info);
    if (default_source_observer_) {
      default_source_observer_->descriptor_type = descriptor;
      return node(ast::SourceLocation{info}, begin, end, info);
    }
    if (q_->in_default_member_initializer()) {
      diag(begin, Diag::source_location_default_member);
      return fail;
    }
    return literal(type, source_location_address(descriptor, site), ValueCategory::prvalue, begin, end);
  }

  // GNU requires a constant size and uses a null alignment argument for typical alignment.
  Ex builtin_atomic_lock_free(std::span<const Ex> args, u32 begin, u32 end) {
    const auto fail = [&] {
      diag(begin, Diag::builtin_atomic_lock_free);
      return Ex{NodeId::none, begin, end};
    };
    const Ex width = convert(args[0], builtin(w_->target.size_type), args[0].begin);
    const QualType cv_void = types().add_cv(builtin(Builtin::void_), Cv(3));
    const Ex address = convert(args[1], types().pointer(cv_void), args[1].begin);
    if (!width.ok() || !address.ok()) return {NodeId::none, begin, end};
    const auto value = constant_probe_value(width, /*runtime_invariant=*/true);
    const auto alignment = constant_probe_value(address, /*runtime_invariant=*/true);
    if (!value || !ins().values.is<ast::VInt>(*value) ||
        !alignment || !ins().values.is<ast::VNullptr>(*alignment)) return fail();
    const auto size = ast::int_bits(ins(), *value);
    if (size.hi || (size.lo != 3 && size.lo != 4)) return fail();
    // A three-byte object has no native atomic width on the target. Other widths
    // still need native support or a measured target guarantee.
    return bool_literal(size.lo == 4, begin, end);
  }

  // A nonconstant order is carried as an operand for its effects and uses seq_cst at runtime.
  bool builtin_atomic_order(Ex& argument, std::optional<ir::MemOrder>& order) {
    argument = convert(argument, builtin(Builtin::int_), argument.begin);
    if (!argument.ok()) return false;
    // Evaluating a call in the constexpr interpreter could select its if-consteval
    // arm even though the order argument executes at runtime.
    const auto value = constant_probe_value(argument, /*runtime_invariant=*/true);
    if (!value) return true;
    if (ins().values.is<ast::VInt>(*value)) {
      const auto bits = ast::int_bits(ins(), *value);
      if (bits.hi == 0 && bits.lo <= std::to_underlying(ir::MemOrder::seq_cst)) {
        order = ir::MemOrder(bits.lo);
        return true;
      }
    }
    diag(argument.begin, Diag::builtin_atomic_order);
    return false;
  }

  Ex builtin_atomic_call(lex::AtomicBuiltin kind, std::vector<Ex>& args, u32 begin, u32 end);

  // Builtins with ordinary expression arguments (docs/EXTENSIONS.md).
  Ex builtin_call(const lex::BuiltinFunctionInfo& b, std::vector<Ex> args, u32 begin, u32 end, u32 paren_at);

  std::optional<lex::VaBuiltin> va_builtin(NameId name) {
    auto b = lex::va_builtin(ins().names.spelling(name));
    return b && !declared(name) ? b : std::nullopt;
  }

  Ex va_call(lex::VaBuiltin builtin_op, Ex cursor, std::optional<Ex> source,
             std::optional<QualType> fetch, u32 begin, u32 end);

  // The expression naming what lookup found for `name` ([expr.prim.id]): one entity, or an
  // overload set.
  // `unqualified`: an unqualified-id looked up at namespace scope or introduced by a block
  // using-declaration. Calls may add ADL when every result is a namespace function.
  Ex decls_expr(std::vector<ast::DeclSema> found, bool any, NameId name, u32 begin, u32 end, bool callee = false,
                bool unqualified = false);

  // ------------------------------------------------------------ literals ([lex.literal])

  Ex literal(QualType t, ConstId v, ValueCategory cat, u32 begin, u32 end) {
    ExprInfo info{t, cat, ins().values.is_dependent(v) ? expr_flag::dependent : u8{0}};
    return node(ast::Literal{info, v}, begin, end, info);
  }

  // [lex.icon]: the first type in the literal's list that can represent its value.
  Ex integer_literal(const lex::Literal& lit, u32 begin, u32 end);

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
  Ex integer_user_literal(NameId name, const lex::Literal& lit, u32 begin, u32 end);

  // [lex.fcon]: choose the target's representation of `double`, `float` or `long double`.
  // The extended types' suffixes (`f16`, `bf16`, …) have no values yet (#336).
  Ex float_literal(const lex::Literal& lit, u32 begin, u32 end) {
    using enum lex::FloatSuffix;
    lex::FloatSuffix suffix = lit.float_suffix;
    Builtin k = suffix == f ? Builtin::float_ : suffix == l ? Builtin::long_double : Builtin::double_;
    ast::FloatFormat want = suffix == f ? ast::FloatFormat::binary32 : ast::FloatFormat::binary64;
    const bool x87 = suffix == l && w_->target[k].format == ast::FloatFormat::x87_extended;
    if ((suffix != none && suffix != f && suffix != l) || (!x87 && w_->target[k].format != want)) {
      diag(begin, Diag::extended_float_not_supported);
      return {NodeId::none, begin, end};
    }
    if (suffix == l && (x87 ? lit.x87_overflow : lit.overflow)) diag(begin, Diag::float_literal_out_of_range);
    QualType t = builtin(k);
    const ast::Bits128 bits = x87 ? ast::Bits128{lit.x87[0], lit.x87[1]} : ast::Bits128{lit.value, 0};
    if (lit.nonzero_float_spelling && bits.lo == 0 && bits.hi == 0) {
      if (compatibility() == ast::Compatibility::gcc) diag(begin, Diag::float_literal_underflow_gcc);
      if (compatibility() == ast::Compatibility::clang) diag(begin, Diag::float_literal_underflow_clang, t);
    }
    return literal(t, ast::float_value(ins(), w_->target, t, bits), ValueCategory::prvalue, begin, end);
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
    return string_lvalue(literal_character_type(l.encoding), l.units, begin, end);
  }

  Ex string_lvalue(Builtin elem, std::vector<u32> units, u32 begin, u32 end) {
    units.push_back(0);
    const QualType t = types().array(types().add_cv(builtin(elem), Cv::const_), units.size());
    return literal(t, w_->values.string(t, units), ValueCategory::lvalue, begin, end);
  }

  Ex pretty_function(u32 begin, u32 end);

  // Canonical arguments use their entire structure. Aliases are stripped by append_pretty_type,
  // and the encoding's delimiters distinguish each level of qualification and indirection.
  bool append_pretty_args(std::string& text, std::span<const TemplateArg> args);

  bool append_pretty_name(std::string& text, EntityId id, bool function_scope = false) {
    const EntityRec& entity = w_->entities.rec(id);
    const std::string_view name = ins().names.spelling(entity.name);
    if (name.empty() || entity.unnamed_type_ordinal || entity.enum_linkage_name) return false;
    // An unnamed namespace is unambiguous for the function body within its own unit. Reject
    // such type arguments, whose names alone cannot distinguish them across units.
    if (name == "(anonymous)" && !function_scope) return false;
    if (entity.scope != EntityId::none) {
      const auto kind = w_->entities.rec(entity.scope).kind;
      if ((kind != EntityKind::namespace_ && kind != EntityKind::class_) ||
          !append_pretty_name(text, entity.scope, function_scope)) return false;
    }
    text += "::";
    text += name;
    return entity.templ == EntityId::none || append_pretty_args(text, entity.args);
  }

  static std::string_view pretty_cv(Cv cv) {
    switch (cv) {
      case Cv::none: return "";
      case Cv::const_: return "const";
      case Cv::volatile_: return "volatile";
      case Cv::const_volatile: return "const volatile";
    }
    std::unreachable();
  }

  bool append_pretty_parameters(std::string& text, const TypeRec& type) {
    text += '(';
    const char* separator = "";
    for (QualType p : type.params) {
      text += separator;
      separator = ", ";
      if (!append_pretty_type(text, p)) return false;
    }
    if (type.variadic) {
      text += separator;
      text += "...";
    }
    text += ')';
    if (type.fn_cv != Cv::none) {
      text += ' ';
      text += pretty_cv(type.fn_cv);
    }
    if (type.fn_ref != ast::RefQual::none) text += type.fn_ref == ast::RefQual::lvalue ? " &" : " &&";
    if (type.fn_noexcept) text += " noexcept";
    if (type.linkage == LangLinkage::c) text += " extern \"C\"";
    return true;
  }

  bool append_pretty_type(std::string& text, QualType t);

  Ex bool_literal(bool v, u32 begin, u32 end) {
    QualType t = builtin(Builtin::bool_);
    return literal(t, w_->values.integer(t, v), ValueCategory::prvalue, begin, end);
  }

  Ex nullptr_literal(u32 begin, u32 end) {
    QualType t = builtin(Builtin::nullptr_t);
    return literal(t, w_->values.null_pointer(t), ValueCategory::prvalue, begin, end);
  }

  Ex null_member_pointer(QualType t, u32 begin, u32 end) {
    t = object_unqualified(t);
    return literal(t, ins().intern_value(ast::VMemberPtr{t}), ValueCategory::prvalue, begin, end);
  }

  std::optional<TemplateArg> qualified_member_constant(QualType qualifier, NameId name, u32 at) {
    const auto before = diag_count();
    Ex value = rvalue(scope_expr(Scope{std::nullopt, qualifier}, name, at, at, at, std::nullopt, true));
    if (has_error_since(before)) {
      drop_failures(before);
      return std::nullopt;
    }
    if (auto literal = as<ast::Literal>(value))
      return TemplateArg{TemplateArgKind::value, types().canonical(Types::unqualified(value.type())), literal->value};
    return std::nullopt;
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
    return discard_gcc_reinterpretation(e);
  }

  // GCC drops a discarded pointer/integer reinterpretation while evaluating its operand.
  Ex discard_gcc_reinterpretation(Ex e) {
    if (compatibility() != ast::Compatibility::gcc || !e.ok()) return e;
    Ex operand = e;
    while (auto p = as<ast::Paren>(operand)) {
      operand.begin += p->operand.at;
      operand.id = p->operand.id;
    }
    const auto cast = as<ast::ImplicitConversion>(operand);
    if (!cast || (cast->conversion != ast::ConversionKind::integral_to_pointer &&
                  cast->conversion != ast::ConversionKind::pointer_to_integral))
      return e;
    return {cast->operand.id, operand.begin + cast->operand.at, operand.end,
            expression_info(cast->operand.id, /*draft=*/true)};
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
      ast::Bits128 bits = ast::int_bits(ins(), lit->value);
      if (k == ast::ConversionKind::boolean) bits = {u64(!ast::wide::is_zero(bits)), 0};
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

  // Whether a non-template function with constraints `c` in normal form (its trailing
  // requires-clause) is a candidate ([over.match.viable]/3): satisfied, or still dependent. An
  // atom whose check was a hard error leaves it in, as for a template's.
  bool trailing_constraints_satisfied(ConstId c) {
    if (c == ConstId::none || templates().constraint_dependent(c)) return true;
    return q_->satisfied(c).value_or(true);
  }
  // Whether function types `a` and `b` have the same parameter-type-lists and implicit object
  // parameters, for comparing non-template candidates by constraints ([over.match.best.general]/2.6).
  bool same_parameter_types(QualType a, QualType b) {
    const TypeRec x = types().rec(types().canonical(a));
    const TypeRec y = types().rec(types().canonical(b));
    if (x.params.size() != y.params.size() || x.variadic != y.variadic || x.fn_cv != y.fn_cv || x.fn_ref != y.fn_ref)
      return false;
    for (std::size_t i = 0; i < x.params.size(); ++i)
      if (types().canonical(x.params[i]) != types().canonical(y.params[i])) return false;
    return true;
  }
  // A non-template function's trailing requires-clause: a class member's as its class records it,
  // with a specialization's arguments substituted; otherwise as declared.
  ConstId trailing_constraints(const ast::DeclSema& d) {
    if (d.constraints == ConstId::none || d.entity == EntityId::none) return d.constraints;
    if (const ClassRec* cr = class_rec(w_->entities.rec(d.entity).scope))
      for (const MemberDeclRec& m : cr->members)
        if (m.entity == d.entity) return m.ineligible ? unsatisfiable_constraint() : m.constraints;
    return d.constraints;
  }
  // Never satisfied: an empty disjunction.
  ConstId unsatisfiable_constraint() {
    return ins().intern_value(ast::VConstraint{1, ins().intern_seq(std::span<const ConstId>{})});
  }
  bool constraints_satisfied(const ast::DeclSema& d, std::span<const TemplateArg> args = {}) {
    // A non-template function: its trailing requires-clause. A pattern with a template head (a
    // generic lambda's call operator, unflagged) substitutes `args` below.
    if (!(d.flags & decl_flag::template_) && args.empty() && ins().seqs.size(d.template_params) == 0)
      return trailing_constraints_satisfied(trailing_constraints(d));
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
  std::vector<AddressMatch> address_matches(const AddressSet& set, std::optional<QualType> fn);

  Ex overload_address(Ex e, QualType to, u32 at_) {
    auto set = address_set(e);
    if (!set) return e;
    auto fn = address_target(to);
    if (!fn) return e;
    return resolve_address(e, *set, fn, at_);
  }

  Ex resolve_address(Ex e, const AddressSet& set, std::optional<QualType> fn, u32 at_);

  // The conversions every operand that needs a prvalue gets ([basic.lval], [conv.lval],
  // [conv.array], [conv.func]).
  Ex rvalue(Ex e);

  // [expr.type.conv]/1, [dcl.type.auto.deduct]: decay-copy, `auto(e)` or `auto{e}`. The result
  // is a prvalue of the decayed type of `e`, copy-initialized from `e`.
  Ex decay_copy(Ex operand, u32 begin, u32 end, bool replay_body);

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
  // An unscoped member enum's inferred type waits for its dependent values to be replayed.
  bool deferred_enum_layout(QualType t) {
    if (!types().is_enum(t)) return false;
    const EnumRec* r = enum_rec(types().enum_of(t));
    return r && !r->underlying;
  }
  QualType underlying_of(QualType t) {
    const EnumRec* r = enum_rec(types().enum_of(t));
    return r && r->underlying ? *r->underlying : builtin(Builtin::int_);
  }

  // [conv.ptr]: an integer literal with value zero, or a prvalue of type std::nullptr_t.
  bool is_null_pointer_constant(const Ex& e) {
    if (!e.ok() || e.glvalue()) return false;
    if (types().is(e.type(), Builtin::nullptr_t)) return true;
    NodeId n = e.id;
    u32 width = e.width();
    while (ast::kind_of(n) == ast::Nodes::index_of<ast::Paren>()) {
      // An identity cast can keep its operand's AST node with a longer source range.
      if (b_->width(n) != width) return false;
      width -= 2;
      n = b_->get<ast::Paren>(n).operand.id;
    }
    if (width != 1 || ast::kind_of(n) != ast::Nodes::index_of<ast::Literal>()) return false;
    // An integer-literal's type is a signed or unsigned integer type of rank int or above
    // ([lex.icon]); a character or bool literal is not an integer-literal.
    auto b = types().builtin_of(e.type());
    if (!b || !ast::is_integral(*b) || *b == Builtin::bool_ || rank_below_int(*b) || is_character(*b)) return false;
    const ConstId value = b_->get<ast::Literal>(n).value;
    return ins().values.is<ast::VInt>(value) && ast::wide::is_zero(ast::int_bits(ins(), value));
  }
  bool is_data_member_pointer(QualType t) {
    return types().is_member_pointer(t) && !types().is_function(types().rec(t).inner);
  }
  Ex null_member_pointer_conversion(Ex e, QualType target) {
    NodeId id = e.id;
    while (ast::kind_of(id) == ast::Nodes::index_of<ast::Paren>()) id = b_->get<ast::Paren>(id).operand.id;
    if (ast::kind_of(id) == ast::Nodes::index_of<ast::Literal>()) return null_member_pointer(target, e.begin, e.end);
    return convert_node(e, ast::ConversionKind::null_pointer, target);
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
  Ex convert(Ex e, QualType to, u32 at_);

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
  std::optional<Ex> cast_base_to_derived(Ex e, QualType target, bool pointer, u32 at_);

  // The conversion function that copy-initialization of an object of non-reference type
  // `target` from class object `e` calls: [over.match.copy]/1.2 for a class `target`, whose
  // candidates yield it or a class derived from it, else [over.match.conv], whose candidates
  // yield a type a standard conversion sequence converts to it. Explicit conversion functions
  // are no candidates. A conversion function template's specialization is the one deduction
  // gives ([temp.deduct.conv]). None when no candidate is viable, or for a source class with
  // bases, whose inherited conversion functions are not considered yet (#1724).
  struct ConversionChoice {
    const MemberDeclRec* member = nullptr;
    std::vector<TemplateArg> args;  // a conversion function template's deduced arguments
    bool ambiguous = false;
  };
  std::optional<ConversionChoice> select_conversion(const Ex& e, QualType target, u32 at_);
  // The call of the conversion function `choice` selects on object `e`, converted to `to`.
  Ex call_conversion(Ex e, const ConversionChoice& choice, QualType to, u32 at_) {
    if (choice.ambiguous) {
      diag(at_, Diag::ambiguous_conversion, e.type(), to);
      return {NodeId::none, e.begin, e.end};
    }
    const EntityId cls = types().class_of(e.type());
    Ex callee = bind_members(cls, {choice.member}, e, false, e.begin, e.end, at_);
    // A template's specialization is named by the arguments deduction gave (#340).
    if (choice.member->is_template) explicit_args_[std::to_underlying(callee.id)] = choice.args;
    Ex call = member_call(callee, {}, e.end, at_);
    if (!call.ok()) return call;
    return convert(call, to, at_);
  }

  // [over.match.conv]: the bounded exact-result case. Copy initialization excludes explicit conversions.
  std::optional<Ex> exact_conversion_call(Ex object, QualType target, u32 at_, bool allow_explicit);

  // A conversion as `static_cast<to>(e)` performs it ([expr.static.cast]); a cast in functional
  // notation or a cast-expression is one when the supported static_cast rules apply
  // ([expr.type.conv]/2, [expr.cast]/4). The downcast slice is for named static_cast expressions.
  Ex static_cast_to(Ex e, QualType to, u32 at_, bool named_static_cast = false);

  // [expr.dynamic.cast]: the source pointer is converted to a prvalue. Same-type
  // and derived-to-base casts do not require a polymorphic source.
  Ex dynamic_cast_to(Ex e, QualType to, u32 begin, u32 end);

  // [expr.dynamic.cast]: the pointer must designate the source polymorphic subobject.
  Ex dynamic_cast_to_void(Ex e, QualType to, u32 begin, u32 end);

  // [except.handle]: no void, rvalue reference, or incomplete object, with the cv void* exception.
  bool check_handler_type(QualType type, u32 at_) {
    if (types().is_dependent(type) && !types().has_placeholder(type)) return true;
    if (types().has_placeholder(type) || types().is_void(type) ||
        (types().is_reference(type) && types().rec(type).rvalue_ref)) {
      diag(at_, Diag::invalid_handler_type, type);
      return false;
    }
    QualType object = types().is_reference(type) ? types().referee(type) : type;
    if (types().is_pointer(type)) {
      QualType pointee = types().pointee(type);
      return types().is_void(pointee) || types().is_function(pointee) || require_complete(pointee, at_);
    }
    if (!require_complete(object, at_)) return false;
    if (!types().is_class(object)) return true;
    diag(at_, Diag::not_supported, lex::Tk::kw_catch);
    return false;
  }

  // [expr.const.cast]/3: the qualification decompositions have identical constructors
  // and terminal types. Either direction can change cv at any level.
  bool const_cast_similar(QualType from, QualType to) {
    if (same_unqualified(from, to)) return true;
    const TypeRec a = types().rec(from), b = types().rec(to);
    if (a.kind != b.kind) return false;
    if (a.kind == TypeKind::pointer) return const_cast_similar(a.inner, b.inner);
    if (a.kind == TypeKind::array && a.bound_value == b.bound_value)
      return const_cast_similar(a.inner, b.inner);
    return false;
  }

  Ex const_cast_to(Ex e, QualType to, u32 at_);

  // [expr.const.cast]/7: whether a conversion between pointer types removes cv-qualifiers at a
  // level both decompose to, or adds them below a level that is not const. The deepest common
  // decomposition decides, since each level's condition depends only on the levels above it.
  bool casts_away_constness(QualType from, QualType to) {
    QualType a = from, b = to;
    bool const_so_far = true;
    while (types().is_pointer(a) && types().is_pointer(b)) {
      a = types().pointee(a);
      b = types().pointee(b);
      const Cv ca = object_cv(a), cb = object_cv(b);
      if (!cv_within(ca, cb) || (ca != cb && !const_so_far)) return true;
      const_so_far = const_so_far && (std::to_underlying(cb) & std::to_underlying(Cv::const_));
    }
    return false;
  }

  // [expr.reinterpret.cast]: the object and function pointer, pointer/integer, identity and
  // glvalue conversions. Every one stays out of constant evaluation ([expr.const]).
  Ex reinterpret_cast_to(Ex e, QualType to, u32 at_);

  std::vector<ast::DeclSema> written_allocation_declarations(NameId name) {
    auto written = q_->namespace_members(EntityId::none, name);
    // A preceding declarator in this item is visible before the item is published.
    for (const OwnDecl& d : own)
      if (d.name == name && d.sema.kind == ast::DeclKind::function &&
          w_->entities.rec(d.sema.entity).scope == EntityId::none)
        written.push_back(d.sema);
    return written;
  }

  // [basic.stc.dynamic] makes the ordinary declarations visible even without a header.
  Ex allocation_name_expr(std::string_view spelling, u32 begin, u32 end) {
    const NameId name = ins().names.intern(spelling);
    const bool array = spelling.ends_with("[]");
    std::vector<ast::DeclSema> found;
    for (AllocationFunction candidate : World::allocation_functions) {
      if (allocation_name(candidate, array) != spelling) continue;
      const auto d = w_->allocation_function(candidate, array);
      q_->use_allocation_function(d.entity, allocation_has_alignment(candidate));
      found.push_back(d);
    }
    auto written = written_allocation_declarations(name);
    found.insert(found.end(), written.begin(), written.end());
    return decls_expr(std::move(found), true, name, begin, end);
  }

  // In a default-aligned new-expression, the sole allocation argument is size_t
  // ([expr.new]). Every admitted written overload takes that exact first parameter. An
  // overload with additional, all-defaulted parameters ties the ordinary one ([over.match]).
  bool ambiguous_allocation(u32 at, bool array) {
    const NameId name = ins().names.intern(allocation_name(AllocationFunction::allocate, array));
    for (const ast::DeclSema& d : written_allocation_declarations(name)) {
      if (d.kind != ast::DeclKind::function) continue;
      const auto count = types().rec(d.type).params.size();
      if (count < 2) continue;
      const Candidate candidate = non_template_call_candidate(d, true);
      if (candidate.required(count) == 1) {
        diag(at, Diag::ambiguous_call, name);
        return true;
      }
    }
    return false;
  }

  Ex usual_allocation_call(Ex size, u32 begin, bool array = false) {
    const auto f = w_->allocation_function(AllocationFunction::allocate, array);
    q_->use_allocation_function(f.entity);
    ExprInfo fn_info{f.type, ValueCategory::lvalue, 0};
    Ex callee = node(ast::DeclRef{fn_info, f.entity}, begin, begin, fn_info);
    ExprInfo call_info{types().rec(f.type).inner, ValueCategory::prvalue, 0};
    Child sizes[] = {at(size, begin)};
    return node(ast::Call{call_info, f.entity, at(callee, begin), b_->list(sizes)}, begin, begin, call_info);
  }

  // Whether class `c` or one of its bases declares an allocation or deallocation function, which
  // a new- or delete-expression would look up before the global ones ([expr.new], [expr.delete]).
  bool class_allocation_functions(EntityId c) {
    const ClassRec* r = class_rec(c);
    if (!r) return false;
    for (const MemberDeclRec& m : r->members)
      if (m.is_function && is_allocation_name(ins().names.spelling(m.name))) return true;
    return std::ranges::any_of(r->bases, [&](const BaseRec& b) { return class_allocation_functions(b.cls); });
  }

  // The class objects the usual `operator new(size_t)` and `operator delete(void*, size_t)` can
  // allocate and release: no class-specific allocation functions (#293) and no new-extended
  // alignment, which would select the `std::align_val_t` overloads ([expr.new]/19).
  bool supported_class_allocation(QualType t) {
    if (class_allocation_functions(types().class_of(t))) return false;
    const auto alignment = layout_of(t, true);
    return alignment && *alignment <= 2u * w_->target.pointer_size;
  }

  // [dcl.init.general]/8: default-initialization of a const `t` initializes it, by a
  // user-provided default constructor or by default member initializers for every member.
  bool const_default_constructible(QualType t) {
    t = types().innermost_array_element(t);
    if (!types().is_class(t)) return false;
    const ClassRec* r = class_rec(types().class_of(t));
    if (!r) return false;
    for (const MemberDeclRec& m : r->members) {
      if (!m.is_ctor || m.is_template || m.is_deleted || m.entity == r->defaulted_default_ctor) continue;
      // A default constructor: every parameter has a default argument ([class.default.ctor]).
      const std::size_t params = types().rec(m.type).params.size();
      const std::size_t required = m.defaults.empty() ? params : std::size_t(std::ranges::count(m.defaults, NodeId::none));
      if (required == 0) return true;
    }
    if (r->union_) return r->fields.empty() || std::ranges::count_if(r->fields, &FieldRec::dmi) == 1;
    for (const FieldRec& f : r->fields)
      if (!f.dmi && !const_default_constructible(f.type)) return false;
    return std::ranges::all_of(r->bases, [&](const BaseRec& b) {
      return const_default_constructible(types().class_type(b.cls));
    });
  }

  // The written default constructor must be a usable, unambiguous trivial leaf.
  bool gcc_union_variant_trivial_default(const ClassRec& record) {
    if (!record.defined || !record.member_declarations_valid || !record.trivial_defaulted_default_ctor)
      return false;
    const EntityId ctor = record.defaulted_default_ctor;
    const auto member = std::ranges::find(record.members, ctor, &MemberDeclRec::entity);
    if (member == record.members.end() || !member->is_ctor || member->is_template || member->is_deleted ||
        record.access_of(ctor) != ast::Access::public_)
      return false;
    if (const auto destructor = record.destructor_declaration();
        destructor != EntityId::none && record.access_of(destructor) != ast::Access::public_)
      return false;
    return std::ranges::none_of(record.members, [&](const MemberDeclRec& other) {
      if (!other.is_ctor || other.entity == ctor) return false;
      const auto params = types().rec(other.type).params.size();
      return other.is_template || params == 0 ||
             (other.defaults.size() == params && !std::ranges::contains(other.defaults, NodeId::none));
    });
  }

  // GCC's direct empty allocation checks the implicit union default constructor. Use the
  // ordinary base and field defaults without treating a trivial defaulted leaf as nontrivial.
  bool gcc_union_variant_prevents_implicit_default(QualType type, std::set<EntityId>& visited) {
    type = types().innermost_array_element(type);
    if (!types().is_class(type)) return false;
    const EntityId cls = types().class_of(type);
    const ClassRec* record = class_rec(cls);
    if (!record || !visited.insert(cls).second) return false;
    if (record->user_ctors) return !gcc_union_variant_trivial_default(*record);
    if (record->implicit_default_ctor != EntityId::none) return true;
    return std::ranges::any_of(record->bases, [&](const BaseRec& base) {
      return gcc_union_variant_prevents_implicit_default(types().class_type(base.cls), visited);
    }) || std::ranges::any_of(record->fields, [&](const FieldRec& field) {
      return field.dmi || field.init != ConstId::none ||
             (!record->union_ && gcc_union_variant_prevents_implicit_default(field.type, visited));
    });
  }

  // [expr.new]/26: the new-initializer initializes the class object as in a declaration
  // ([dcl.init.general]): default-initialization without one, value-initialization for `()`,
  // and direct- or direct-list-initialization otherwise.
  Ex new_class_initializer(QualType t, const std::optional<std::vector<Ex>>& args, const Braced* braces, u32 begin,
                           u32 end);

  // [expr.new]: initialize the object after the call to the global allocation function.
  Ex new_object(QualType t, const std::optional<std::vector<Ex>>& args, const Braced* braces, u32 begin, u32 end,
                std::optional<Ex> placement);

  // [expr.delete]: cv-qualifiers are ignored when passing the address to the global deallocator.
  Ex delete_object(Ex e, u32 begin, u32 end);

  // Limit the object size to the largest positive byte distance representable by ptrdiff_t.
  u64 max_new_array_count(u64 element_size) const {
    int size_bits = shape(w_->target, w_->target.ptrdiff_type).bits - 1;
    return ((u64{1} << size_bits) - 1) / element_size;
  }

  // [expr.new]: a potentially evaluated constant bound is checked before unsigned conversion.
  bool valid_new_bound(Ex bound, u64 element_size, ConstId known) {
    ValueRec value = w_->values.rec(known);
    int size_bits = shape(w_->target, w_->target.ptrdiff_type).bits - 1;
    u64 max_count = max_new_array_count(element_size);
    bool negative = false;
    bool oversized = false;
    if (value.kind == ValueKind::integer) {
      negative = ast::is_signed(w_->target, *types().builtin_of(value.type)) &&
                 ast::is_negative(ast::int_bits(ins(), known));
      oversized = !negative && (value.wide || value.bits > max_count);
    } else if (value.kind == ValueKind::floating) {
      negative = value.real < 0;
      // Check representability before truncating to an integer.
      oversized = !std::isfinite(value.real) || value.real >= std::ldexp(1.0, size_bits) ||
                  (!negative && u64(value.real) > max_count);
    }
    if (negative || oversized) {
      diag(bound.begin, negative ? Diag::negative_new_bound : Diag::oversized_new_bound);
      return false;
    }
    return true;
  }

  // Prove that replacing the bound with its evaluated value leaves no runtime work.
  // Clang's wide-bound case first converts that constant to the target's allocation-size type.
  bool arithmetic_array_bound(Ex bound, bool clang_wide);

  Ex new_array(QualType t, Ex bound, bool literal_arithmetic_bound, const std::optional<std::vector<Ex>>& args,
               const Braced* braces, u32 begin, u32 end);

  Ex delete_array(Ex operand, u32 begin, u32 end) {
    Ex fail{NodeId::none, begin, end};
    if (!operand.ok()) return fail;
    QualType v = builtin(Builtin::void_);
    if (dependent(operand)) return dependent_node({operand}, begin, end, v);
    if (types().is_class(operand.type())) {
      diag(begin, Diag::not_supported, lex::Tk::kw_delete);  // contextual conversion from a class
      return fail;
    }
    if (!types().is_pointer(operand.type()) || !types().is_object(types().pointee(operand.type()))) {
      diag(begin, Diag::delete_requires_pointer, operand.type());
      return fail;
    }
    if (!scalar_allocation_type(types().pointee(operand.type()))) {
      diag(begin, Diag::not_supported, lex::Tk::kw_delete);
      return fail;
    }
    Ex pointer = rvalue(operand);
    if (!pointer.ok()) return fail;
    const EntityId f = w_->allocation_function(AllocationFunction::deallocate, /*array=*/true).entity;
    q_->use_allocation_function(f);
    ExprInfo info{v, ValueCategory::prvalue, 0};
    return node(ast::DeleteArray{info, f, at(pointer, begin)}, begin, end, info);
  }

  bool scalar_allocation_type(QualType t) {
    return types().is_integral(t) || types().is_pointer(t);
  }

  bool checking_class_alignment = false;
  struct ClassAlignmentOperand {
    bool& checking;
    bool saved;
    explicit ClassAlignmentOperand(Sema& s)
        : checking(s.checking_class_alignment), saved(std::exchange(checking, true)) {}
    ~ClassAlignmentOperand() { checking = saved; }
  };

  ConstId layout_query_value(QualType operand, bool align) {
    const std::array args{templates().type_arg(operand)};
    const auto op = align ? ast::DepOp::alignof_type : ast::DepOp::sizeof_type;
    return ins().intern_value(ast::VDepOp{op, ins().intern_seq(std::span<const TemplateArg>(args))});
  }
  // A dependent layout query whose enclosing construct reparses the written operand.
  Ex dependent_layout_query(QualType operand, bool align, u32 begin, u32 end) {
    return literal(builtin(w_->target.size_type), layout_query_value(operand, align), ValueCategory::prvalue, begin, end);
  }

  // [expr.sizeof], [expr.alignof]: a `size_t` prvalue.
  Ex size_of(QualType t, bool align, u32 begin, u32 end);
  // A concrete operand is checked even when the other operand needs template substitution.
  // A missing class fact or layout limits evaluation, but the expression can still be formed.
  bool bit_cast_operand(QualType type, u32 at_, bool destination);

  std::optional<u64> bit_cast_size(QualType type) {
    const QualType element = types().innermost_array_element(type);
    if (types().is_class(element) && q_->class_layout_uncertain(types().class_of(element))) return std::nullopt;
    return layout_of(type, false);
  }

  // Verify that each ordinary unsigned field contributes exactly its storage bits. The
  // lowering can then derive the layout from the field widths in declaration order.
  bool bit_cast_representation(QualType type, u64 size);

  Ex bit_cast(QualType destination, Ex source, u32 begin, u32 end, u32 type_at);

  // The supported direct-member slice of [support.types.layout]. Its result uses the
  // same target record offsets as code generation and the target's size_t.
  Ex offset_of(QualType t, NameId name, u32 begin, u32 end, u32 name_at);

  // A complete object type's size or alignment: from the target, and for a class by
  // `ast::record_layout` over its members' (#194), the rule the back end lays it out by.
  std::optional<u64> layout_of(QualType t, bool align) {
    if (align) t = types().innermost_array_element(t);  // [expr.alignof]: an array's element alignment
    auto p = place_of(t);
    if (!p) return std::nullopt;
    return align ? p->align : p->size;
  }
  // What a complete object type's layout says of it as a component, once per class
  // (`class_places_`).
  std::optional<ast::Placed> place_of(QualType t);
  // A complete class's layout (`ast::record_layout`, the rule the back end lays it out by).
  std::optional<ast::RecordLayout> class_layout(EntityId ce) {
    if (auto it = class_places_.find(std::to_underlying(ce)); it != class_places_.end()) return it->second;
    const ClassRec* c = is_complete(types().class_type(ce)) ? class_rec(ce) : nullptr;
    if (!c) return std::nullopt;
    auto layout = record_layout(*c, ce);
    if (layout) class_places_.emplace(std::to_underlying(ce), *layout);
    return layout;
  }
  // Also used on a class about to be published, before its own alignment has been applied.
  std::optional<ast::RecordLayout> record_layout(const ClassRec& c, EntityId ce);
  std::map<u32, ast::RecordLayout> class_places_;  // complete classes' layouts

  // ------------------------------------------------------------ placeholder types ([dcl.spec.auto])

  // What declared type `t`, which holds a placeholder, deduces from initializer `e`
  // ([dcl.type.auto.deduct]): for `decltype(auto)`, `e`'s decltype ([dcl.type.decltype]); for
  // `auto`, `t` with `auto` replaced by an invented type parameter, deduced as from a call with
  // `e` as the argument ([temp.deduct.call]). `Builtin::dependent` while `e` is type-dependent;
  // none after a diagnosed failure, at `at_`.
  // `replaced` receives what replaced the placeholder, which several declarators of one
  // declaration must agree on ([dcl.type.auto.deduct]/2).
  std::optional<QualType> deduce_placeholder(QualType t, Ex e, u32 at_, QualType* replaced = nullptr);
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
  // deduced reference to the initializer. The draft gives the copy its declared cv.
  std::optional<std::pair<QualType, Ex>> array_binding(QualType declared, Ex source, u64 names, u32 at_);

  // [dcl.struct.bind]/7: a complete std::tuple_size<E> with any member named value takes
  // precedence, even if its value could not be used to obtain the size.
  bool reject_tuple_binding(QualType type, u32 at_) {
    auto std_scope = scope_member(global_scope(), ins().names.intern("std"));
    if (!std_scope || !std_scope->ns) return false;
    auto tmpl = namespace_type_template(*std_scope->ns, ins().names.intern("tuple_size"));
    if (!tmpl) return false;
    const TemplateArg arg = templates().type_arg(type);
    auto size = class_template_id(*tmpl, std::span(&arg, 1), at_);
    if (!size) return true;  // a diagnosed template-id error
    if (!types().is_class(*size) || !find_member(types().class_of(*size), ins().names.intern("value"))) return false;
    diag(at_, Diag::binding_tuple_unsupported, type);
    return true;
  }

  // [class.dtor]: the destructor of the hidden object or materialized temporary is potentially
  // invoked. A reference to an existing object does not itself invoke a destructor.
  bool check_binding_destructor(QualType type, u32 at_) {
    return check_destructor_access(type, at_, Diag::binding_unusable_destructor);
  }

  // Follow the glvalue designator through the operations in [class.temporary]. A temporary
  // reached through a conditional branch or array subscript needs additional lowering (#157).
  bool check_binding_lifetime(Ex e, bool unsupported_path = false);

  // [dcl.struct.bind]/1, /8: initialize one hidden object or reference and project its data
  // members in declaration order. This slice uses only addressable public fields of a root class.
  std::optional<std::pair<QualType, const ClassRec*>> class_binding(QualType declared, const Ex& source, u64 names, u32 at_);

  // [dcl.type.decltype]/1: an unparenthesized id-expression or member access names its entity's
  // declared type; any other expression has its type, as `T&&` for an xvalue and `T&` for an
  // lvalue.
  QualType decltype_of(Ex e, bool parenthesized) {
    if (!parenthesized) {
      if (auto l = as<ast::LocalRef>(e)) return local_declared_type(locals_[l->local]);
      if (auto it = declared_types_.find(std::to_underlying(e.id)); it != declared_types_.end()) return it->second;
      // The declared type: the declaration's that named the entity (a static data member's is
      // its type, a function's its function type), a data member's from its class.
      if (as<ast::DeclRef>(e)) return e.type();
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
  bool check_base_path(QualType ct, EntityId b, const std::optional<std::vector<EntityId>>& path, int paths, u32 at_,
                       bool check_access = true) {
    if (!path || paths != 1) {
      diag(at_, Diag::ambiguous_base, types().class_type(b), Types::unqualified(ct));
      return false;
    }
    if (!check_access) return true;
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
  // Without `check_access`, the edges' access is not checked: a conversion the language makes
  // whatever their access.
  Ex to_base(Ex e, EntityId b, u32 at_, bool binding_lvalue_reference = false, bool check_access = true);

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

  bool function_indirection_convertible(QualType from, QualType to) {
    return types().is_pointer(from) && types().is_pointer(to) &&
           types().function_pointer_conversion(types().pointee(from), types().pointee(to));
  }

  // [expr.type]: the composite type of otherwise identical function pointers is the throwing one.
  std::optional<QualType> composite_function_pointer(QualType a, QualType b) {
    if (function_indirection_convertible(a, b)) return b;
    if (function_indirection_convertible(b, a)) return a;
    return std::nullopt;
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

  QualType integral_common_type(const Ex& a, const Ex& b) {
    const auto left = sema::promoted(w_->target, *types().builtin_of(a.type()), a.info.bit_width);
    const auto right = sema::promoted(w_->target, *types().builtin_of(b.type()), b.info.bit_width);
    return builtin(common_integer(w_->target, left, right));
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

  struct OrdinaryOperatorChoice {
    const ast::DeclSema* nonmember = nullptr;
    bool ambiguous = false;
  };

  // Rank direct ordinary binary candidates in operand order. This subset compares
  // exact class and arithmetic arguments, including arithmetic conversions by value.
  OrdinaryOperatorChoice ordinary_binary_choice(NameId name, std::span<const Ex> operands,
                                                 std::span<const ast::DeclSema> nonmembers);

  static void add_operator_nonmember(std::vector<ast::DeclSema>& set, const ast::DeclSema& declaration) {
    if (declaration.kind == ast::DeclKind::function && declaration.entity != EntityId::none &&
        !std::ranges::contains(set, declaration.entity, &ast::DeclSema::entity))
      set.push_back(declaration);
  }

  std::vector<ast::DeclSema> ordinary_operator_nonmembers(NameId name) {
    std::vector<ast::DeclSema> set;
    for (const auto& o : own)
      if (o.name == name) add_operator_nonmember(set, o.sema);
    for (DeclId d : q_->visible_decls(name, directives())) add_operator_nonmember(set, q_->decl_sema(d));
    return set;
  }

  void add_operator_adl(std::vector<ast::DeclSema>& set, NameId name, std::span<const Ex> operands,
                        bool at_instantiation) {
    auto associations = adl_associations(operands, at_instantiation, /*ordinary_call=*/!at_instantiation);
    for_each_associated_friend(associations, name, at_instantiation,
                               [&](const ast::DeclSema& d) { add_operator_nonmember(set, d); });
    for (EntityId ns : associations.namespaces)
      for (const ast::DeclSema& d : q_->adl_functions(ns, name, at_instantiation)) add_operator_nonmember(set, d);
  }

  // [over.match.oper]: an operator with an operand of class or enumeration type calls an operator
  // function when one is viable: a member `operator@` of the left operand's class, else the
  // non-member ones that unqualified lookup and argument-dependent lookup find
  // ([over.match.oper]/3). If none is viable, try the built-in operator. The ordinary binary
  // recovery compares members and nonmembers when a member-only probe diagnoses access,
  // deletion or ambiguity.
  // General joint ranking with other candidates is outside this subset.
  std::optional<Ex> overloaded(std::string_view op, std::vector<Ex> operands, u32 begin, u32 end, u32 op_at,
                               bool rank_failed_member = false);

  // [over.match.oper]/3.4: the rewritten candidates of `x == y` and `x != y` when no
  // `operator==` (for `==`) or `operator!=` (for `!=`) is viable: `operator==` as `x == y` for
  // `!=`, then `operator==` with the operands reversed, as `y == x`; `!=` negates the result.
  // Only for an operand of class type. The selected `operator==` must be a rewrite target: no
  // `operator!=` corresponding to it is declared in its class, or its namespace for a
  // non-member; and it returns cv `bool`.
  // (A rewritten candidate is not yet ranked against a viable non-rewritten one, including the
  // built-in candidates for enumerations, nor are other rewrite targets tried when the selected
  // one is not one: #295.)
  std::optional<Ex> rewritten_equality(bool not_equal, Ex l, Ex r, u32 begin, u32 end, u32 op_at);

  Ex builtin_address_of(Ex e, u32 begin, u32 end, u32 op_at) {
    if (e.glvalue() && e.info.bit_width) {
      diag(op_at, Diag::bit_field_address);
      return {NodeId::none, begin, end};
    }
    QualType pointee = e.type();
    if (is_bound_member(e) && address_set(e)) pointee = builtin(Builtin::overload_set);
    else if (!e.glvalue()) {
      diag(op_at, Diag::not_lvalue_address, e.type());
      return {NodeId::none, begin, end};
    }
    ExprInfo info{types().pointer(pointee), ValueCategory::prvalue, 0};
    return node(ast::Unary{info, ast::UnaryOp::address_of, at(e, begin)}, begin, end, info);
  }

  Ex unary(ast::UnaryOp op, Ex e, u32 begin, u32 end, u32 op_at);

  enum class BinOp : u8 {
    mul, div, rem, add, sub, shl, shr, lt, gt, le, ge, eq, ne, bit_and, bit_xor, bit_or, logical_and, logical_or, comma,
    spaceship, dot_star, arrow_star,
  };

  static constexpr std::string_view binary_spellings[] = {"*", "/", "%", "+", "-", "<<", ">>", "<", ">", "<=", ">=",
                                                          "==", "!=", "&", "^", "|", "&&", "||", ",", "<=>"};

  // The built-in member pointer operators ([expr.mptr.oper]). Access checking takes place when
  // the address expression names the field ([expr.unary.op]).
  Ex member_pointer_access(Ex object, Ex pointer, bool arrow, u32 op_at);

  Ex binary(BinOp op, Ex l, Ex r, u32 op_at);

  enum class Composite : u8 { none, converted, failed };
  // [expr.type]/4: two pointer operands converted to their composite pointer type, as the
  // relational, equality and three-way operators need: a pointer to void and one to an object,
  // two function pointers (when `function_pointers`), or two object pointers, a pointer to a
  // derived class becoming one to its base class and the cv-qualifiers combined. `none` when
  // the operands have no composite pointer type (not diagnosed); `failed` when a conversion was
  // diagnosed.
  Composite composite_pointers(Ex& a, Ex& c, bool function_pointers, u32 op_at);

  // The comparison category types ([cmp.categories]), weakest first: the common comparison
  // category of several is the weakest of them ([class.spaceship]).
  enum class Ordering : u8 { partial, weak, strong };
  struct ComparisonCategory {
    QualType type;
    std::vector<EntityId> values;  // as ast::ThreeWay::results
  };
  // [cmp.categories.pre]: `std::partial_ordering`, `std::weak_ordering` or `std::strong_ordering`,
  // which <compare> declares, found by qualified lookup in `std`, with the static data members
  // that name its values. Diagnosed at `at_` when it is not declared so or is incomplete.
  std::optional<ComparisonCategory> comparison_category(Ordering k, std::optional<u32> at_);
  // The comparison category type `t` is, if it is one.
  std::optional<Ordering> ordering_of(QualType t) {
    t = Types::unqualified(types().canonical(t));
    if (!types().is_class(t)) return std::nullopt;
    for (Ordering k : {Ordering::partial, Ordering::weak, Ordering::strong})
      if (const auto c = comparison_category(k, std::nullopt); c && types().canonical(c->type) == t) return k;
    return std::nullopt;
  }

  // [expr.spaceship]: the built-in three-way comparison, for operands of arithmetic, enumeration
  // or object pointer type.
  Ex builtin_three_way(Ex l, Ex r, u32 begin, u32 end, u32 op_at);

  // [over.match.oper], [over.match.best.general]: rank the direct and synthesized
  // reversed orientations as one set. Leave the existing lookup and resolution path
  // in charge when other candidate or conversion kinds may affect the result.
  std::optional<Ex> ranked_three_way(BinOp op, Ex l, Ex r, u32 begin, u32 end, u32 op_at);

  // [over.match.oper]/3.4, /8: the rewritten candidates of a relational operator `x @ y` and of
  // `x <=> y`, when no ordinary candidate is viable and an operand has class type: an
  // `operator<=>` as `(x <=> y) @ 0`, then, synthesized with the operands reversed, as
  // `0 @ (y <=> x)` (only the reversed one for `<=>` itself). A rewritten expression's own
  // operator is not rewritten again. With enumeration operands and no class, only where the
  // built-in operator is not viable: a scoped enumeration with anything but itself.
  // (Rewritten candidates are not yet ranked against viable ordinary ones: #295.)
  std::optional<Ex> rewritten_three_way(BinOp op, Ex l, Ex r, u32 begin, u32 end, u32 op_at) {
    if (rewriting_three_way_) return std::nullopt;
    const QualType lt = types().referee(l.type()), rt = types().referee(r.type());
    if (!types().is_class(lt) && !types().is_class(rt)) {
      const bool builtin_viable = (!is_scoped_enum(lt) && !is_scoped_enum(rt)) ||
                                  types().canonical(Types::unqualified(lt)) == types().canonical(Types::unqualified(rt));
      if (builtin_viable || (!types().is_enum(lt) && !types().is_enum(rt))) return std::nullopt;
    }
    const std::size_t n = diag_count();
    std::optional<Ex> compared;
    bool reversed = false;
    if (op != BinOp::spaceship) compared = overloaded("<=>", {l, r}, begin, end, op_at);
    if (!compared) {
      truncate_diags(n);
      compared = overloaded("<=>", {r, l}, begin, end, op_at);
      reversed = true;
    }
    if (!compared) {
      truncate_diags(n);
      return std::nullopt;
    }
    if (!compared->ok()) return Ex{NodeId::none, begin, end};
    return apply_three_way_rewrite(op, *compared, reversed, begin, end, op_at);
  }

  Ex apply_three_way_rewrite(BinOp op, Ex compared, bool reversed, u32 begin, u32 end, u32 op_at) {
    // A literal `0`, one token wide, so it is a null pointer constant too ([conv.ptr]), as the
    // library's comparison operators may take it.
    const QualType int_type = builtin(Builtin::int_);
    const u32 zero_at = reversed ? begin : end - 1;
    const Ex zero = literal(int_type, w_->values.integer(int_type, 0), ValueCategory::prvalue, zero_at, zero_at + 1);
    struct Restore {
      bool& flag;
      ~Restore() { flag = false; }
    } restore{rewriting_three_way_};
    rewriting_three_way_ = true;
    const Ex result = reversed ? binary(op, zero, compared, op_at) : binary(op, compared, zero, op_at);
    return Ex{result.id, begin, end, result.info};
  }
  bool rewriting_three_way_ = false;

  enum class AsgOp : u8 { assign, mul, div, rem, add, sub, shl, shr, bit_and, bit_xor, bit_or };

  // [expr.assign]
  Ex assign(AsgOp op, Ex l, Ex r, u32 op_at);

  // [expr.assign]/9: `x = {…}` assigns `T{…}`, list-initialized, to an object of type T.
  Ex assign_list(Ex l, const Braced& b, u32 op_at);

  bool supports_class_throw(EntityId cls, Ex value);

  // [expr.throw], [except.throw]: retain the operand, with array and function decay.
  Ex throw_expression(std::optional<Ex> operand, u32 begin, u32 end);

  // Retain a typed integral operand without reading a named object. The conditional's
  // substitution and the concrete evaluator each read only the chosen alternative.
  std::optional<TemplateArg> structural_conditional_operand(Ex e) {
    while (auto p = as<ast::Paren>(e)) e.id = p->operand.id;
    const QualType type = types().canonical(e.type());
    if (auto lit = as<ast::Literal>(e)) {
      const auto& values = ins().values;
      if (values.is<ast::VInt>(lit->value) || values.is<ast::VDepField>(lit->value) ||
          values.is<ast::VDepConditional>(lit->value) ||
          (values.is<ast::VDepParam>(lit->value) && values.get<ast::VDepParam>(lit->value).depth != dependent_depth))
        return TemplateArg{TemplateArgKind::value, type, lit->value};
    }
    if (auto decl = as<ast::DeclRef>(e); decl && !decl->reference && !dependent(e)) {
      ConstId value = ins().intern_value(ast::VEntity{decl->entity});
      return TemplateArg{TemplateArgKind::value, type, value};
    }
    if (auto unary = as<ast::Unary>(e);
        unary && (unary->op == ast::UnaryOp::plus || unary->op == ast::UnaryOp::minus)) {
      if (auto lit = as<ast::Literal>({unary->operand.id}); lit && ins().values.is<ast::VInt>(lit->value)) {
        auto bits = ast::int_bits(ins(), lit->value);
        if (unary->op == ast::UnaryOp::minus) bits = ast::wide::neg(bits);
        return TemplateArg{TemplateArgKind::value, type, w_->values.integer(type, bits)};
      }
    }
    return std::nullopt;
  }

  Ex structural_conditional(Ex cond, Ex t, Ex f, u32 q_at);

  // [expr.cond]
  Ex conditional(Ex cond, Ex t, Ex f, u32 q_at, bool retain_class_parameter = false);

  // [expr.call]: a call to a named function or through a pointer to function. Each argument
  // copy-initializes its parameter; arguments matching a C-style ellipsis get the default
  // argument promotions.
  // A call that takes a default still dependent in a template's definition has a value only
  // its instantiation knows ([temp.dep.constexpr]): its type stays, its value is dependent.
  Ex call(Ex callee, std::vector<Ex> args, u32 end, u32 paren_at, std::optional<u32> lookup_at = std::nullopt,
          bool enum_operator = false) {
    const bool outer = std::exchange(dependent_default_, false);
    Ex r = resolved_call(callee, std::move(args), end, paren_at, lookup_at, enum_operator);
    const bool used = std::exchange(dependent_default_, outer);
    if (used && r.ok() && !dependent(r)) return dependent_node({r}, r.begin, r.end, r.type(), r.info.category);
    return r;
  }
  Ex resolved_call(Ex callee, std::vector<Ex> args, u32 end, u32 paren_at, std::optional<u32> lookup_at,
                   bool enum_operator) {
    u32 begin = callee.begin;
    std::vector<NodeId> defaults;  // the chosen function's default arguments
    DefaultArgumentOrigin default_origin = DefaultArgumentOrigin::interned;
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
      // BOOTSTRAP: -Warray-bounds: reserved and appended, as `parameters` in `member_call` (#748).
      // Without the bug: `std::vector<Ex> all{callee};`, `all.insert(all.end(), args.begin(), args.end());`
      std::vector<Ex> all;
      all.reserve(1 + args.size());
      all.push_back(callee);
      all.append_range(args);
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
      const NameId name = w_->entities.rec(set.front().entity).name;
      const bool allocation = is_allocation_name(ins().names.spelling(name));
      auto ex = explicit_args_.find(std::to_underlying(callee.id));
      std::span<const TemplateArg> explicit_args;
      if (ex != explicit_args_.end()) explicit_args = ex->second;
      std::vector<const Braced*> lists;
      if (braced != args.end())
        for (const Ex& a : args) lists.push_back(braced_list(a));
      std::vector<ArgShape> shapes;
      if (lists.empty())
        for (const Ex& a : args) shapes.push_back(argument_shape(a));
      // [over.match.oper]: for a binary operator with no class operand, a nonmember
      // candidate needs an enum or enum-reference parameter matching its operand.
      auto eligible_operator_type = [&](QualType type) {
        if (!enum_operator) return true;
        auto unqualified_type = [&](QualType t) {
          return Types::unqualified(types().canonical(types().referee(t)));
        };
        const auto function = types().rec(type);
        for (std::size_t i = 0; i < std::min(args.size(), function.params.size()); ++i) {
          const QualType operand = unqualified_type(args[i].type());
          if (types().is_enum(operand) && operand == unqualified_type(function.params[i])) return true;
        }
        return false;
      };
      std::vector<Candidate> cands;
      std::vector<const ast::DeclSema*> decls;
      for (const ast::DeclSema& d : set) {
        bool deleted = (d.flags & decl_flag::deleted) != 0;
        if (!(d.flags & decl_flag::template_)) {
          // Explicit template arguments name only templates ([temp.arg.explicit]).
          const ConstId constraints = trailing_constraints(d);
          if (ex != explicit_args_.end() || !eligible_operator_type(d.type) ||
              !trailing_constraints_satisfied(constraints)) continue;
          cands.push_back(non_template_call_candidate(d, allocation));
          cands.back().constraints = constraints;
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
        Templates t = templates_for_use(callee.begin);
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
        if (!st || !eligible_operator_type(*st)) continue;
        cands.push_back({specialization(d, *targs, *st), *st, true, deleted, &d, found_defaults(d)});
        decls.push_back(&d);
      }
      auto chosen = resolve(cands, args, callee.begin, name, std::nullopt, std::nullopt, lists);
      if (!chosen) return {NodeId::none, begin, end};
      const ast::DeclSema& d = *decls[*chosen];
      const Candidate& k = cands[*chosen];
      // The selected specialization's exception specification is needed ([except.spec]/13): it is
      // part of its type, and of its identity, as when its address is taken.
      EntityId selected = k.entity;
      QualType selected_type = k.type;
      if (k.pattern) {
        auto resolved = q_->replay_member_exception(*this, *k.pattern, k.entity, k.type);
        if (!resolved) return {NodeId::none, begin, end};
        if (*resolved != k.type) {
          selected_type = *resolved;
          selected = specialization(*k.pattern, w_->entities.rec(k.entity).args, selected_type);
        }
      }
      if ((d.flags & decl_flag::consteval_) ||
          (!unevaluated && k.pattern && (d.flags & decl_flag::constexpr_) && q_->is_immediate_function(selected)))
        consteval_.insert(std::to_underlying(selected));
      // [temp.inst]: a known return type suffices for unevaluated calls to ordinary templates.
      // Keep the existing body demand for constant-evaluation candidates and placeholder returns.
      // A member of the current instantiation has no instantiation of its own: each specialization
      // of its class names its own ([temp.inst]).
      const EntityId owner = (d.flags & decl_flag::member) ? w_->entities.rec(d.entity).scope : EntityId::none;
      const bool unbound = in_pattern() && owner != EntityId::none && types().is_dependent(types().class_type(owner));
      if (k.pattern && !unbound &&
          (!unevaluated || (d.flags & (decl_flag::member | decl_flag::constexpr_ | decl_flag::consteval_)) ||
           types().has_placeholder(types().rec(selected_type).inner)))
        demands.push_back({selected, ast::Loc{item_, loc_base_ + callee.begin}});
      auto kt = deduced_function(selected, selected_type, callee.begin);
      if (!kt) return {NodeId::none, begin, end};
      ExprInfo ci{*kt, ValueCategory::lvalue, 0};
      callee = node(ast::DeclRef{ci, selected}, callee.begin, callee.end, ci);
      defaults = k.defaults;
      default_origin = k.default_origin;
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
      // Found by class member lookup in the object's class ([over.call.object]/1,
      // [class.member.lookup]): its own, or a base's, named through that base subobject.
      const auto found = find_member(c, op);
      if (!found || !found->member || !found->member->is_function) {
        diag(paren_at, Diag::not_callable, callee.type());
        return {NodeId::none, begin, end};
      }
      if (found->ambiguous) {
        diag(paren_at, Diag::ambiguous_member, op, Types::unqualified(callee.type()));
        return {NodeId::none, begin, end};
      }
      Ex object = callee;
      // A static operator() needs no base subobject ([over.call.object], [expr.ref]); a prvalue
      // object is materialized before the derived-to-base conversion ([class.temporary]/2).
      if (found->owner != c && !std::ranges::all_of(member_functions(found->owner, op), &MemberDeclRec::is_static)) {
        if (!object.glvalue()) object = materialize(object, object.type(), false);
        object = to_base(object, found->owner, paren_at);
        if (!object.ok()) return {NodeId::none, begin, end};
      }
      Ex bound = bound_member(found->owner, op, object, false, begin, callee.end, paren_at, c);
      return member_call(bound, std::move(args), end, paren_at);
    }
    EntityId function = EntityId::none;
    QualType ft = callee.type();
    auto unparenthesized = [&](Ex expression) {
      while (auto p = as<ast::Paren>(expression)) expression = Ex{p->operand.id};
      return expression;
    };
    Ex named = unparenthesized(callee);  // `(f)()` takes f's default arguments too ([expr.call]/1)
    const auto declaration = types().is_function(ft) ? as<ast::DeclRef>(named) : std::nullopt;
    const EntityId default_source = declaration ? declaration->entity : EntityId::none;
    if (declaration && defaults.empty()) {
      defaults = defaults_of(named, default_source);
      default_origin = DefaultArgumentOrigin::interned;
    }
    if (declaration && (named.id == callee.id || is_consteval(default_source) ||
                        ordinary_immediate_member(default_source, types().rec(ft).inner))) {
      function = default_source;
    } else {
      // The address of a named immediate function is itself a known callee. It does not
      // supply default arguments because the call is through a pointer ([expr.call]).
      if (auto address = as<ast::Unary>(named); address && address->op == ast::UnaryOp::address_of) {
        if (auto d = as<ast::DeclRef>(unparenthesized(Ex{address->operand.id})); d && is_consteval(d->entity))
          function = d->entity;
      }
      // A substituted constant argument can hold the immediate function pointer directly.
      if (types().is_pointer(ft) || types().is_function(ft)) {
        if (const auto value = as<ast::Literal>(named)) {
          if (const auto target = immediate_result_entity(value->value)) {
            function = *target;
            consteval_.insert(std::to_underlying(function));
            if (compatibility() == ast::Compatibility::gcc && !immediate_context && !unevaluated)
              escalate_immediate();
          }
        }
      }
      callee = rvalue(callee);
      ft = callee.type();
      if (types().is_pointer(ft) && types().is_function(types().pointee(ft))) ft = types().pointee(ft);
    }
    if (!types().is_function(ft)) {
      diag(paren_at, Diag::not_callable, callee.type());
      return {NodeId::none, begin, end};
    }
    TypeRec fr = types().rec(ft);
    auto children = call_args(fr, args, begin, paren_at, begin, defaults, default_origin, default_source);
    if (!children) return {NodeId::none, begin, end};
    ExprInfo info = result_info(fr.inner);
    return node(ast::Call{info, function, at(callee, begin), b_->list(*children), written_library_builtin(function, fr)},
                begin, end, info);
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

  struct AdlAssociations {
    std::vector<EntityId> namespaces;
    std::vector<EntityId> classes;
  };

  // [basic.lookup.argdep]/3: the namespaces and classes associated with the argument types.
  // The namespace query adds each nearest non-inline ancestor and its inline namespace set.
  AdlAssociations adl_associations(std::span<const Ex> args, bool at_instantiation, bool ordinary_call);

  // [basic.lookup.argdep]/4: a reachable associated class contributes its friends even
  // when their namespace has yet to bind the name for ordinary lookup.
  void for_each_associated_friend(const AdlAssociations& associations, NameId name, bool at_instantiation,
                                  auto&& add) {
    auto add_friend = [&](const ast::DeclSema& d) {
      if (d.entity != EntityId::none) add(d);
    };
    for (EntityId cls : associations.classes) {
      if (const ClassRec* rec = q_->adl_class(cls, at_instantiation)) {
        for (const ast::MemberRec& f : rec->adl_friends)
          if (w_->entities.rec(f.entity).name == name) add_friend(q_->friend_decl(f));
      } else if (!at_instantiation) {
        for (DeclId f : q_->earlier_adl_friends(cls, name)) add_friend(q_->decl_sema(f));
      }
    }
  }

  // Merge the ordinary candidates with ADL before overload resolution ([over.call.func]).
  Ex with_adl(Ex callee, NameId name, std::span<const Ex> args, bool dependent_call);

  // The synthesized begin/end calls in [stmt.ranged] start with no ordinary candidates.
  Ex adl_call(NameId name, Ex arg, u32 begin, u32 end, u32 lookup_at) {
    ExprInfo info{builtin(Builtin::dependent), ValueCategory::lvalue, 0};
    Ex callee = node(ast::DepName{info, name}, begin, end, info);
    return call(callee, {arg}, end, end, lookup_at);
  }

  // A braced-init-list as a call's argument: a placeholder, list-initializing its parameter once
  // the callee is known (`call_args`).
  // The argument a parameter's default argument gives a call, positioned at the call's `)`: its
  // tree, analysed at the declaration ([dcl.fct.default]/5). A supported dependent default
  // is replayed only for the selected specialization and only when omitted ([temp.inst]/12).
  bool dependent_default_ = false;  // a call's default stayed dependent (`call`)
  std::optional<Ex> default_argument(NodeId d, u32 at_, DefaultArgumentOrigin origin = DefaultArgumentOrigin::interned,
                                     EntityId function = EntityId::none, QualType parameter = {},
                                     std::optional<u32> invocation = std::nullopt,
                                     std::optional<u32> descriptor_invocation = std::nullopt) {
    const bool in_builder = origin == DefaultArgumentOrigin::current_item;
    // An earlier declarator's inherited defaults are already adopted by this builder.
    const NodeId interned = in_builder ? b_->adopted_node(d) : d;
    const auto value = interned == NodeId::none ? b_->get<ast::DefaultArg>(d) : ins().ast.get<ast::DefaultArg>(interned);
    auto use = [&](NodeId id, NodeId global, ast::DefaultArg arg) {
      if (!unevaluated && ast::kind_of(arg.arg.id) == ast::Nodes::index_of<ast::DefaultSourceContext>()) {
        auto context = global == NodeId::none ? b_->get<ast::DefaultSourceContext>(arg.arg.id)
                                              : ins().ast.get<ast::DefaultSourceContext>(arg.arg.id);
        if (global != NodeId::none) context.arg.id = b_->adopt(context.arg.id);
        const u32 site = invocation.value_or(at_);
        if (context.observes_line) {
          context.line = source_line_at(site);
          if (default_source_observer_) default_source_observer_->line = true;
        }
        if (context.descriptor_type != EntityId::none) {
          if (default_source_observer_) {
            default_source_observer_->descriptor_type = context.descriptor_type;
          } else {
            if (q_->in_default_member_initializer()) {
              diag(site, Diag::source_location_default_member);
              return Ex{NodeId::none, at_, at_};
            }
            context.descriptor_address = source_location_address(context.descriptor_type, descriptor_invocation.value_or(site));
          }
        }
        arg.arg.id = b_->add(context, 0);
        return node(arg, at_, at_, arg.info);
      }
      return Ex{id == NodeId::none ? b_->adopt(global) : id, at_, at_, arg.info};
    };
    if (ast::kind_of(value.arg.id) == ast::Nodes::index_of<ast::Dependent>()) {
      if (function != EntityId::none && value.source_end > value.source_begin) {
        if (auto replay = q_->replay_function_default(*this, value, function, parameter, at_)) {
          if (*replay == NodeId::none) return std::nullopt;
          const NodeId committed = b_->adopted_node(*replay);
          const auto concrete = committed == NodeId::none ? b_->get<ast::DefaultArg>(*replay)
                                                           : ins().ast.get<ast::DefaultArg>(committed);
          return use(*replay, committed, concrete);
        }
      }
      // In a template's definition the default is instantiated with the call, when the
      // instantiation analyses the call again ([temp.inst]/12): the argument stays dependent.
      if (in_pattern()) {
        dependent_default_ = true;
        return dependent_node({}, at_, at_);
      }
      diag(at_, Diag::default_arg_dependent);
      return std::nullopt;
    }
    return use(in_builder ? d : NodeId::none, interned, value);
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

  std::optional<QualType> initializer_list_element(QualType t) {
    if (!types().is_class(t)) return std::nullopt;
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
    return types().canonical(specialization.args[0].type);
  }

  // [dcl.init.list]: the compiler forms an array of const elements and initializes the
  // list from that array. libycxx supplies a private pointer/size constructor for this.
  std::optional<Ex> initializer_list_argument(QualType t, const Braced& list);

  // A class reference can bind directly to a single reference-related expression.
  const Ex* class_reference_list_operand(QualType type, const Braced& list) {
    if (list.clauses.size() != 1) return nullptr;
    const Braced::Clause& clause = list.clauses.front();
    if (clause.list || clause.designated || !clause.expr.ok() || !types().is_class(clause.expr.type())) return nullptr;
    int paths = 0;
    return same_unqualified(clause.expr.type(), type) ||
           base_path(types().class_of(clause.expr.type()), types().class_of(type), paths) ? &clause.expr : nullptr;
  }

  // Otherwise the list initializes a prvalue of the referenced class ([dcl.init.list]); the
  // materialized object lives until the end of the call's full-expression ([class.temporary]).
  Ex class_reference_list_init(QualType parameter, const Braced& list, bool extend,
                               bool call_argument = false) {
    QualType t = types().referee(parameter);
    if (const auto* source = class_reference_list_operand(t, list))
      return bind_reference(*source, parameter, list.clauses.front().begin, extend);
    if (call_argument && !list.clauses.empty() && !types().rec(parameter).rvalue_ref && object_cv(t) == Cv::const_) {
      if (auto init = initializer_list_argument(t, list)) return bind_reference(*init, parameter, list.begin);
    }
    Ex temporary = list_init(Types::unqualified(t), list, Construction::copy_list);
    return bind_reference(temporary, parameter, list.begin, extend);
  }

  Ex braced_parameter(QualType parameter, const Braced& list) {
    if (types().is_reference(parameter) && types().is_class(types().referee(parameter)) &&
        !types().is_dependent(parameter) && !dependent(list))
      return class_reference_list_init(parameter, list, false, /*call_argument=*/true);
    return list_init(parameter, list, Construction::copy_list);
  }

  // The arguments of a call to a function of type `fr`, each copy-initializing its parameter.
  // A parameter without an argument takes its default argument ([dcl.fct.default], [expr.call]/5).
  std::optional<std::vector<Child>> call_args(const TypeRec& fr, std::span<const Ex> given, u32 begin, u32 paren_at,
                                              u32 invocation_at,
                                              std::span<const NodeId> defaults = {},
                                              DefaultArgumentOrigin origin = DefaultArgumentOrigin::interned,
                                              EntityId default_source = EntityId::none) {
    // Unevaluated calls still check parameter initialization. Only a decltype operand may
    // dispense with the result object. Subexpressions requiring materialization check it separately.
    const bool decltype_operand = decltype_unevaluated != 0 && unevaluated == decltype_unevaluated;
    if (unevaluated == 0) {
      if (!require_supported_virtual_signature(fr, paren_at, /*call=*/true)) return std::nullopt;
    } else if (!require_concrete_parameters(fr, paren_at) ||
               (!decltype_operand && !require_concrete_call_result(fr.inner, paren_at))) {
      return std::nullopt;
    }
    if (!decltype_operand && !check_destructor_access(fr.inner, paren_at)) return std::nullopt;
    std::vector<QualType> params = fr.params;  // copied: converting may intern new types
    std::vector<Ex> args(given.begin(), given.end());
    for (std::size_t i = args.size(); i < params.size() && i < defaults.size() && defaults[i] != NodeId::none; ++i) {
      // GCC uses the opening parenthesis for both builtins. Otherwise the descriptor uses the
      // call's beginning and __builtin_LINE uses a written member name when present.
      const bool gcc = q_->compatibility() == ast::Compatibility::gcc;
      auto a = default_argument(defaults[i], paren_at, origin, default_source, params[i],
                                gcc ? paren_at : invocation_at, gcc ? paren_at : begin);
      if (!a) return std::nullopt;
      args.push_back(*a);
    }
    if (args.size() < params.size()) {
      if (default_source != EntityId::none && q_->defaults_pending(default_source, args.size()))
        diag(paren_at, Diag::default_arg_not_yet_defined);
      else
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
      if (!a.ok()) return std::nullopt;
      children.push_back(at(a, begin));
    }
    return children;
  }

  // [expr.call]/12: an argument matched by the ellipsis gets the lvalue conversions, the integral
  // promotions and the floating-point promotion; std::nullptr_t becomes void*; void is no value.
  Ex variadic_argument(Ex e) {
    if (e.ok() && types().is_class(e.type()) && !require_concrete_object(e.type(), e.begin))
      return {NodeId::none, e.begin, e.end};
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
  bool in_class_assertion() const {
    return current_class != EntityId::none && current_class == class_prefix_owner_ && !static_initializer_name_;
  }

  // Search an incomplete class's earlier members, then its defining bases ([class.member.lookup]).
  std::optional<Ex> incomplete_class_member(NameId name, u32 begin, u32 end, u32 name_at, bool unqualified,
                                             bool callee, std::optional<u32> address_at = {});

  World* w_;
  ast::Builder* b_;
  Questions* q_;
  std::vector<engine::Diagnostic>* diags_;
  ast::ItemId item_;
  u32 loc_base_;
  DefaultSourceObservations* default_source_observer_ = nullptr;
  bool in_requires_body_ = false;
  bool in_constraint_atom_ = false;
  EntityId class_prefix_owner_ = EntityId::none;
  std::optional<NameId> static_initializer_name_;
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
  bool static_constexpr_initializer_ = false;
  bool replay_default_members_ = false;
  std::set<u32> constant_immediate_initializers_;
  std::set<u32> consteval_;                           // consteval functions named so far
  std::map<u32, std::vector<Ex>> list_elems_;         // by draft InitList
  std::map<u32, Ex> explicit_aggregate_prvalues_;     // by the start of the written type conversion
  struct DeferredAccess {
    EntityId cls, member;
    NameId name;
    u32 at;
    bool complete_destruction = false;
  };
  std::vector<DeferredAccess> deferred_access_;
  std::vector<std::function<void()>> deferred_injected_access_;
  struct Bound {
    std::optional<Ex> object;
    std::vector<const MemberDeclRec*> cands;
    EntityId cls = EntityId::none;  // whose members they are, for access ([class.access]/4)
    u32 name_at = 0;
    EntityId naming = EntityId::none;  // the class they were named in ([class.access.base]/5)
    bool qualified = false;            // an explicit qualifier suppresses virtual dispatch
    bool explicit_receiver = false;    // the member was named by a written `.` or `->`
    std::vector<MemberDeclRec> prefix_candidates{};  // ordinary bindings borrow from the completed class
  };
  std::map<u32, Bound> bound_;                        // by draft bound-member Member
  std::set<u32> deleted_;                             // deleted functions named so far
  std::map<u32, std::vector<NodeId>> defaults_;       // by a function's DeclRef: the default arguments its lookup found
  std::vector<std::pair<std::size_t, EntityId>> using_enums_;  // (scope depth, enumeration)
  struct BlockUsing {
    std::size_t depth;
    NameId name;
    std::vector<ast::DeclSema> decls;
    std::optional<std::size_t> type_index;  // representative type or type template in decls
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
  mutable bool outer_asked_ = false;  // `outer` asked the questions for the block around a local class
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
  // The function call operators, in a template's definition, whose noexcept-specifier depends on
  // the template's parameters.
  std::set<u32> dependent_noexcept_calls_;
  std::map<u32, GenericClosure> generic_closures_;  // by operator() entity: this part's generic lambdas
  std::map<u32, ConstId> const_captures_;  // by draft Member naming a captured constant: its value
};

}  // namespace ycxx::sema
