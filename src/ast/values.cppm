// The constant kinds of docs/design/ast.md §5.5: one closed list, `ValueKinds`, in the `values`
// interner. A value's type is canonical and cv-unqualified, so template-argument-equivalent values
// are one `ConstId` (§5.6).
//
// This first list is the α slice's (#39): integers (bool and the character types included),
// `nullptr`, string literal objects and addresses of them and of entities, and the shape of the
// dependent family: a kind marked `[[=dependent]]` is a `VDep*` value, one with free template
// parameters; aggregates of class and array type (δ, #86); floating values (μ, #336). The rest of
// the `VDep*` family follows (#33).
export module ycxx.ast.values;

import std;
import ycxx.ast.handles;
import ycxx.ast.records;
import ycxx.ast.schema;

export namespace ycxx::ast {

// Marks a `VDep*` kind (§5.5).
struct dependent_t {
  constexpr bool operator==(const dependent_t&) const = default;
};
inline constexpr dependent_t dependent{};

// An integer as two's complement in 128 bits, normalized to its type: truncated to the type's
// width, then sign- or zero-extended by the type's signedness. A bool is 0 or 1.
struct Bits128 {
  u64 lo = 0;
  u64 hi = 0;
  constexpr bool operator==(const Bits128&) const = default;
};

// The value of `v`'s low `width` bits, sign- or zero-extended to 128: the unique value of a
// `width`-bit type congruent to `v` modulo 2^width ([conv.integral]).
// BOOTSTRAP: contracts: not constexpr, as make_node (ycxx.ast.handles).
Bits128 wrap(Bits128 v, u32 width, bool is_signed)
  pre(width >= 1 && width <= 128)
{
  if (width == 128) return v;
  if (width > 64) {
    u64 mask = (u64{1} << (width - 64)) - 1;
    u64 hi = v.hi & mask;
    bool neg = is_signed && (hi >> (width - 65)) & 1;
    return {v.lo, neg ? hi | ~mask : hi};
  }
  u64 mask = width == 64 ? ~u64{0} : (u64{1} << width) - 1;
  u64 lo = v.lo & mask;
  bool neg = is_signed && (lo >> (width - 1)) & 1;
  return neg ? Bits128{lo | ~mask, ~u64{0}} : Bits128{lo, 0};
}

constexpr Bits128 bits_of(std::int64_t v) { return {u64(v), v < 0 ? ~u64{0} : 0}; }
constexpr Bits128 bits_of(u64 v) { return {v, 0}; }
constexpr bool is_negative(Bits128 v) { return std::int64_t(v.hi) < 0; }

// Two's-complement arithmetic modulo 2^128, for the 128-bit integer types (#1172). C++26 has no
// 128-bit integer, so the halves are combined by hand.
namespace wide {

constexpr bool is_zero(Bits128 a) { return (a.lo | a.hi) == 0; }
constexpr Bits128 bit_not(Bits128 a) { return {~a.lo, ~a.hi}; }
constexpr Bits128 bit_and(Bits128 a, Bits128 b) { return {a.lo & b.lo, a.hi & b.hi}; }
constexpr Bits128 bit_or(Bits128 a, Bits128 b) { return {a.lo | b.lo, a.hi | b.hi}; }
constexpr Bits128 bit_xor(Bits128 a, Bits128 b) { return {a.lo ^ b.lo, a.hi ^ b.hi}; }
constexpr Bits128 add(Bits128 a, Bits128 b) {
  u64 lo = a.lo + b.lo;
  return {lo, a.hi + b.hi + u64(lo < a.lo)};
}
constexpr Bits128 sub(Bits128 a, Bits128 b) { return {a.lo - b.lo, a.hi - b.hi - u64(a.lo < b.lo)}; }
constexpr Bits128 neg(Bits128 a) { return sub({}, a); }
constexpr bool ult(Bits128 a, Bits128 b) { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
constexpr bool slt(Bits128 a, Bits128 b) { return a.hi != b.hi ? std::int64_t(a.hi) < std::int64_t(b.hi) : a.lo < b.lo; }

// The whole product of two 64-bit numbers.
constexpr Bits128 mul64(u64 a, u64 b) {
  constexpr u64 low32 = 0xffff'ffff;
  u64 a0 = a & low32, a1 = a >> 32, b0 = b & low32, b1 = b >> 32;
  u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  u64 mid = (p00 >> 32) + (p01 & low32) + (p10 & low32);
  return {(mid << 32) | (p00 & low32), p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32)};
}
constexpr Bits128 mul(Bits128 a, Bits128 b) {
  Bits128 r = mul64(a.lo, b.lo);
  r.hi += a.lo * b.hi + a.hi * b.lo;
  return r;
}

// Shifts by `k` < 128.
constexpr Bits128 shl(Bits128 a, u32 k) {
  if (k == 0) return a;
  if (k >= 64) return {0, a.lo << (k - 64)};
  return {a.lo << k, (a.hi << k) | (a.lo >> (64 - k))};
}
constexpr Bits128 lshr(Bits128 a, u32 k) {
  if (k == 0) return a;
  if (k >= 64) return {a.hi >> (k - 64), 0};
  return {(a.lo >> k) | (a.hi << (64 - k)), a.hi >> k};
}
constexpr Bits128 ashr(Bits128 a, u32 k) {
  if (k == 0) return a;
  u64 fill = is_negative(a) ? ~u64{0} : 0;
  if (k >= 64) return {u64(std::int64_t(a.hi) >> (k - 64)), fill};
  return {(a.lo >> k) | (a.hi << (64 - k)), u64(std::int64_t(a.hi) >> k)};
}

constexpr u32 bit_width(Bits128 a) { return a.hi ? 64 + u32(std::bit_width(a.hi)) : u32(std::bit_width(a.lo)); }
constexpr u32 countr_zero(Bits128 a) { return a.lo ? u32(std::countr_zero(a.lo)) : 64 + u32(std::countr_zero(a.hi)); }

struct QuotRem {
  Bits128 quot, rem;
};
// Unsigned division by a nonzero `b`, one quotient bit at a time.
constexpr QuotRem udivrem(Bits128 a, Bits128 b) {
  if (a.hi == 0 && b.hi == 0) return {{a.lo / b.lo, 0}, {a.lo % b.lo, 0}};
  Bits128 q{}, r{};
  for (u32 i = 128; i-- > 0;) {
    // The partial remainder is at most the bits of `a` above `i`, so doubling it cannot pass 2^128.
    r = shl(r, 1);
    r.lo |= (i >= 64 ? a.hi >> (i - 64) : a.lo >> i) & 1;
    if (!ult(r, b)) {
      r = sub(r, b);
      (i >= 64 ? q.hi : q.lo) |= u64{1} << (i % 64);
    }
  }
  return {q, r};
}
// Signed division truncating toward zero, the remainder taking the dividend's sign
// ([expr.mul]/4), by a nonzero `b`; the minimum divided by -1 wraps.
constexpr QuotRem sdivrem(Bits128 a, Bits128 b) {
  bool na = is_negative(a), nb = is_negative(b);
  auto [q, r] = udivrem(na ? neg(a) : a, nb ? neg(b) : b);
  return {na != nb ? neg(q) : q, na ? neg(r) : r};
}

// The decimal digits of `a` read as unsigned.
inline std::string decimal(Bits128 a) {
  if (a.hi == 0) return std::to_string(a.lo);
  constexpr u64 chunk = 10'000'000'000'000'000'000u;  // 10^19, the largest power of ten in 64 bits
  auto [q, r] = udivrem(a, {chunk, 0});
  std::string low = std::to_string(r.lo);
  return decimal(q) + std::string(19 - low.size(), '0') + low;
}

}  // namespace wide

// ---------------------------------------------------------------- non-dependent kinds

// An integral value of `type` ([basic.fundamental]): bool, a character or integer type.
struct VInt {
  QualType type;
  Bits128 bits;
};

// A value of a floating-point type ([basic.fundamental], [basic.extended.fp]): its encoding in the
// target's format for the type (`Target::layout`), in the low bits; 128 bits hold every format.
// Values are bit patterns, so +0 and -0 are distinct and each NaN payload is its own value.
struct VFloat {
  QualType type;
  Bits128 bits;
};

struct VNullptr {};

// A pointer to a data member ([expr.unary.op], [conv.mem]). The field uses declaration order in
// the owning class. The absent field denotes the null member pointer value.
struct VMemberPtr {
  static constexpr u32 null_field = ~u32{0};
  QualType type;
  u32 field = null_field;
};

// A string literal object ([lex.string]): its array type and its code units, the terminating
// null included. String literals are identified by content, not by an entity (§3.7.9).
struct VString {
  QualType type;
  SeqId<u32> units;
};

// A declared object or function, as the base of an address (§5.5).
struct VEntity {
  EntityId entity;
};

// The address of an object or of a subobject of it, plus a byte offset (§5.5): `base` is a
// `VString` or a `VEntity`, `path` the subobject indices from it.
struct VLvalue {
  ConstId base;
  SeqId<u32> path;
  std::int64_t offset;
  bool gcc_nested_end = false;
};

// An object of array or class type ([dcl.init.aggr]): its canonical type and the value of each
// element, or of each non-static data member in declaration order.
struct VAggregate {
  QualType type;
  SeqId<ConstId> elems;
};

// ---------------------------------------------------------------- the dependent family

// A non-type template parameter ([temp.param]).
struct [[=dependent]] VDepParam {
  u16 depth;
  u16 index;
};

// A function parameter named in a trailing return type or noexcept-specifier, by position, as the
// Itanium ABI mangles it (§5.5).
struct [[=dependent]] VDepFnParam {
  u16 depth;
  u16 index;
  u8 cv = 0;
};

// A namespace variable template found at the point of definition, with dependent arguments.
struct [[=dependent]] VDepVariable {
  EntityId templ;
  SeqId<TemplateArg> args;
  SeqId<TemplateArg> written_args;
  u8 qualified;
  u8 global;
  SeqId<NameId> qualifier{};
};

// A call to a named immediate function. The arguments are explicit types for a function
// template without function arguments, or ordinary values for a non-template function.
struct [[=dependent]] VDepCall {
  QualType result_type;
  EntityId callee;
  SeqId<TemplateArg> args;
};

// A concept-id whose written template arguments still depend on a template parameter.
struct [[=dependent]] VDepConcept {
  EntityId concept_;
  SeqId<TemplateArg> args;
  bool qualified = false;
  bool global = false;
};

// A value named through a dependent nested-name-specifier.
struct [[=dependent]] VDepMember {
  QualType qualifier;
  NameId name;
};

// A field selected on a fixed class template parameter object ([expr.ref]).
struct [[=dependent]] VDepField {
  EntityId owner = EntityId::none;
  EntityId member = EntityId::none;
  QualType type;
  ConstId object = ConstId::none;
};

// The guard and two typed alternatives of a structural parameter projection. An operand
// holding a VEntity reads the named object only when that operand is selected.
struct [[=dependent]] VDepConditional {
  QualType type;
  SeqId<TemplateArg> operands;
};

enum class DepOp : u8 {
  is_same, is_reference, is_const, is_volatile, is_array, is_pointer, is_function,
  is_enum, is_union, is_class,
  is_member_pointer, is_member_object_pointer, is_member_function_pointer,
  is_object, logical_not, logical_and, is_trivially_copyable,
  is_constructible, is_nothrow_constructible, is_trivially_constructible,
  is_assignable, is_nothrow_assignable, is_trivially_assignable,
  is_convertible, is_nothrow_convertible,
  is_base_of,
  is_virtual_base_of,
  is_pointer_interconvertible_base_of,
  is_layout_compatible,
  is_empty,
  is_destructible,
  is_nothrow_destructible,
  is_trivially_destructible,
  is_bounded_array,
  is_unbounded_array,
  is_scoped_enum,
  is_polymorphic,
  is_abstract,
  is_standard_layout,
  is_final,
  is_aggregate,
  is_implicit_lifetime,
  has_virtual_destructor,
  has_unique_object_representations,
  reference_constructs_from_temporary,
  reference_converts_from_temporary,
  alignof_type,  // replayed from the written dependent argument
  array_rank,
  is_trivial, is_pod,
  sizeof_type,  // replayed from the written dependent argument
};

constexpr bool returns_size_type(DepOp op) {
  return op == DepOp::alignof_type || op == DepOp::sizeof_type || op == DepOp::array_rank;
}

// A dependent operation (§5.5). The predicates take canonical qualified type operands.
// The constructibility predicates take a target and an argument sequence. `is_same`, the class
// relations (`is_base_of`, `is_virtual_base_of`, `is_pointer_interconvertible_base_of`),
// `is_layout_compatible`, assignability, convertibility and the `reference_*_from_temporary`
// predicates take two types. The other type predicates take one.
// `alignof_type`, `sizeof_type` and `array_rank` each take one type operand.
// `logical_not` takes one bool value operand, and `logical_and` takes two.
struct [[=dependent]] VDepOp {
  DepOp op;
  SeqId<TemplateArg> operands;
};

// A non-type pack expansion `pattern...` ([temp.variadic]), an element of a dependent pack argument.
struct [[=dependent]] VDepExpansion {
  ConstId pattern;
};

// `sizeof...(P)` of template parameter pack P ([expr.sizeof]).
struct [[=dependent]] VDepSizeofPack {
  u16 depth;
  u16 index;
};

// ---------------------------------------------------------------- constraints ([temp.constr])
// *#88 step 6, Fable's decision.*

// A type as a value: a type parameter's argument in an atomic constraint's parameter mapping, its
// cv-qualifiers apart.
struct VType {
  QualType type;  // unqualified, as types in values are
  u8 cv;
};

// The operands of an operator as written, for ABI encoding before conversions and folding.
// `token` is a lex::Tk. A named cast stores its type as a VType before the operand.
struct VWrittenOperator {
  u8 token;
  SeqId<ConstId> operands;
};

// A fold-expression as written, whose operands are recorded in source order.
struct VWrittenFold {
  u8 token;
  bool left;
  ConstId first;
  ConstId second;
};

struct VWrittenCallee {
  NameId name;
  EntityId resolved;
  SeqId<NameId> qualifier;
  SeqId<TemplateArg> arguments;
  bool global;
  bool template_id;
};

struct VWrittenMember {
  ConstId object;
  NameId name;
  SeqId<TemplateArg> arguments;
  bool arrow;
  bool template_id;
};

// A pack occupies one parameter slot in an atomic constraint's mapping.
struct VConstraintPack {
  SeqId<ConstId> elements;
};

// A mapped enum argument retains its nominal type alongside the underlying integer value.
struct VConstraintEnum {
  QualType type;
  ConstId value;
};

// An atomic constraint ([temp.constr.atomic]): the appearance of its expression (the item that
// contains it, and its tokens [begin, end) in that item's declaration part), and its parameter
// mapping: one entry per parameter of the head at `depth`, a type, pack or value for a parameter the
// expression names and none for the others ([temp.constr.atomic]/1). Two are identical when they
// are one value ([temp.constr.atomic]/2). `calls` are the offsets, from `begin`, of its unqualified
// calls that were dependent where it appears: checking it adds argument-dependent lookup at the
// point of use to them ([temp.dep.candidate]). An atom of a trailing requires-clause may name the
// function's parameters, whose types may name any template parameter: its mapping is then complete.
struct VAtom {
  ItemId item;
  u32 begin;
  u32 end;
  u16 depth;
  SeqId<ConstId> mapping;
  SeqId<u32> calls;
  bool fn_params;  // it names a function parameter (a trailing requires-clause's), so it maps all
  EntityId lookup_class;  // class scope visible at the atom's spelling
};

// A normal form ([temp.constr.normal]): the conjunction (`op` 0) or disjunction (`op` 1) of normal
// forms and atoms.
struct VConstraint {
  u8 op;
  SeqId<ConstId> operands;
};

// [temp.constr.fold]: the normalized operand and the unexpanded template packs it
// names. Bind each pack element when checking satisfaction, in increasing order.
struct VFoldConstraint {
  ConstId operand;
  ConstId expression;  // token replay for a pack pattern with a nontrivial argument mapping
  SeqId<u32> packs;
  u8 op;
  SeqId<FoldPackBinding> bound;
  bool legacy = false;  // GCC uses atomic ordering for a constrained parameter pack
};

// The written form of a requires-expression in a constraint symbol.
struct [[=dependent]] VRequiresExpression {
  SeqId<QualType> parameters;
  SeqId<WrittenRequirement> requirements;
  bool parameter_list;
};

struct [[=dependent]] VRequiresParameter {
  u32 scope;
  u32 index;
  Cv cv;
};

using ValueKinds = closed_list<VInt, VFloat, VNullptr, VString, VLvalue, VEntity, VAggregate, VDepParam, VDepFnParam, VDepExpansion,
                               VDepSizeofPack, VType, VAtom, VConstraint, VDepOp, VMemberPtr, VDepVariable, VConstraintPack,
                               VDepCall, VDepMember, VDepConcept, VConstraintEnum, VDepField, VDepConditional,
                               VRequiresExpression, VRequiresParameter, VWrittenOperator, VWrittenCallee, VWrittenMember,
                               VWrittenFold, VFoldConstraint>;

template <class T>
constexpr bool is_value_kind = ValueKinds::contains<T>;

template <class T>
constexpr bool is_dependent_kind = !std::meta::annotations_of_with_type(^^T, ^^dependent_t).empty();

static_assert(std::ranges::all_of(ValueKinds::infos, [](std::meta::info k) {
  if (std::meta::size_of(k) > 32) throw std::meta::exception(u8"a value payload is at most 32 bytes", k);
  return check_record(k);
}));

}  // namespace ycxx::ast
