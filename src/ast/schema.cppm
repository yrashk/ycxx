// What reflection derives from a node kind or record (docs/design/ast.md §2, §3.7, §9, §10):
// its members, its schema seed, whether it is an expression, the sequencing of its operands, and
// the checks that keep the invariants of §2 (no pointer, no `Loc`, only listed handles, the
// payload budget). Node kinds and records are plain aggregates; everything here reads them.
export module ycxx.ast.schema;

import std;
import ycxx.ast.fingerprint;
import ycxx.ast.handles;

export namespace ycxx::ast {

template <class T>
consteval auto members_of() {
  return std::define_static_array(
      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
}

// ---------------------------------------------------------------- expressions

enum class ValueCategory : u8 { prvalue, lvalue, xvalue };

// What every expression node carries (§3.3): its sugared type and value category. A node kind is
// an expression kind exactly when it has a member of this type named `info`.
struct ExprInfo {
  QualType type;
  ValueCategory category;
  u8 flags;  // manifestly constant-evaluated and the like (§3.7.7); defined with the parser
  constexpr bool operator==(const ExprInfo&) const = default;
};

consteval bool is_expression(std::meta::info kind) {
  for (auto m : std::meta::nonstatic_data_members_of(kind, std::meta::access_context::unchecked()))
    if (std::meta::has_identifier(m) && std::meta::identifier_of(m) == "info" &&
        std::meta::dealias(std::meta::type_of(m)) == ^^ExprInfo)
      return true;
  return false;
}

// ---------------------------------------------------------------- sequencing (§3.7.4, [intro.execution])
//
// Each operand of an expression kind (a `Child` or `ChildList` member) carries one annotation:
// `[[=sequenced(r)]]`, `[[=indeterminately_sequenced(r)]]` or `[[=unsequenced(r)]]`, where the rank
// `r` (default 0) orders groups: every operand of a lower rank is sequenced before every operand
// of a higher one. Within one rank all operands carry the same annotation, which says how they
// relate to each other: in declaration order, indeterminately, or unsequenced. A `ChildList`'s
// elements relate to each other the same way. The rule belongs to the kind, so shared nodes need
// no per-occurrence data.

enum class Order : u8 { sequenced, indeterminately_sequenced, unsequenced };

struct sequencing {
  Order order;
  u8 rank = 0;
  consteval sequencing operator()(u8 r) const { return {order, r}; }
  constexpr bool operator==(const sequencing&) const = default;
};

inline constexpr sequencing sequenced{Order::sequenced};
inline constexpr sequencing indeterminately_sequenced{Order::indeterminately_sequenced};
inline constexpr sequencing unsequenced{Order::unsequenced};

// How the evaluation of operand `a` relates to that of operand `b`.
enum class Relation : u8 { before, after, indeterminately_sequenced, unsequenced };

consteval bool is_child_type(std::meta::info type) {
  type = std::meta::dealias(type);
  return type == ^^Child || type == ^^ChildList;
}

consteval std::optional<sequencing> sequencing_of(std::meta::info member) {
  auto a = std::meta::annotations_of_with_type(member, ^^sequencing);
  if (a.empty()) return std::nullopt;
  if (a.size() > 1) throw std::meta::exception(u8"an operand has one sequencing annotation", member);
  return std::meta::extract<sequencing>(a[0]);
}

// An operand of an expression kind: its position among the kind's members, and its sequencing.
struct Operand {
  u32 member;
  sequencing seq;
  bool list;
};

template <class T>
consteval std::vector<Operand> operand_list() {
  std::vector<Operand> r;
  u32 i = 0;
  for (auto m : std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())) {
    if (is_child_type(std::meta::type_of(m)))
      r.push_back({i, *sequencing_of(m), std::meta::dealias(std::meta::type_of(m)) == ^^ChildList});
    ++i;
  }
  return r;
}

// The operands of an expression kind, in declaration order; usable at run time.
template <class T>
inline constexpr auto operands = std::define_static_array(operand_list<T>());

// The relation between operands `a` and `b` (indices into `operands<T>`). For `a == b` it is the
// relation between two elements of a `ChildList` operand, the earlier one first.
template <class T>
consteval Relation relation(u32 a, u32 b) {
  auto ops = operand_list<T>();
  sequencing x = ops.at(a).seq;
  sequencing y = ops.at(b).seq;
  if (x.rank != y.rank) return x.rank < y.rank ? Relation::before : Relation::after;
  switch (x.order) {
    case Order::sequenced: return a <= b ? Relation::before : Relation::after;
    case Order::indeterminately_sequenced: return Relation::indeterminately_sequenced;
    case Order::unsequenced: return Relation::unsequenced;
  }
  std::unreachable();
}

// ---------------------------------------------------------------- what a value may hold (§2)

// The types a node or record may hold: integers and plain enums, the listed handles, `SeqId`s,
// child edges (`nodes` only) and aggregates of these. Never a pointer, a reference, a floating
// value, a `Loc`, or an enum that names an interner without being in `handles`.
consteval void check_value_type(std::meta::info type, std::meta::info where, bool node) {
  type = std::meta::dealias(type);
  auto fail = [&](const char8_t* why) { throw std::meta::exception(why, where); };
  if (std::meta::is_pointer_type(type) || std::meta::is_reference_type(type) ||
      std::meta::is_member_pointer_type(type))
    fail(u8"a pointer or reference in an interned value (ast.md §2, invariant 1)");
  if (std::meta::is_floating_point_type(type)) fail(u8"a floating value outside `values`");
  if (type == ^^Loc) fail(u8"a Loc in a node or record (ast.md §2)");
  if (type == ^^bool || std::meta::is_integral_type(type)) return;
  if (std::meta::is_enum_type(type)) {
    bool listed = false;
    for (auto h : std::meta::template_arguments_of(std::meta::dealias(^^handles)))
      listed = listed || type == h;
    if (names_an_interner(type) && !listed) fail(u8"a handle missing from `handles`");
    return;
  }
  if (std::meta::has_template_arguments(type) && std::meta::template_of(type) == ^^SeqId) {
    check_value_type(std::meta::template_arguments_of(type)[0], where, false);  // elements are records
    return;
  }
  if (type == ^^Child || type == ^^ChildList) {
    if (!node) fail(u8"a child edge outside a node (records hold a NodeId)");
    return;
  }
  if (std::meta::is_class_type(type) && std::meta::is_aggregate_type(type)) {
    if (!std::meta::bases_of(type, std::meta::access_context::unchecked()).empty())
      fail(u8"a base class: reflection walks only direct members");
    for (auto m : std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked()))
      check_value_type(std::meta::type_of(m), m, node);
    return;
  }
  fail(u8"a type a node or record may not hold");
}

