// #774: an argument of unscoped enumeration type for an integral parameter converts through its
// underlying type ([temp.arg.nontype]/1, [conv.prom]), also as a default naming an earlier
// enumeration parameter; overloads on an enumeration and an `int` parameter each take their own.
// EXIT: 0
enum class K { a, b, c };
enum U { u0, u1, u2 };
template<K k> int f() { return 10 + int(k); }
template<int i> int f() { return 20 + i; }
template<int k> struct S { static constexpr int v = k; };
template<U a, int n = a> struct E { static constexpr int v = n; };
int main() {
  if (f<K::b>() != 11 || f<1>() != 21) return 1;
  if (S<u2>::v != 2 || E<u1>::v != 1) return 2;
  return 0;
}
