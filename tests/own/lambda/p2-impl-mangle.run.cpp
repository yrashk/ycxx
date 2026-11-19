// θ step 2 (#280): closure types in inline functions, member functions and function templates
// have the Itanium names Clang gives them (`Ul <lambda-sig> E [n] _` numbered per signature in
// closing-brace order, in a <local-name>; the static invoker `8__invoke`), and their captures are
// laid out in capture order (docs/design/closures.md). The peer, compiled by Clang, calls ycxx's
// definitions by those names with closure objects it builds itself.
// PEER: p2-impl-mangle-peer.cpp
// XFAIL: gcc GCC names a closure's static invoker `_FUN`, not Clang's `__invoke`
struct S {
  int v;
  int run(int k) {
    auto f = [this, k](int x) { return v + k + x; };
    return f(1);
  }
};
inline int f() {
  auto a = [] { return 1; };
  auto b = [](int x) { return x; };
  auto c = [] { return 2; };
  auto d = [x = [] { return 7; }] { return x() + 1; };
  return a() + b(1) + c() + d();
}
template<class T> T g(T t) {
  auto l = [t](T u) { return t + u; };
  return l(t);
}
inline auto h() { return [](int x) { return x * 3; }; }
int peer_check();
int main() {
  S s{1};
  int (*p)(int) = h();
  if (s.run(2) != 4 || f() != 12 || g(3) != 6 || p(2) != 6) return 1;
  return peer_check();
}
