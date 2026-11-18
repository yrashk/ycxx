// The record shapes of docs/design/ast.md §3.6: what `item_sema`, `decl_sema`, `class_def` and
// `analyze_body` return. They are structural aggregates; lists of them are `SeqId`s in the `seqs`
// interner. Lists whose records hold `NodeId`s are interned with their tree when the frame
// commits (§9.3); the rest are interned eagerly.
//
// The enumerations and the small records (`ParamSema`, `BaseSpec`, `MemberRec`, `LocalRec`) are
// the core's first cut; sema (M3) fills in their enumerators and flags.
export module ycxx.ast.records;

import std;
import ycxx.ast.handles;
import ycxx.ast.schema;
import ycxx.ast.nodes;

export namespace ycxx::ast {

enum class DeclKind : u8 { namespace_, class_, enum_, enumerator, variable, function, parameter, type_alias, concept_, template_ };
enum class Linkage : u8 { none, internal, module, external };
enum class Storage : u8 { none, static_, thread_local_, extern_, automatic };
enum class Access : u8 { none, public_, protected_, private_ };

struct ParamSema {
  EntityId entity;
  QualType type;
  NameId name;
  NodeId default_arg;  // non-member functions only; members use default_arg(p, ctx) (§3.6)
  u16 flags;
  constexpr bool operator==(const ParamSema&) const = default;
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
  NodeId contracts;  // non-members only, this declaration's own (§7.5)
  SeqId<ParamSema> params;
  constexpr bool operator==(const DeclSema&) const = default;
};

struct ItemSema {
  NodeId tree;
  SeqId<DeclSema> decls;
  constexpr bool operator==(const ItemSema&) const = default;
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

struct BodySema {
  NodeId tree;
  SeqId<LocalRec> locals;
  constexpr bool operator==(const BodySema&) const = default;
};

// The element types of the `seqs` interner: one pool each, indexed by position in this list.
using SeqElements = closed_list<NodeId, EntityId, QualType, NameId, Cleanup, ParamSema, DeclSema, BaseSpec,
                                MemberRec, LocalRec, u8, u32>;

static_assert(std::ranges::all_of(SeqElements::infos, [](std::meta::info t) {
  if (std::meta::is_class_type(t)) return check_record(t);
  check_value_type(t, t, false);  // an enum: plain, or a listed handle
  return true;
}));
static_assert(check_record(^^ItemSema) && check_record(^^ClassDef) && check_record(^^BodySema));

}  // namespace ycxx::ast
