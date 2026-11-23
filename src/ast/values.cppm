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
};

// A namespace variable template found at the point of definition, with dependent arguments.
struct [[=dependent]] VDepVariable {
  EntityId templ;
  SeqId<TemplateArg> args;
};

// A call to a named immediate function. The arguments are explicit types for a function
// template without function arguments, or ordinary values for a non-template function.
struct [[=dependent]] VDepCall {
  EntityId callee;
  SeqId<TemplateArg> args;
};

// A concept-id whose written template arguments still depend on a template parameter.
struct [[=dependent]] VDepConcept {
  EntityId concept_;
  SeqId<TemplateArg> args;
};

// A value named through a dependent nested-name-specifier.
struct [[=dependent]] VDepMember {
  QualType qualifier;
  NameId name;
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
};

// A dependent operation (§5.5). The predicates take canonical qualified type operands.
// The constructibility predicates take a target and an argument sequence,
// `is_same`, `is_base_of`, assignability and convertibility take two operands. The other predicates take one.
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

using ValueKinds = closed_list<VInt, VFloat, VNullptr, VString, VLvalue, VEntity, VAggregate, VDepParam, VDepFnParam, VDepExpansion,
                               VDepSizeofPack, VType, VAtom, VConstraint, VDepOp, VMemberPtr, VDepVariable, VConstraintPack,
                               VDepCall, VDepMember, VDepConcept, VConstraintEnum>;

template <class T>
constexpr bool is_value_kind = ValueKinds::contains<T>;

template <class T>
constexpr bool is_dependent_kind = !std::meta::annotations_of_with_type(^^T, ^^dependent_t).empty();

static_assert(std::ranges::all_of(ValueKinds::infos, [](std::meta::info k) {
  if (std::meta::size_of(k) > 32) throw std::meta::exception(u8"a value payload is at most 32 bytes", k);
  return check_record(k);
}));

}  // namespace ycxx::ast
