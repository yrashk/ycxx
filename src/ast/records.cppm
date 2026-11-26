// The record shapes of docs/design/ast.md §3.6: what `item_sema`, `decl_sema`, `class_def` and
// `analyze_body` return. They are structural aggregates; lists of them are `SeqId`s in the `seqs`
// interner. Lists whose records hold `NodeId`s are interned with their tree when the frame
// commits (§9.3); the rest are interned eagerly.
//
// The enumerations and the small records (`ParamSema`, `BaseSpec`, `MemberRec`, `LocalRec`) are
// the core's first cut; sema (M3) fills in their enumerators and flags.
export module ycxx.ast.records;

import std;
import ycxx.hash;
import ycxx.ast.handles;
import ycxx.ast.schema;
import ycxx.ast.nodes;

export namespace ycxx::ast {

// `data_member` is a non-static data member, which is not a variable ([basic.pre]).
enum class DeclKind : u8 { namespace_, class_, enum_, enumerator, variable, function, parameter, type_alias, concept_, template_, data_member };
enum class Linkage : u8 { none, internal, module, external };
enum class Storage : u8 { none, static_, thread_local_, extern_, automatic };
enum class Access : u8 { none, public_, protected_, private_ };
enum class GnuVisibility : u8 { none, default_, hidden };

// A template argument (§5.6): a type, or a value converted to its parameter's type
// ([temp.arg.nontype]); `type` is canonical, and a value's is the value's type. A pack parameter's
// argument is a `pack` of its elements, types or values, never packs; while dependent an element
// may be an expansion (`TPackExpansion`, `VDepExpansion`) ([temp.variadic]). A `template_`
// argument (*amended by #88 step 8, Fable's decision*) is a class or alias template, `templ`;
// while dependent it is a template template parameter's `TTemplateParam` or a `TDepName` in
// `type` ([temp.arg.template]).
enum class TemplateArgKind : u8 { type, value, pack, template_ };
struct TemplateArg {
  TemplateArgKind kind;
  QualType type;
  ConstId value;              // none for a type
  SeqId<TemplateArg> elems{};  // a pack's elements; the empty list (index 0) otherwise
  EntityId templ = EntityId::none;  // a template_ argument's template; none while dependent
  constexpr bool operator==(const TemplateArg&) const = default;
};

// A template-parameter ([temp.param]) of a template-head: its name, its kind and position
// (`TTemplateParam` and `VDepParam` refer to it by depth and index), a non-type parameter's type,
// and its default argument if it has one (`has_default`).
struct TemplateParamSema {
  NameId name;
  TemplateArgKind kind;
  u8 pack;
  u8 has_default;
  u16 depth;
  u16 index;
  QualType type;     // a type or template template parameter's own `TTemplateParam`; a non-type
                     // parameter's type
  TemplateArg default_;
  // A template template parameter's own template-head ([temp.param]/1), its parameters at
  // `depth + 1`; empty otherwise.
  SeqId<TemplateParamSema> params{};
  u8 rank_type_default = 0;  // substituting the written type default can check a dependent rank
  ConstId type_constraint = ConstId::none;  // concept and its explicit arguments, for linkage
  constexpr bool operator==(const TemplateParamSema&) const = default;
};

// The written expression is separate from the normal form used for constraint checking.
struct WrittenConstraint {
  ConstId expression = ConstId::none;
  Fp128 tokens{};
  constexpr bool operator==(const WrittenConstraint&) const = default;
};

struct ParamSema {
  EntityId entity;
  QualType type;
  NameId name;
  NodeId default_arg;  // non-member functions only; members use default_arg(p, ctx) (§3.6)
  u16 flags;
  constexpr bool operator==(const ParamSema&) const = default;
};

// [dcl.attr.annotation]: the type deduced by reflect_constant's by-value parameter, the
// evaluated constant, and the item-relative position of the annotation's '='. Enum constants
// use their underlying representation in the value interner, paired here with the enum type.
struct AnnotationSema {
  QualType type;
  ConstId value;
  u32 offset;
  constexpr bool operator==(const AnnotationSema&) const = default;
};

// An annotation as returned to consumers, with the owning declaration's item.
struct DeclarationAnnotation {
  QualType type;
  ConstId value;
  ItemId item;
  u32 offset;
};

struct DeclSema {
  EntityId entity;
  QualType type;
  DeclKind kind;
  Linkage linkage;
  Storage storage;
  Access access;
  u16 flags;  // includes explicit_object (§5.4)
  SeqId<NodeId> attributes;  // Attribute nodes
  NodeId constraint;
  // A template's associated constraints in normal form (a `VConstraint` or `VAtom`), or a concept's
  // own constraint-expression's; none if it has none ([temp.constr.decl]).
  ConstId constraints;
  NodeId contracts;  // non-members only, this declaration's own (§7.5)
  SeqId<ParamSema> params;
  // A template's template-head ([temp.pre]); empty for a declaration that is not a template.
  SeqId<TemplateParamSema> template_params;
  u8 bit_width = 0;  // a non-static data member declared as a bit-field ([class.bit])
  bool is_virtual = false;  // a virtual member function ([class.virtual])
  bool is_explicit = false;  // a constructor with a bare explicit specifier ([dcl.fct.spec])
  bool is_mutable = false;  // a mutable non-static data member ([dcl.stc])
  // A named function declarator spelled an exception specifier, even if its type does not retain it.
  bool has_written_noexcept = false;
  bool has_trailing_return = false;
  bool hidden_visibility = false;  // an explicit GNU visibility("hidden") attribute on this declaration
  bool explicit_weak = false;  // an explicit GNU weak attribute on this declaration
  bool is_pure = false;  // a written pure-specifier ([class.abstract])
  SeqId<AnnotationSema> annotations{};  // the occurrences written on this declaration
  bool is_final = false;  // written on a class head or virtual member ([class.pre], [class.virtual])
  bool explicit_default_visibility = false;
  GnuVisibility namespace_visibility = GnuVisibility::none;  // lexical namespace-definition occurrence
  u8 field_alignment = 0;  // a validated alignment stricter than the field type's natural alignment
  WrittenConstraint head_constraint{};
  WrittenConstraint trailing_constraint{};
  constexpr bool operator==(const DeclSema&) const = default;
};

// A pack a pattern names ([temp.variadic]): a template parameter pack by depth and index, or a
// function parameter pack by name (`fn`).
struct PackRefRec {
  u16 depth;
  u16 index;
  NameId fn;
  constexpr bool operator==(const PackRefRec&) const = default;
};
// A pack expansion of a templated definition, by the part offsets of its pattern (`begin`, and
// just past it, `end`), with the packs it names: its instantiations read the pattern once per
// element (§6.4).
struct ExpansionRec {
  u32 begin;
  u32 end;
  SeqId<PackRefRec> packs;
  constexpr bool operator==(const ExpansionRec&) const = default;
};

struct BaseSpec {
  QualType type;
  Access access;
  u8 is_virtual;
  u8 is_pack_expansion;
  constexpr bool operator==(const BaseSpec&) const = default;
};

struct MemberRec {
  DeclId decl;
  EntityId entity;
  Access access;
  constexpr bool operator==(const MemberRec&) const = default;
};

struct ClassDef {
  SeqId<BaseSpec> bases;
  SeqId<MemberRec> members;
  SeqId<MemberRec> friends;
  SeqId<DeclSema> synthesized;  // class-1 declarations (§4.1)
  SeqId<EntityId> nested;
  EntityId injected_class_name;
  u16 flags;
  constexpr bool operator==(const ClassDef&) const = default;
};

namespace local_flag {
inline constexpr u16 requires_parameter = 2;  // notation without storage ([expr.prim.req.general])
inline constexpr u16 structured_binding = 4;  // a local reference slot for a structured binding ([dcl.struct.bind])
inline constexpr u16 binding_reference_member = 32;  // the slot type is also the binding's referenced type
}

// A block-scope declaration: plain data of the body tree (§4.4). `entity` is set for those that
// can be named from outside the body (local classes, lambdas' closure types, block-scope statics).
struct LocalRec {
  NameId name;
  QualType type;
  EntityId entity;
  NodeId decl;  // the LocalVar or other declaring node
  u16 flags;
  constexpr bool operator==(const LocalRec&) const = default;
};

// A lambda-expression's closure type and its members' definitions, made by the analysis of the
// part it is in (docs/design/closures.md): the closure class, its function call operator (its
// type with the return type deduced, its body and the body's own locals, parameters first), and
// for a captureless lambda the static invoker and the conversion function to a pointer to
// function ([expr.prim.lambda.closure]/8–9). `begin`, `body_begin` and `end` are the lambda's
// token offsets in the part: its `[`, its body's `{`, and just past its `}`.
struct ClosureDef {
  EntityId closure;
  EntityId call;
  QualType call_type;
  NodeId body;
  SeqId<LocalRec> locals;
  EntityId invoker;
  EntityId conversion;
  NodeId invoker_body;
  NodeId conversion_body;
  u32 begin;
  u32 body_begin;
  u32 end;
  u16 flags;  // closure_flag
  // A generic lambda's function call operator is a template ([expr.prim.lambda.closure]/7): its
  // template-head; its specializations re-analyse the body from `body_begin`, where the enclosing
  // function's constants it names without capturing them are `consts` with `const_values`.
  SeqId<TemplateParamSema> template_params;
  SeqId<LocalRec> consts;
  SeqId<ConstId> const_values;
  SeqId<NameId> captures;  // the names its body knows its closure's fields by
  constexpr bool operator==(const ClosureDef&) const = default;
};

// A declaration part keeps its pack expansions (§6.4) and completed lambdas.
struct ItemSema {
  NodeId tree;
  SeqId<DeclSema> decls;
  SeqId<ExpansionRec> expansions;
  SeqId<ClosureDef> closures{};
  NameId asm_hidden_symbol{~u32{0}};
  u32 asm_directive_at = 0;
  constexpr bool operator==(const ItemSema&) const = default;
};

// A templated body keeps what its instantiations need of it (§6.4): its pack expansions, and the
// offsets of its calls that were dependent ([temp.dep.candidate]).
struct BodySema {
  NodeId tree;
  SeqId<LocalRec> locals;
  SeqId<ExpansionRec> expansions;
  SeqId<u32> dependent_calls;
  // A function whose declared return type holds a placeholder ([dcl.spec.auto]): its function type
  // with the return type its body deduced, when `deduced` is set.
  QualType deduced_type{};
  u8 deduced = 0;
  // Its lambdas' closures, in the order their bodies end (docs/design/closures.md).
  SeqId<ClosureDef> closures{};
  constexpr bool operator==(const BodySema&) const = default;
};

// The element types of the `seqs` interner: one pool each, indexed by position in this list.
using SeqElements = closed_list<NodeId, EntityId, QualType, NameId, ParamSema, DeclSema, BaseSpec,
                                MemberRec, LocalRec, u8, u32, ConstId, TemplateArg, TemplateParamSema, PackRefRec,
                                ExpansionRec, ClosureDef, AnnotationSema, VirtualMemberSelection>;

static_assert(std::ranges::all_of(SeqElements::infos, [](std::meta::info t) {
  if (std::meta::is_class_type(t)) return check_record(t);
  check_value_type(t, t, false);  // an enum: plain, or a listed handle
  return true;
}));
static_assert(check_record(^^ItemSema) && check_record(^^ClassDef) && check_record(^^BodySema));

}  // namespace ycxx::ast
