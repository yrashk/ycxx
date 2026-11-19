// θ step 3 (#281): the specializations of generic lambdas' function call operators have the
// Itanium names Clang gives them: the closure's <lambda-sig> with the invented parameters as
// `T_` and the explicit ones declared (`Ty`), then the operator's <template-args> and its
// declared return type `Da` (docs/design/closures.md). The peer, compiled by Clang, calls them.
// PEER: p3-impl-mangle-peer.cpp
inline int k() {
  auto a = [](auto x) { return x + 1; };
  auto b = []<class T>(T x, int y) { return x * y; };
  int z = 3;
  auto c = [](auto x, auto& y) { return x + y; };
  return a(1) + int(b(2L, 3)) + c(1, z) + int(a(2L));
}
inline long packs() {
  auto s = [](auto... xs) { return (xs + ... + 0); };
  auto c = []<class... Ts>(Ts... xs) { return sizeof...(xs); };
  auto m = [](int a, auto... rest) { return a * (1 + ... + rest); };
  return s(1, 2L) + long(c(1, 'a')) + m(2, 3);
}
template<class T> int t(T v) {
  auto g = [v](auto x) { return x + v; };
  return g(1);
}
inline int heads() {
  auto d = [](auto x) { return x; };
  auto e = []<class T>(T x) { return x; };
  auto f = [](auto x) { return x + 1; };
  auto u = []<class = int>(int x) { return x; };
  return d(1) + e(1) + f(1) + u(1);
}
int peer_check();
int main() {
  if (k() != 15 || t(2) != 3 || packs() != 13 || heads() != 5) return 1;
  return peer_check();
}
