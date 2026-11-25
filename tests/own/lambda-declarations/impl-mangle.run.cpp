// The closure types of declarations' lambdas have the names Clang gives them (#338): the variable
// or member is a <closure-prefix>, `<name> M`, its name a substitution candidate, then
// `Ul <lambda-sig> E [n] _` (docs/design/closures.md). The peer, compiled by Clang, calls ycxx's
// definitions by those names.
// PEER: impl-mangle-peer.cpp
// XFAIL: gcc GCC names the invoker `_FUN` and omits a variable template's `M`
struct A {
  static constexpr auto f = [](A*, A**) { return 1; };
  static constexpr auto g = [](auto x, A*) { return x + 2; };
  static constexpr auto two = [](int x) { return [](int y) { return y + 3; }(x); };
  static constexpr int (*p)(int) = [](int x) { return x + 4; };
};
namespace N {
struct Q {};
template <class T>
struct B {
  static constexpr auto f = [](T x, Q*) { return x + 5; };
  static constexpr auto g = [](auto x, T) { return x + 6; };
};
inline constexpr auto v = [](int x, Q*) { return x + 7; };
inline constexpr auto w = [](auto x, Q*) { return x + 8; };
template <class T>
inline constexpr auto vt = [](T x, Q*) { return x + 9; };
template <class T>
inline constexpr auto gt = [](auto x, Q*) { return x + 10; };
}
int peer_check();
// Arguments no compiler folds the calls with.
int calls(int one, A* pa, N::Q* q) {
  int (*p)(int) = A::p;
  if (A::f(pa, &pa) != 1 || A::g(one, pa) != 3 || A::two(one) != 4 || p(one) != 5) return 1;
  if (N::B<int>::f(one, q) != 6 || N::B<N::Q*>::g(one, q) != 7 || N::v(one, q) != 8 || N::w(one, q) != 9) return 2;
  return N::vt<int>(one, q) == 10 && N::gt<int>(one, q) == 11 ? 0 : 3;
}
int main() {
  A a;
  N::Q q;
  if (int r = calls(1, &a, &q)) return r;
  return peer_check();
}
