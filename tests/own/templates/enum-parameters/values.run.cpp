// #774: non-type template parameters of enumeration type ([temp.param]/7: an enumeration is a
// structural type). Scoped and unscoped, with a fixed underlying type and in a namespace;
// explicit and partial specializations on enumerator values, a default argument, deduction from
// a specialization, and arguments converted as [temp.arg.nontype] says.
// EXIT: 0
enum class K { a, b };
enum U : short { u0, u1 = -3 };
namespace n { enum class N : unsigned char { x = 200 }; }

template<K k> struct I;
template<> struct I<K::a> { static int g() { return 1; } };
template<> struct I<K::b> { static int g() { return 2; } };
template<K k> int f() { return I<k>::g(); }
template<U u> int h() { return u; }
template<n::N v> int m() { return int(v); }
template<K k, U u = u1> struct P { static constexpr int v = 0; };
template<U u> struct P<K::b, u> { static constexpr int v = u; };
template<K k> int deduce(I<k>) { return k == K::b ? 20 : 10; }

int main() {
  if (f<K::a>() != 1 || f<K::b>() != 2) return 1;
  if (h<u1>() != -3 || h<static_cast<U>(5)>() != 5) return 2;
  if (m<n::N::x>() != 200) return 3;
  if (P<K::b>::v != -3 || P<K::a>::v != 0 || P<K::b, u0>::v != 0) return 4;
  if (deduce(I<K::b>{}) != 20 || deduce(I<K::a>{}) != 10) return 5;
  return 0;
}
