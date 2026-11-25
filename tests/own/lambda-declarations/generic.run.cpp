// Generic lambdas in declarations (#338): a specialization of the call operator is analysed in the
// declaration, in the class or at the point that uses it ([expr.prim.lambda.closure]/7).
struct A {
  static constexpr int k = 4;
  static constexpr int n = [](auto... xs) { return (xs + ... + k); }(1, 2, 3);
  static constexpr auto f = [](auto x) { return x + k; };
  static constexpr int z = f(3);
  static constexpr int y = [] { return f(1) + z; }();
  static constexpr auto nest = [](auto x) { return [=](auto y) { return x + y; }(2); };
};
template <class T>
struct L {
  static constexpr int k = sizeof(T);
  static constexpr T m = [](auto x) { return x + k; }(1);
  static constexpr auto f = [](auto x) { return x + k; };
  static constexpr T q = f(2) + m;
};
namespace N {
inline constexpr int v = [](auto a) { return a + 1; }(2);
inline constexpr auto w = [](auto a) { return a + 1; };
}
template <class F>
int call(F f) { return f(4); }
static_assert(A::n == 10 && A::z == 7 && A::y == 12 && A::nest(1) == 3);
static_assert(L<int>::m == 5 && L<int>::q == 11 && L<char>::f(1) == 2 && N::v == 3 && N::w(1) == 2);
int main() {
  int o = 1;
  long l = 2;
  if (A::f(o) != 5 || A::f(l) != 6 || L<short>::f(o) != 3 || A::nest(o) != 3) return 1;
  return call(L<int>::f) == 8 && N::w(o) == 2 && call(N::w) == 5 ? 0 : 2;
}
