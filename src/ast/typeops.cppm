// What sema asks of types and constants (docs/design/ast.md §5): checked constructors with the
// construction-time rules of §5.4, the properties of the fundamental types on a target, integral
// promotions ([conv.prom]), the usual arithmetic conversions ([expr.arith.conv]), the integral
// conversion of values ([conv.integral]) and floating constants ([basic.fundamental]).
//
// Types given to the classification and conversion functions may be sugared and cv-qualified;
// results are canonical and cv-unqualified. `in.types.is<K>(q)` and `get<K>(q)` read `q`'s own
// node, which may be sugar: ask them of `in.canonical(q)`, or use the predicates here.
export module ycxx.ast.typeops;

import std;
import ycxx.ast.handles;
import ycxx.ast.types;
import ycxx.ast.values;
import ycxx.ast.target;
import ycxx.ast.interner;

export namespace ycxx::ast {

// ---------------------------------------------------------------- construction (§5.4)

QualType builtin(Interners& in, Builtin b, Cv cv = Cv::none) { return in.intern_type(TBuiltin{b}, cv); }

// The fundamental type `q` stands for, if it is one.
std::optional<Builtin> builtin_of(const Interners& in, QualType q) {
  QualType c = in.types.canon(q);
  if (!in.types.is<TBuiltin>(c)) return std::nullopt;
  return in.types.get<TBuiltin>(c).b;
}

bool is_reference(const Interners& in, QualType q) {
  QualType c = in.types.canon(q);
  return in.types.is<TLRef>(c) || in.types.is<TRRef>(c);
}
bool is_function(const Interners& in, QualType q) { return in.types.is<TFunction>(in.types.canon(q)); }
bool is_array(const Interners& in, QualType q) { return in.types.is<TArray>(in.types.canon(q)); }
bool is_void(const Interners& in, QualType q) { return builtin_of(in, q) == Builtin::void_; }
bool is_pointer(const Interners& in, QualType q) { return in.types.is<TPointer>(in.types.canon(q)); }

// The canonical type `q` points to. Expressions carry sugared types (§5.3), so a pointer is
// recognized here, through typedefs, not by `in.types.is<TPointer>(q)`, which reads `q`'s own node.
QualType pointee_of(const Interners& in, QualType q)
  pre(is_pointer(in, q))
{
  return in.types.get<TPointer>(in.types.canon(q)).pointee;
}

QualType pointer_to(Interners& in, QualType pointee, Cv cv = Cv::none)
  pre(!is_reference(in, pointee))  // [dcl.ptr]
{
  return in.intern_type(TPointer{pointee}, cv);
}

// References collapse ([dcl.ref]): a reference to a reference, through a typedef or a template
// argument, is an lvalue reference unless both are rvalue references. The referred-to reference
// keeps its spelling where it is the result.
QualType lvalue_ref_to(Interners& in, QualType referee)
  pre(!is_void(in, referee))
{
  QualType c = in.types.canon(referee);
  if (in.types.is<TLRef>(c)) return without_cv(referee);
  if (in.types.is<TRRef>(c)) return in.intern_type(TLRef{in.types.get<TRRef>(c).referee});
  return in.intern_type(TLRef{referee});
}

QualType rvalue_ref_to(Interners& in, QualType referee)
  pre(!is_void(in, referee))
{
  if (is_reference(in, referee)) return without_cv(referee);
  return in.intern_type(TRRef{referee});
}

// A parameter's type after [dcl.fct]'s adjustments: an array of `T` becomes a pointer to `T`, a
// function type a pointer to it, and top-level cv is dropped.
QualType adjust_parameter(Interners& in, QualType param) {
  QualType c = in.canonical(param);
  if (in.types.is<TArray>(c)) return pointer_to(in, in.types.get<TArray>(c).elem);
  if (in.types.is<TFunction>(c)) return pointer_to(in, without_cv(param));
  return without_cv(param);
}

ConstId bool_value(Interners& in, bool v) {
  return in.intern_value(VInt{builtin(in, Builtin::bool_), {v ? u64{1} : 0, 0}});
}

struct FunctionSpec {
  bool variadic = false;
  Cv cv = Cv::none;
  RefQual ref = RefQual::none;
  bool noexcept_ = false;
  LangLinkage linkage = LangLinkage::cxx;
};

// A function type from its declarator's parts: parameters are adjusted (§5.4).
QualType function_type(Interners& in, QualType ret, std::span<const QualType> params, FunctionSpec spec = {}) {
  std::vector<QualType> adjusted;
  adjusted.reserve(params.size());
  for (QualType p : params) adjusted.push_back(adjust_parameter(in, p));
  return in.intern_type(TFunction{ret, in.intern_seq(std::span<const QualType>(adjusted)),
                                  bool_value(in, spec.noexcept_), u8(spec.variadic), spec.cv, spec.ref,
                                  spec.linkage});
}

// ---------------------------------------------------------------- fundamental types on a target
//
// BOOTSTRAP: contracts: the functions from here on are not constexpr, as make_node
// (ycxx.ast.handles): GCC 16.2 fails to link an importer that inlines a checked inline function.

// The signed or unsigned integer type a character type or `bool` is represented as
// ([basic.fundamental]); an integer type is its own.
Builtin underlying(const Target& t, Builtin b)
  pre(is_integral(b))
{
  switch (b) {
    case Builtin::bool_: return Builtin::unsigned_char;
    case Builtin::char_: return t.char_signed ? Builtin::signed_char : Builtin::unsigned_char;
    case Builtin::wchar_t_: return t.wchar_underlying;
    case Builtin::char8_t_: return Builtin::unsigned_char;
    case Builtin::char16_t_: return t.char16_underlying;
    case Builtin::char32_t_: return t.char32_underlying;
    default: return b;
  }
}

bool is_signed(const Target& t, Builtin b) {
  return is_integral(b) && b != Builtin::bool_ && is_signed_integer(underlying(t, b));
}

// The width in bits of an integral type's values: 1 for bool, its size in bits otherwise (no
// target ycxx plans for has padding bits).
u32 width_of(const Target& t, Builtin b)
  pre(is_integral(b))
{
  return b == Builtin::bool_ ? 1 : t[b].size * 8u;
}

// Integer conversion rank ([conv.rank]): bool lowest; a character type has its underlying type's
// rank; an unsigned type its signed counterpart's. The 128-bit extended types are wider than every
// standard type, so they rank above them all.
u32 integer_rank(const Target& t, Builtin b)
  pre(is_integral(b))
{
  if (b == Builtin::bool_) return 1;
  switch (underlying(t, b)) {
    case Builtin::signed_char: case Builtin::unsigned_char: return 2;
    case Builtin::short_: case Builtin::unsigned_short: return 3;
    case Builtin::int_: case Builtin::unsigned_int: return 4;
    case Builtin::long_: case Builtin::unsigned_long: return 5;
    case Builtin::long_long: case Builtin::unsigned_long_long: return 6;
    case Builtin::int128: case Builtin::unsigned_int128: return 7;
    default: std::unreachable();
  }
}

// Whether integral type `to` can represent every value of a `width`-bit integer of the given
// signedness.
bool represents(const Target& t, Builtin to, u32 width, bool is_signed_) {
  u32 w = width_of(t, to);
  if (is_signed(t, to)) return is_signed_ ? width <= w : width < w;
  return !is_signed_ && width <= w;
}

Builtin unsigned_counterpart(Builtin b)
  pre(is_signed_integer(b) || is_unsigned_integer(b))
{
  switch (b) {
    case Builtin::signed_char: return Builtin::unsigned_char;
    case Builtin::short_: return Builtin::unsigned_short;
    case Builtin::int_: return Builtin::unsigned_int;
    case Builtin::long_: return Builtin::unsigned_long;
    case Builtin::long_long: return Builtin::unsigned_long_long;
    case Builtin::int128: return Builtin::unsigned_int128;
    default: return b;
  }
}

// ---------------------------------------------------------------- promotions ([conv.prom])

// The integral promotion of `b`, or `b` itself when none applies. `bit_width` is the width of a
// bit-field operand, 0 otherwise ([conv.prom] for bit-fields).
Builtin promoted(const Target& t, Builtin b, u32 bit_width = 0) {
  if (!is_integral(b)) return b;
  if (b == Builtin::bool_) return Builtin::int_;
  bool s = is_signed(t, b);
  if (bit_width != 0) {
    if (represents(t, Builtin::int_, bit_width, s)) return Builtin::int_;
    if (represents(t, Builtin::unsigned_int, bit_width, s)) return Builtin::unsigned_int;
  }
  u32 w = width_of(t, b);
  if (b == Builtin::wchar_t_ || b == Builtin::char8_t_ || b == Builtin::char16_t_ || b == Builtin::char32_t_) {
    for (Builtin to : {Builtin::int_, Builtin::unsigned_int, Builtin::long_, Builtin::unsigned_long, Builtin::long_long,
                       Builtin::unsigned_long_long})
      if (represents(t, to, w, s)) return to;
    return underlying(t, b);
  }
  if (integer_rank(t, b) >= integer_rank(t, Builtin::int_)) return b;
  return represents(t, Builtin::int_, w, s) ? Builtin::int_ : Builtin::unsigned_int;
}

// The promoted type of an operand of type `q` (cv and sugar ignored), canonical and unqualified.
QualType promoted(Interners& in, const Target& t, QualType q, u32 bit_width = 0) {
  QualType c = without_cv(in.canonical(q));
  if (!in.types.is<TBuiltin>(c)) return c;
  Builtin b = in.types.get<TBuiltin>(c).b;
  Builtin p = promoted(t, b, bit_width);
  return p == b ? c : builtin(in, p);
}

// ---------------------------------------------------------------- floating-point conversion rank

namespace detail {

// The rank of a standard floating type, or of an extended one with the values of one or more
// standard types ([conv.rank]): float 1, double 2, long double 3.
std::optional<u32> standard_float_rank(const Target& t, Builtin b) {
  if (b == Builtin::float_) return 1;
  if (b == Builtin::double_) return 2;
  if (b == Builtin::long_double) return 3;
  u32 matches = 0, rank = 0;
  for (Builtin s : {Builtin::float_, Builtin::double_, Builtin::long_double})
    if (t[s].format == t[b].format) ++matches, rank = *standard_float_rank(t, s);
  if (matches == 0) return std::nullopt;
  return matches == 1 ? rank : 2;
}

}  // namespace detail

// Floating-point conversion rank ([conv.rank]); unordered when neither value set contains the
// other.
std::partial_ordering float_rank_compare(const Target& t, Builtin a, Builtin b)
  pre(is_floating(a) && is_floating(b))
{
  if (!t.has(a) || !t.has(b)) return std::partial_ordering::unordered;  // the target lacks one
  auto ra = detail::standard_float_rank(t, a), rb = detail::standard_float_rank(t, b);
  if (ra && rb) return *ra <=> *rb;
  FloatShape x = shape_of(t[a].format), y = shape_of(t[b].format);
  if (x.precision == y.precision && x.exponent == y.exponent) return std::partial_ordering::equivalent;
  if (x.precision <= y.precision && x.exponent <= y.exponent) return std::partial_ordering::less;
  if (x.precision >= y.precision && x.exponent >= y.exponent) return std::partial_ordering::greater;
  return std::partial_ordering::unordered;
}

// Of two floating types of equal rank, the one of greater subrank: an extended type over a
// standard one ([conv.rank]); between extended types the order is ours to define, by `Builtin`.
Builtin greater_subrank(Builtin a, Builtin b) {
  if (is_standard_floating(a) != is_standard_floating(b)) return is_standard_floating(a) ? b : a;
  return a < b ? b : a;
}

// ---------------------------------------------------------------- [expr.arith.conv]

// The common type of the usual arithmetic conversions, or none when they do not apply (an operand
// is not arithmetic) or are ill-formed (unordered floating types). Unscoped enumerations come
// with #33's enums.
std::optional<Builtin> usual_arithmetic(const Target& t, Builtin a, Builtin b) {
  if (!is_arithmetic(a) || !is_arithmetic(b)) return std::nullopt;
  if (is_floating(a) || is_floating(b)) {
    if (a == b) return a;
    if (!is_floating(b)) return a;
    if (!is_floating(a)) return b;
    auto o = float_rank_compare(t, a, b);
    if (o == std::partial_ordering::unordered) return std::nullopt;
    if (o == std::partial_ordering::equivalent) return greater_subrank(a, b);
    return o == std::partial_ordering::greater ? a : b;
  }
  a = promoted(t, a);
  b = promoted(t, b);
  if (a == b) return a;
  bool sa = is_signed(t, a), sb = is_signed(t, b);
  if (sa == sb) return integer_rank(t, a) >= integer_rank(t, b) ? a : b;
  Builtin s = sa ? a : b, u = sa ? b : a;
  if (integer_rank(t, u) >= integer_rank(t, s)) return u;
  if (represents(t, s, width_of(t, u), false)) return s;
  return unsigned_counterpart(s);
}

std::optional<QualType> usual_arithmetic(Interners& in, const Target& t, QualType a, QualType b) {
  auto ba = builtin_of(in, a), bb = builtin_of(in, b);
  if (!ba || !bb) return std::nullopt;
  auto r = usual_arithmetic(t, *ba, *bb);
  if (!r) return std::nullopt;
  return builtin(in, *r);
}

// ---------------------------------------------------------------- integer constants

// The value of integral type `q` congruent to `bits` modulo 2^width ([conv.integral]); for bool,
// whether `bits` is nonzero ([conv.bool]).
ConstId int_value(Interners& in, const Target& t, QualType q, Bits128 bits) {
  auto b = builtin_of(in, q);
  contract_assert(b && is_integral(*b));
  QualType c = builtin(in, *b);
  if (*b == Builtin::bool_) return in.intern_value(VInt{c, {(bits.lo | bits.hi) != 0 ? u64{1} : 0, 0}});
  return in.intern_value(VInt{c, wrap(bits, width_of(t, *b), is_signed(t, *b))});
}
ConstId int_value(Interners& in, const Target& t, QualType q, std::int64_t v) { return int_value(in, t, q, bits_of(v)); }
ConstId int_value(Interners& in, const Target& t, Builtin b, std::int64_t v) {
  return int_value(in, t, builtin(in, b), bits_of(v));
}

Bits128 int_bits(Interners& in, ConstId c)
  pre(in.values.is<VInt>(c))
{
  return in.values.get<VInt>(c).bits;
}

// The integral conversion of constant `c` to integral type `to`.
ConstId convert_integral(Interners& in, const Target& t, ConstId c, QualType to)
  pre(in.values.is<VInt>(c))
{
  return int_value(in, t, to, int_bits(in, c));
}

// ---------------------------------------------------------------- floating constants

// Whether ycxx computes with values of format `f`: those of the host's `float` and `double`, IEEE
// binary32 and binary64, in round-to-nearest-even. Values of the other formats are not supported
// yet (#336).
constexpr bool computes(FloatFormat f) { return f == FloatFormat::binary32 || f == FloatFormat::binary64; }

// The format of floating type `q` on target `t`.
FloatFormat float_format(const Interners& in, const Target& t, QualType q) {
  auto b = builtin_of(in, q);
  contract_assert(b && is_floating(*b));
  return t[*b].format;
}

// The value of floating type `q` encoded by `bits` in its format.
ConstId float_value(Interners& in, const Target& t, QualType q, Bits128 bits) {
  auto b = builtin_of(in, q);
  contract_assert(b && is_floating(*b));
  u32 width = t[*b].size * 8;
  return in.intern_value(VFloat{builtin(in, *b), wrap(bits, width < 128 ? width : 128, false)});
}

// `v` as a value of floating type `q`, rounded to nearest, ties to even, when inexact
// ([conv.double]: the choice is implementation-defined); a value beyond the type's finite range
// becomes an infinity.
ConstId float_value(Interners& in, const Target& t, QualType q, double v) {
  FloatFormat f = float_format(in, t, q);
  contract_assert(computes(f));
  u64 bits = f == FloatFormat::binary32 ? std::bit_cast<std::uint32_t>(static_cast<float>(v)) : std::bit_cast<u64>(v);
  return float_value(in, t, q, Bits128{bits, 0});
}

// The value of a floating constant of a format ycxx computes with, exactly, as a `double`.
double float_double(const Interners& in, const Target& t, ConstId c) {
  contract_assert(in.values.is<VFloat>(c));
  VFloat v = in.values.get<VFloat>(c);
  FloatFormat f = float_format(in, t, v.type);
  contract_assert(computes(f));
  if (f == FloatFormat::binary32) return std::bit_cast<float>(static_cast<std::uint32_t>(v.bits.lo));
  return std::bit_cast<double>(v.bits.lo);
}

// ---------------------------------------------------------------- arrays

// An array of `elem`. The bound is a converted constant expression of type `std::size_t`
// ([dcl.array]), so an integer bound is converted here and `int[3]` is one type whatever the
// bound's spelling; `ConstId::none` is an unknown bound, and a dependent bound is kept. Sema
// diagnoses an element type that is not a complete object type and a bound that is not positive;
// the precondition rejects only what cannot be represented.
QualType array_of(Interners& in, const Target& t, QualType elem, ConstId bound = ConstId::none)
  pre(!is_reference(in, elem) && !is_function(in, elem) && !is_void(in, elem))
  pre(bound == ConstId::none || in.values.is_dependent(bound) || in.values.is<VInt>(bound))
{
  if (in.values.is<VInt>(bound)) bound = convert_integral(in, t, bound, builtin(in, t.size_type));
  return in.intern_type(TArray{elem, bound});
}

}  // namespace ycxx::ast