// A node kind: an aggregate of permitted members, trivially copyable, at most 32 bytes (§9.6);
// an expression kind's operands are all annotated, structure kinds' none are, and within a rank
// the annotations agree.
consteval bool check_node_kind(std::meta::info kind) {
  if (!std::meta::is_aggregate_type(kind) || !std::meta::is_trivially_copyable_type(kind))
    throw std::meta::exception(u8"a node kind is a trivially copyable aggregate", kind);
  if (!std::meta::bases_of(kind, std::meta::access_context::unchecked()).empty())
    throw std::meta::exception(u8"a node kind has no base class: reflection walks only direct members", kind);
  if (std::meta::size_of(kind) > 32)
    throw std::meta::exception(u8"a node payload is at most 32 bytes (ast.md §9.6)", kind);
  bool expr = is_expression(kind);
  std::vector<sequencing> seen;
  for (auto m : std::meta::nonstatic_data_members_of(kind, std::meta::access_context::unchecked())) {
    check_value_type(std::meta::type_of(m), m, true);
    auto s = sequencing_of(m);
    if (s && !is_child_type(std::meta::type_of(m)))
      throw std::meta::exception(u8"a sequencing annotation on a member that is not an operand", m);
    if (!is_child_type(std::meta::type_of(m))) continue;
    if (expr != s.has_value())
      throw std::meta::exception(expr ? u8"an expression operand without a sequencing annotation"
                                      : u8"a sequencing annotation on a structure kind", m);
    if (!s) continue;
    for (auto o : seen)
      if (o.rank == s->rank && o.order != s->order)
        throw std::meta::exception(u8"operands of one rank disagree on their sequencing", m);
    seen.push_back(*s);
  }
  return true;
}

consteval bool check_record(std::meta::info type) {
  if (!std::meta::is_aggregate_type(type) || !std::meta::is_trivially_copyable_type(type))
    throw std::meta::exception(u8"a record is a trivially copyable aggregate", type);
  if (!std::meta::bases_of(type, std::meta::access_context::unchecked()).empty())
    throw std::meta::exception(u8"a record has no base class: reflection walks only direct members", type);
  for (auto m : std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked()))
    check_value_type(std::meta::type_of(m), m, false);
  return true;
}

// Whether values of `type` refer to tree nodes, directly or through a `SeqId`. Such lists are
// built with their tree and interned when the frame commits; all others are interned eagerly
// (§3.6, §9.3).
consteval bool holds_nodes(std::meta::info type) {
  type = std::meta::dealias(type);
  if (type == ^^NodeId || type == ^^Child || type == ^^ChildList) return true;
  if (std::meta::has_template_arguments(type) && std::meta::template_of(type) == ^^SeqId)
    return holds_nodes(std::meta::template_arguments_of(type)[0]);
  if (std::meta::is_class_type(type))
    for (auto m : std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked()))
      if (holds_nodes(std::meta::type_of(m))) return true;
  return false;
}

template <class T>
constexpr bool holds_nodes_v = holds_nodes(^^T);

// ---------------------------------------------------------------- schema seeds

// A fingerprint seed per type: its name and, in order, its members' names and types. A field
// added, renamed or retyped changes every fingerprint of the type.
consteval Fp128 schema_seed(std::meta::info type) {
  type = std::meta::dealias(type);
  Fp128 h = seed(std::meta::display_string_of(type));
  if (std::meta::is_class_type(type) && !(std::meta::has_template_arguments(type) &&
                                          std::meta::template_of(type) == ^^SeqId))
    for (auto m : std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked())) {
      h = mix_bytes(h, std::meta::identifier_of(m));
      h = mix_bytes(h, std::meta::display_string_of(std::meta::type_of(m)));
    }
  return h;
}

template <class T>
inline constexpr Fp128 schema_seed_v = schema_seed(^^T);

}  // namespace ycxx::ast
