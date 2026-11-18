// The type kinds of docs/design/ast.md §5.1: one closed list, `TypeKinds`, of plain aggregates in
// the `types` interner, canonical kinds and sugar kinds. A sugar kind marks the type it spells with
// `[[=canonical]]`. Fingerprint, equality, walking and canonicalization are derived from these
// definitions by reflection (ycxx.ast.interner), as for tree nodes.
//
// This first list is the α slice's (#39): builtins, pointers, references, arrays, function types
// and typedef sugar, and class types (δ, #86). Enums, member pointers and the dependent kinds
// follow (#33).
export module ycxx.ast.types;

import std;
import ycxx.ast.handles;
import ycxx.ast.schema;

export namespace ycxx::ast {

// The fundamental types ([basic.fundamental]) and the markers of §5.1.
enum class Builtin : u8 {
  void_, nullptr_t,
  bool_,
  char_, signed_char, unsigned_char, wchar_t_, char8_t_, char16_t_, char32_t_,
  short_, unsigned_short, int_, unsigned_int, long_, unsigned_long, long_long, unsigned_long_long,
  float_, double_, long_double,
  float16, float32, float64, float128, bfloat16,  // [basic.extended.fp]
  meta_info,
  dependent, overload_set, bound_member,
};

constexpr u32 builtin_count = u32(Builtin::bound_member) + 1;

constexpr bool is_standard_signed(Builtin b) { return b == Builtin::signed_char || b == Builtin::short_ || b == Builtin::int_ || b == Builtin::long_ || b == Builtin::long_long; }
constexpr bool is_standard_unsigned(Builtin b) {
  return b == Builtin::unsigned_char || b == Builtin::unsigned_short || b == Builtin::unsigned_int ||
         b == Builtin::unsigned_long || b == Builtin::unsigned_long_long;
}
// [basic.fundamental]: `char`, `wchar_t`, `char8_t`, `char16_t` and `char32_t`, whose signedness and
// width come from their underlying type.
constexpr bool is_char_type(Builtin b) { return b >= Builtin::char_ && b <= Builtin::char32_t_ && b != Builtin::signed_char && b != Builtin::unsigned_char; }
// Integral types ([basic.fundamental]): bool, the character types, the signed and unsigned integer types.
constexpr bool is_integral(Builtin b) { return b >= Builtin::bool_ && b <= Builtin::unsigned_long_long; }
constexpr bool is_floating(Builtin b) { return b >= Builtin::float_ && b <= Builtin::bfloat16; }
constexpr bool is_standard_floating(Builtin b) { return b >= Builtin::float_ && b <= Builtin::long_double; }
constexpr bool is_arithmetic(Builtin b) { return is_integral(b) || is_floating(b); }

constexpr QualType without_cv(QualType q) { return QualType(std::to_underlying(q) & ~7u); }

enum class RefQual : u8 { none, lvalue, rvalue };
// Language linkage is part of a function type ([dcl.link]).
enum class LangLinkage : u8 { cxx, c };

// Marks the member of a sugar kind that holds the type it spells (§5.1).
struct canonical_t {
  constexpr bool operator==(const canonical_t&) const = default;
};
inline constexpr canonical_t canonical{};

// ---------------------------------------------------------------- canonical kinds

struct TBuiltin {
  Builtin b;
};

struct TPointer {
  QualType pointee;
};

// References are built through ycxx.ast.typeops, which collapses them ([dcl.ref]).
struct TLRef {
  QualType referee;
};
struct TRRef {
  QualType referee;
};

// `bound` is an integer constant, or `ConstId::none` for an unknown bound ([dcl.array]).
struct TArray {
  QualType elem;
  ConstId bound;
};

// A function type ([dcl.fct]). `params` hold the adjusted parameter types (§5.4); `noexcept_` is
// a bool constant (or, later, a `VDep*` operand). `cv` and `ref` are the member function's
// qualifiers.
struct TFunction {
  QualType ret;
  SeqId<QualType> params;
  ConstId noexcept_;
  u8 variadic;
  Cv cv;
  RefQual ref;
  LangLinkage linkage;
};

// A class type ([class]): the class's entity (§4.3). Its members and layout are the class's
// `class_def` and `layout` (§3.6), not part of the type, so the type of an incomplete class and of
// its definition are one type ([basic.types.general]).
struct TClass {
  EntityId entity;
};

// ---------------------------------------------------------------- sugar kinds

// A use of a typedef-name or alias-declaration. The alias's template arguments come with alias
// templates.
struct STypedef {
  [[=canonical]] QualType type;
  EntityId alias;
};

using TypeKinds = closed_list<TBuiltin, TPointer, TLRef, TRRef, TArray, TFunction, TClass, STypedef>;

template <class T>
constexpr bool is_type_kind = TypeKinds::contains<T>;

// The member of a sugar kind marked `[[=canonical]]`, or null for a canonical kind.
consteval std::meta::info canonical_member(std::meta::info kind) {
  std::meta::info r{};
  for (auto m : std::meta::nonstatic_data_members_of(kind, std::meta::access_context::unchecked())) {
    if (std::meta::annotations_of_with_type(m, ^^canonical_t).empty()) continue;
    if (r != std::meta::info{}) throw std::meta::exception(u8"a sugar kind marks one member [[=canonical]]", m);
    if (std::meta::dealias(std::meta::type_of(m)) != ^^QualType)
      throw std::meta::exception(u8"the [[=canonical]] member is a QualType", m);
    r = m;
  }
  return r;
}

template <class T>
constexpr bool is_sugar_kind = canonical_member(^^T) != std::meta::info{};

// A type kind is a record (ast.md §2) of at most 32 bytes (§9.6).
static_assert(std::ranges::all_of(TypeKinds::infos, [](std::meta::info k) {
  if (std::meta::size_of(k) > 32) throw std::meta::exception(u8"a type payload is at most 32 bytes", k);
  canonical_member(k);
  return check_record(k);
}));

}  // namespace ycxx::ast
