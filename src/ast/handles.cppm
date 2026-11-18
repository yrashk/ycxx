// The handle vocabulary of docs/design/ast.md §2. Every reference between front-end values is a
// typed 32-bit handle into a process-wide interner, or a small structural id (`DeclId`, `Loc`).
// The handle types form one closed list, `handles`, which is what fingerprinting, walking and
// compaction iterate over. Each handle enum names its interner with an annotation (§12's probe:
// annotations on enumeration types work in GCC 16.2).
export module ycxx.ast.handles;

import std;

export namespace ycxx::ast {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// A closed list of types: node kinds, sequence element types, handle types.
template <class... Ts>
struct closed_list {
  static constexpr std::array<std::meta::info, sizeof...(Ts)> infos{^^Ts...};
  static constexpr u32 size = sizeof...(Ts);
  template <u32 I>
  using at = Ts...[I];
  template <class T>
  static constexpr bool contains = (std::is_same_v<T, Ts> || ...);
  template <class T>
    requires contains<T>
  static consteval u32 index_of() {
    u32 i = 0;
    u32 r = 0;
    ((std::is_same_v<T, Ts> ? (r = i, ++i) : ++i), ...);
    return r;
  }
};

// The interners of §9.1. An enumerator names the member of `Interners` (ycxx.ast.interner) that
// holds the handle's values; the enumerator and the member share an identifier.
enum class interner : u8 { names, tokens, items, entities, ast, types, values, seqs };

enum class [[=interner::names]] NameId : u32 {};
enum class [[=interner::tokens]] TokSeqId : u32 {};
enum class [[=interner::tokens]] BodyId : u32 {};
enum class [[=interner::items]] ItemId : u32 {};
enum class [[=interner::entities]] EntityId : u32 { none = 0xffffffff };
enum class [[=interner::values]] ConstId : u32 { none = 0xffffffff };

// A tree node: the kind in the top 8 bits, the index in its kind's pool in the low 24 (§2, §9.4).
// Kind 255 is reserved for `none`.
enum class [[=interner::ast]] NodeId : u32 { none = 0xffffffff };

constexpr u32 node_kind_bits = 8;
constexpr u32 node_index_bits = 24;
constexpr u32 max_node_kinds = (1u << node_kind_bits) - 1;
constexpr u32 max_node_index = (1u << node_index_bits) - 1;

constexpr u32 kind_of(NodeId n) { return std::to_underlying(n) >> node_index_bits; }
constexpr u32 index_of(NodeId n) { return std::to_underlying(n) & max_node_index; }
// BOOTSTRAP: contracts: the checked constructors below are not constexpr. GCC 16.2 does not emit
// its TU-local contract-violation helper in an importer that inlines a non-template inline
// function with a precondition, so the importer fails to link unless it has a contract of its own.
NodeId make_node(u32 kind, u32 index)
  pre(kind < max_node_kinds)
  pre(index <= max_node_index)
{
  return NodeId((kind << node_index_bits) | index);
}

// A type node in the top 29 bits and its top-level cv in the low 3 (§2, §5). The type kinds and
// their canonical forms are #33's; this is only the handle.
enum class [[=interner::types]] QualType : u32 {};

enum class Cv : u8 { none = 0, const_ = 1, volatile_ = 2, const_volatile = 3 };

constexpr u32 type_node_of(QualType q) { return std::to_underlying(q) >> 3; }
constexpr Cv cv_of(QualType q) { return Cv(std::to_underlying(q) & 7); }
// BOOTSTRAP: contracts: not constexpr, as make_node.
QualType make_qual_type(u32 node, Cv cv = Cv::none)
  pre(node < (1u << 29))
{
  return QualType((node << 3) | std::to_underlying(cv));
}
constexpr QualType with_cv(QualType q, Cv cv) {
  return QualType(std::to_underlying(q) | std::to_underlying(cv));
}

// BOOTSTRAP: defaulted comparison: the structs of src/ast declare `operator==` as a defaulted
// member, not a hidden friend; GCC 16.2 with -freflection crashes in an importer that compares
// two values of an exported struct whose defaulted `operator==` is a friend.

// An interned ordered list of `T`s (§2): `DeclListId`, `TypeListId` and the record lists of §3.6.
// One interner, `seqs`, holds a pool per element type. A struct, since an enum cannot be a template.
template <class T>
struct SeqId {
  u32 index;
  constexpr bool operator==(const SeqId&) const = default;
};

template <class T>
constexpr bool is_seq_id = std::meta::has_template_arguments(^^T) && std::meta::template_of(^^T) == ^^SeqId;

// The closed list of interned handle types. `SeqId<T>` is a family, recognized by `is_seq_id`;
// `DeclId` and `Loc` are structural and walked as aggregates.
using handles = closed_list<NameId, TokSeqId, BodyId, ItemId, EntityId, ConstId, NodeId, QualType>;

template <class T>
constexpr bool is_handle = handles::contains<T> || is_seq_id<T>;

// The interner a handle enum names in its annotation.
consteval interner interner_of(std::meta::info handle) {
  auto a = std::meta::annotations_of_with_type(handle, ^^interner);
  if (a.size() != 1) throw std::meta::exception(u8"a handle names exactly one interner", handle);
  return std::meta::extract<interner>(a[0]);
}

consteval bool names_an_interner(std::meta::info type) {
  return std::meta::is_enum_type(type) && !std::meta::annotations_of_with_type(type, ^^interner).empty();
}

// §4.1: a 4-bit class and a 28-bit number.
enum class DeclClass : u8 { syntactic = 0, synthesized = 1, injected = 2 };
enum class DeclOrd : u32 {};

// BOOTSTRAP: contracts: not constexpr, as make_node.
DeclOrd make_decl_ord(DeclClass c, u32 number)
  pre(number < (1u << 28))
{
  return DeclOrd((u32(std::to_underlying(c)) << 28) | number);
}
constexpr DeclClass class_of(DeclOrd o) { return DeclClass(std::to_underlying(o) >> 28); }
constexpr u32 number_of(DeclOrd o) { return std::to_underlying(o) & ((1u << 28) - 1); }

// One declared name of an item (§4.1). Structural, not interned.
struct DeclId {
  ItemId item;
  DeclOrd ord;
  constexpr bool operator==(const DeclId&) const = default;
};

// A source position: an item and a token offset in it (§8). Diagnostics, demand lists and debug
// side tables only; never in a node, a record or a key. The location machinery is #34's.
struct Loc {
  ItemId item;
  u32 offset;
  constexpr bool operator==(const Loc&) const = default;
};

// A child edge: the child and its token offset relative to the parent's start (§2, invariant 2).
// The child holds no position, so equal subtrees are one node wherever they occur.
struct Child {
  NodeId id = NodeId::none;
  u32 at = 0;
  constexpr bool operator==(const Child&) const = default;
};

// A range of the owning interner's (or frame builder's) edge pool.
struct ChildList {
  u32 offset = 0;
  u32 size = 0;
  constexpr bool operator==(const ChildList&) const = default;
};

}  // namespace ycxx::ast
