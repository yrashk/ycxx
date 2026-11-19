// Types and constants (ast.md §5): hash-consing, canon[] and the construction-time rules of §5.4,
// target layouts, [conv.rank], [conv.prom], [expr.arith.conv] and [conv.integral].
import std;
import ycxx.ast;
import ycxx.unit;

using namespace ycxx::ast;
using ycxx::unit::check;

namespace {

void interning() {
  Interners in;
  QualType i = builtin(in, Builtin::int_);
  check(builtin(in, Builtin::int_) == i && builtin(in, Builtin::long_) != i, "builtins are hash-consed");
  QualType ci = builtin(in, Builtin::int_, Cv::const_);
  check(type_node_of(ci) == type_node_of(i) && cv_of(ci) == Cv::const_, "cv lives in the handle");
  check(in.fp(ci) != in.fp(i), "cv is in the fingerprint");
  QualType p = pointer_to(in, ci);
  check(pointer_to(in, ci) == p && pointer_to(in, i) != p, "pointer to const int is one node");
  check(in.canonical(p) == p, "a node with canonical children is its own canonical form");

  // Fingerprints do not depend on interning order (§2, invariant 3).
  Interners other;
  QualType l = builtin(other, Builtin::long_);
  QualType p2 = pointer_to(other, builtin(other, Builtin::int_, Cv::const_));
  check(other.fp(p2) == in.fp(p) && l != builtin(in, Builtin::long_), "fingerprints are order-independent");
}

void canon() {
  Interners in;
  QualType i = builtin(in, Builtin::int_);
  QualType ci = builtin(in, Builtin::int_, Cv::const_);
  EntityId alias = in.entities.add<EntityId>(seed("A"));

  // `typedef const int A;`: the cv the typedef adds survives in canon[] (§5.2).
  QualType a = in.intern_type(STypedef{ci, alias});
  check(a != ci && in.canonical(a) == ci, "canonical(A) is const int");
  check(in.canonical(with_cv(a, Cv::volatile_)) == builtin(in, Builtin::int_, Cv::const_volatile),
        "cv on a typedef merges with the typedef's");
  check(in.same_type(a, ci) && !in.same_type(a, i), "same_type compares canonical forms");

  // A pointer to sugar: canon is the pointer to the canonical type.
  QualType pa = pointer_to(in, a);
  check(pa != pointer_to(in, ci) && in.canonical(pa) == pointer_to(in, ci), "canon(A*) is const int*");

  // cv on an array goes to its elements ([basic.type.qualifier]).
  ConstId three = int_value(in, aarch64_darwin, Builtin::unsigned_long, 3);
  QualType arr = array_of(in, aarch64_darwin, i, three);
  check(in.qualified(arr, Cv::const_) == array_of(in, aarch64_darwin, ci, three), "const applied to int[3] is const int[3]");
  // ... and through a typedef only when canonicalized.
  QualType arr_t = in.intern_type(STypedef{arr, in.entities.add<EntityId>(seed("Arr"))});
  QualType c_arr_t = in.qualified(arr_t, Cv::const_);
  check(type_node_of(c_arr_t) == type_node_of(arr_t) && cv_of(c_arr_t) == Cv::const_, "const Arr keeps its spelling");
  check(in.canonical(c_arr_t) == array_of(in, aarch64_darwin, ci, three), "canonical(const Arr) is const int[3]");

  // cv on a reference or function type through a typedef is dropped ([dcl.ref], §5.4).
  QualType r = lvalue_ref_to(in, i);
  QualType r_t = in.intern_type(STypedef{r, in.entities.add<EntityId>(seed("R"))});
  check(in.qualified(r_t, Cv::const_) == r_t && in.canonical(with_cv(r_t, Cv::const_)) == r,
        "const R is R when R is int&");
  check(in.qualified(r, Cv::const_) == r, "int& const is int&");
  QualType f = function_type(in, i, {});
  check(in.qualified(f, Cv::volatile_) == f, "cv on a function type is dropped");

  // The bound is converted to size_t ([dcl.array]): `int[3]` is one type however 3 is spelled.
  check(array_of(in, aarch64_darwin, i, int_value(in, aarch64_darwin, Builtin::int_, 3)) == arr, "int[3] with an int bound");
  check(in.values.get<VInt>(in.types.get<TArray>(arr).bound).type == builtin(in, Builtin::unsigned_long), "the bound is size_t");
  // cv on nested array sugar: canonical() is a read of what qualified() interned.
  QualType arr2_t = in.intern_type(STypedef{array_of(in, aarch64_darwin, arr_t, three), in.entities.add<EntityId>(seed("Arr2"))});
  const Interners& cin = in;
  check(cin.canonical(in.qualified(arr2_t, Cv::volatile_)) ==
            array_of(in, aarch64_darwin, array_of(in, aarch64_darwin, builtin(in, Builtin::int_, Cv::volatile_), three), three),
        "canonical(volatile Arr2) is volatile int[3][3]");
  check(cin.same_type(c_arr_t, array_of(in, aarch64_darwin, ci, three)), "same_type on const Interners");

  // Arrays of unknown bound are distinct from every bounded array.
  check(array_of(in, aarch64_darwin, i) != arr && in.types.get<TArray>(array_of(in, aarch64_darwin, i)).bound == ConstId::none, "int[] has no bound");
}

void pointers() {
  Interners in;
  QualType i = builtin(in, Builtin::int_);
  QualType p = pointer_to(in, i);
  QualType p_t = in.intern_type(STypedef{p, in.entities.add<EntityId>(seed("P"))});
  // `is<TPointer>` reads the node; is_pointer and pointee_of see through sugar (§5.3).
  check(!in.types.is<TPointer>(p_t) && is_pointer(in, p_t) && pointee_of(in, p_t) == i, "a typedef of int* is a pointer");
  check(!is_pointer(in, i) && is_pointer(in, with_cv(p, Cv::const_)), "int is not a pointer; int* const is");
}

void references() {
  Interners in;
  QualType i = builtin(in, Builtin::int_);
  QualType lr = lvalue_ref_to(in, i);
  QualType rr = rvalue_ref_to(in, i);
  check(in.types.is<TLRef>(lr) && in.types.is<TRRef>(rr) && lr != rr, "int& and int&& differ");
  // Reference collapsing ([dcl.ref]).
  check(lvalue_ref_to(in, lr) == lr, "& & is &");
  check(lvalue_ref_to(in, rr) == lr, "&& & is &");
  check(rvalue_ref_to(in, lr) == lr, "& && is &");
  check(rvalue_ref_to(in, rr) == rr, "&& && is &&");
  // Through a typedef, the referred-to reference keeps its spelling.
  QualType r_t = in.intern_type(STypedef{lr, in.entities.add<EntityId>(seed("R"))});
  check(rvalue_ref_to(in, r_t) == r_t && in.canonical(lvalue_ref_to(in, r_t)) == lr, "R&& is R; R& is int&");
  QualType rr_t = in.intern_type(STypedef{rr, in.entities.add<EntityId>(seed("RR"))});
  check(lvalue_ref_to(in, rr_t) == lr, "RR& is int&");
}

void functions() {
  Interners in;
  QualType i = builtin(in, Builtin::int_);
  QualType c = builtin(in, Builtin::char_);
  QualType cc = builtin(in, Builtin::char_, Cv::const_);
  ConstId four = int_value(in, aarch64_darwin, Builtin::unsigned_long, 4);

  // [dcl.fct]: arrays and functions decay, top-level cv is dropped.
  std::array p1{array_of(in, aarch64_darwin, c, four), builtin(in, Builtin::int_, Cv::const_)};
  std::array p2{pointer_to(in, c), i};
  QualType f1 = function_type(in, i, p1);
  QualType f2 = function_type(in, i, p2);
  check(f1 == f2, "int(char[4], const int) is int(char*, int)");
  std::array p3{function_type(in, i, {})};
  std::array p4{pointer_to(in, function_type(in, i, {}))};
  check(function_type(in, i, p3) == function_type(in, i, p4), "a function parameter becomes a pointer to function");
  // cv below the top level stays.
  std::array p5{pointer_to(in, cc)};
  check(function_type(in, i, p5) != f2, "int(const char*) is not int(char*)");

  // A typedef parameter keeps its spelling; canonically, top-level cv is gone.
  QualType ci_t = in.intern_type(STypedef{builtin(in, Builtin::int_, Cv::const_), in.entities.add<EntityId>(seed("CI"))});
  std::array p6{ci_t};
  std::array p7{i};
  QualType f6 = function_type(in, i, p6);
  check(f6 != function_type(in, i, p7) && in.canonical(f6) == function_type(in, i, p7), "void(CI) is canonically void(int)");

  // noexcept, variadic, qualifiers and language linkage are part of the type.
  check(function_type(in, i, {}, {.noexcept_ = true}) != function_type(in, i, {}), "noexcept is in the type");
  check(function_type(in, i, {}, {.variadic = true}) != function_type(in, i, {}), "... is in the type");
  check(function_type(in, i, {}, {.cv = Cv::const_}) != function_type(in, i, {}), "member cv is in the type");
  check(function_type(in, i, {}, {.ref = RefQual::lvalue}) != function_type(in, i, {}), "ref-qualifier is in the type");
  QualType puts = function_type(in, i, std::array{pointer_to(in, cc)}, {.linkage = LangLinkage::c});
  check(puts != function_type(in, i, std::array{pointer_to(in, cc)}) &&
            in.types.get<TFunction>(puts).linkage == LangLinkage::c,
        "extern \"C\" int(const char*) has C language linkage");
  TFunction ft = in.types.get<TFunction>(f1);
  check(ft.ret == i && in.seqs.size(ft.params) == 2 && in.seqs.at(ft.params, 0) == pointer_to(in, c),
        "parameters are stored adjusted");
  check(in.values.is<VInt>(ft.noexcept_) && int_bits(in, ft.noexcept_) == Bits128{0, 0}, "noexcept(false) is the bool false");
}

void layouts() {
  const Target& t = aarch64_darwin;
  check(t[Builtin::int_].size == 4 && t[Builtin::long_].size == 8 && t[Builtin::long_double].size == 8,
        "AArch64 Darwin: int 4, long 8, long double 8");
  check(t[Builtin::bool_].size == 1 && t[Builtin::wchar_t_].size == 4 && t.pointer_size == 8, "bool 1, wchar_t 4, pointers 8");
  check(!t.has(Builtin::float128) && x86_64_linux.has(Builtin::float128), "binary128 only where the target has it");
  check(x86_64_linux[Builtin::long_double].size == 16 && x86_64_linux[Builtin::long_double].align == 16, "x86-64 long double");
  check(is_signed(t, Builtin::char_) && !is_signed(t, Builtin::char8_t_) && is_signed(t, Builtin::wchar_t_) &&
            !is_signed(t, Builtin::bool_),
        "signedness of the character types");
  check(width_of(t, Builtin::bool_) == 1 && width_of(t, Builtin::short_) == 16 && width_of(t, Builtin::char32_t_) == 32,
        "integer widths");
}

void ranks() {
  const Target& t = aarch64_darwin;
  using enum Builtin;
  // [conv.rank]: ranks follow the standard chain, unsigned equal to signed, characters by underlying type.
  check(integer_rank(t, bool_) < integer_rank(t, char_) && integer_rank(t, char_) == integer_rank(t, signed_char) &&
            integer_rank(t, signed_char) < integer_rank(t, short_) && integer_rank(t, short_) < integer_rank(t, int_) &&
            integer_rank(t, int_) < integer_rank(t, long_) && integer_rank(t, long_) < integer_rank(t, long_long),
        "the integer rank chain");
  check(integer_rank(t, unsigned_long) == integer_rank(t, long_), "unsigned has its signed counterpart's rank");
  check(integer_rank(t, char16_t_) == integer_rank(t, unsigned_short) && integer_rank(t, wchar_t_) == integer_rank(t, int_),
        "character types rank as their underlying types");

  // Floating-point ranks: standard types strictly ordered even when long double is double.
  check(float_rank_compare(t, long_double, double_) == std::partial_ordering::greater, "long double > double");
  check(float_rank_compare(t, float_, double_) == std::partial_ordering::less, "float < double");
  check(float_rank_compare(t, float32, float_) == std::partial_ordering::equivalent, "float32_t ranks as float");
  // float64_t has the values of double and long double here, so the rank of double.
  check(float_rank_compare(t, float64, double_) == std::partial_ordering::equivalent &&
            float_rank_compare(t, float64, long_double) == std::partial_ordering::less,
        "float64_t on Darwin ranks as double");
  check(float_rank_compare(t, float16, bfloat16) == std::partial_ordering::unordered, "float16_t and bfloat16_t are unordered");
  check(float_rank_compare(t, float16, float_) == std::partial_ordering::less, "float16_t < float");
  // A type the target lacks orders with nothing ([basic.extended.fp]: float128_t is optional).
  check(float_rank_compare(t, float128, double_) == std::partial_ordering::unordered && !usual_arithmetic(t, float128, double_),
        "float128_t is absent on Darwin");
  check(float_rank_compare(x86_64_linux, float128, long_double) == std::partial_ordering::greater,
        "float128_t > x87 long double");
}

void promotions() {
  const Target& t = aarch64_darwin;
  using enum Builtin;
  // [conv.prom]
  check(promoted(t, bool_) == int_, "bool promotes to int");
  check(promoted(t, char_) == int_ && promoted(t, unsigned_char) == int_ && promoted(t, unsigned_short) == int_,
        "small integers promote to int");
  check(promoted(t, int_) == int_ && promoted(t, unsigned_int) == unsigned_int && promoted(t, long_) == long_,
        "int and above do not promote");
  check(promoted(t, char8_t_) == int_ && promoted(t, char16_t_) == int_ && promoted(t, wchar_t_) == int_,
        "char8_t, char16_t and wchar_t promote to int");
  check(promoted(t, char32_t_) == unsigned_int, "char32_t promotes to unsigned int");
  check(promoted(t, double_) == double_ && promoted(t, float_) == float_, "floating types are not integral-promoted");
  // Bit-fields.
  check(promoted(t, unsigned_int, 31) == int_, "a 31-bit unsigned bit-field promotes to int");
  check(promoted(t, unsigned_int, 32) == unsigned_int, "a 32-bit unsigned bit-field promotes to unsigned int");
  check(promoted(t, long_, 40) == long_, "a 40-bit long bit-field keeps its type");

  Interners in;
  QualType cs = in.intern_type(STypedef{builtin(in, short_, Cv::const_), in.entities.add<EntityId>(seed("CS"))});
  check(promoted(in, t, cs) == builtin(in, int_), "the promoted type is canonical and unqualified");
  check(promoted(in, t, builtin(in, long_, Cv::const_)) == builtin(in, long_), "cv is dropped");
}

void arithmetic() {
  const Target& t = aarch64_darwin;
  using enum Builtin;
  auto uac = [&](Builtin a, Builtin b) { return usual_arithmetic(t, a, b); };
  // [expr.arith.conv]
  check(uac(char_, short_) == int_, "char + short is int");
  check(uac(int_, unsigned_int) == unsigned_int, "int + unsigned is unsigned");
  check(uac(long_, unsigned_int) == long_, "long + unsigned: long represents every unsigned");
  check(uac(long_long, unsigned_long) == unsigned_long_long, "long long + unsigned long is unsigned long long (LP64)");
  check(uac(unsigned_long, long_) == unsigned_long, "equal ranks: the unsigned type");
  check(uac(bool_, bool_) == int_, "bool + bool is int");
  check(uac(int_, double_) == double_ && uac(float_, long_long) == float_, "an integer converts to the floating type");
  check(uac(float_, double_) == double_ && uac(double_, long_double) == long_double, "the greater floating rank");
  check(uac(float64, double_) == float64 && uac(double_, float64) == float64, "equal rank: float64_t has the greater subrank");
  check(!uac(float16, bfloat16), "unordered floating types are ill-formed");
  check(!uac(int_, void_) && !uac(nullptr_t, int_), "non-arithmetic operands");
  check(uac(char32_t_, int_) == unsigned_int, "char32_t + int is unsigned int");

  Interners in;
  auto r = usual_arithmetic(in, t, builtin(in, int_, Cv::const_), builtin(in, unsigned_long));
  check(r && *r == builtin(in, unsigned_long), "usual_arithmetic on QualTypes");
  check(!usual_arithmetic(in, t, pointer_to(in, builtin(in, int_)), builtin(in, int_)), "a pointer is not arithmetic");
}

void constants() {
  const Target& t = aarch64_darwin;
  using enum Builtin;
  Interners in;
  ConstId a = int_value(in, t, int_, 42);
  check(int_value(in, t, int_, 42) == a && int_value(in, t, long_, 42) != a, "constants are hash-consed with their type");
  check(int_bits(in, a) == bits_of(std::int64_t{42}), "42 reads back");
  ConstId m1 = int_value(in, t, int_, -1);
  check(int_bits(in, m1) == Bits128{~0ull, ~0ull}, "-1 is sign-extended");

  // [conv.integral]: wraparound, modulo 2^N.
  QualType u = builtin(in, unsigned_int);
  check(int_bits(in, convert_integral(in, t, m1, u)) == Bits128{0xffffffffull, 0}, "(unsigned)-1 is 2^32-1");
  ConstId big = int_value(in, t, long_, 0x1'8000'0001);
  check(int_bits(in, convert_integral(in, t, big, builtin(in, int_))) == bits_of(std::int64_t{-2147483647}),
        "(int)0x180000001 is -2147483647");
  check(int_bits(in, convert_integral(in, t, int_value(in, t, int_, 300), builtin(in, char_))) == bits_of(std::int64_t{44}),
        "(char)300 is 44");
  check(int_bits(in, convert_integral(in, t, int_value(in, t, int_, 200), builtin(in, signed_char))) ==
            bits_of(std::int64_t{-56}),
        "(signed char)200 is -56");
  check(int_bits(in, convert_integral(in, t, m1, builtin(in, unsigned_long_long))) == Bits128{~0ull, 0},
        "(unsigned long long)-1");
  // [conv.bool]
  check(convert_integral(in, t, int_value(in, t, int_, 256), builtin(in, bool_)) == bool_value(in, true), "(bool)256 is true");
  check(convert_integral(in, t, int_value(in, t, int_, 0), builtin(in, bool_)) == bool_value(in, false), "(bool)0 is false");
  check(int_bits(in, convert_integral(in, t, bool_value(in, true), builtin(in, int_))) == bits_of(std::int64_t{1}), "(int)true is 1");
  // The value's type is canonical and unqualified, whatever was asked for.
  check(int_value(in, t, builtin(in, int_, Cv::const_), std::int64_t{42}) == a, "const int 42 is int 42");

  check(wrap(Bits128{0, 1}, 65, true) == Bits128{0, ~0ull} && wrap(Bits128{0, 1}, 65, false) == Bits128{0, 1},
        "wrap past 64 bits");

  // The dependent family's shape (§5.5).
  ConstId d = in.intern_value(VDepParam{0, 1});
  check(in.intern_value(VDepParam{0, 1}) == d && in.intern_value(VDepFnParam{0, 1}) != d, "VDep* values are hash-consed by kind");
  check(in.values.is_dependent(d) && !in.values.is_dependent(a) && !in.values.is_dependent(ConstId::none), "is_dependent");
  // An array bound may be dependent.
  QualType da = array_of(in, aarch64_darwin, builtin(in, int_), d);
  check(in.canonical(da) == da && in.types.get<TArray>(da).bound == d, "T[N] holds VDepParam");
}

void bit_field_layout() {
  for (const Target& target : {aarch64_darwin, x86_64_linux}) {
    auto integer = scalar_placed(target[Builtin::int_].size, target[Builtin::int_].align);
    auto character = scalar_placed(1, 1);
    auto long_ = scalar_placed(target[Builtin::long_].size, target[Builtin::long_].align);
    auto bits = [&](u8 width) {
      Placed field = integer;
      field.bit_width = width;
      return field;
    };
    auto file_slice = record_layout({}, std::array{integer, bits(24), character, long_}, false, true, 0);
    check(file_slice.offsets == std::vector<std::uint64_t>{0, 4, 7, 8} && file_slice.size == 16 && file_slice.align == 8 &&
              file_slice.bit_fields[1].mask() == 0xffffffu && file_slice.bit_fields[1].bytes() == 3,
          "the signed 24-bit field occupies exactly three bytes before the ordinary char");
    auto common = record_layout({}, std::array{bits(3), bits(5), character}, false, true, 0);
    check(common.offsets == std::vector<std::uint64_t>{0, 0, 1} && common.size == 4 &&
              common.bit_fields[0].mask() == 0x07u && common.bit_fields[1].mask() == 0xf8u,
          "consecutive fields can share one byte");
    auto boundary = record_layout({}, std::array{bits(31), bits(2), character}, false, true, 0);
    check(boundary.offsets == std::vector<std::uint64_t>{0, 4, 5} && boundary.size == 8,
          "a bit-field that does not fit starts a fresh allocation unit");
  }
}

void compaction() {
  // Lists held only by types survive compaction and are rewritten in place.
  Interners in;
  QualType i = builtin(in, Builtin::int_);
  in.intern_seq({builtin(in, Builtin::long_)});  // garbage, dropped
  std::array ps{builtin(in, Builtin::char_), i};
  QualType f = function_type(in, i, ps);
  Fp128 before = in.fp(f);
  compact(in, [](Compactor&) {});
  TFunction ft = in.types.get<TFunction>(f);
  check(in.fp(f) == before && in.seqs.size(ft.params) == 2 && in.seqs.at(ft.params, 1) == i,
        "a function type's parameters survive compaction");
  check(function_type(in, i, ps) == f, "re-interning finds the same type");
}

}  // namespace

int main() {
  interning();
  canon();
  pointers();
  references();
  functions();
  layouts();
  ranks();
  promotions();
  arithmetic();
  constants();
  bit_field_layout();
  compaction();
  return ycxx::unit::result("ast.types");
}
