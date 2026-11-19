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
template<class T> int t(T v) {
  auto g = [v](auto x) { return x + v; };
  return g(1);
}
int peer_check();
int main() {
  if (k() != 15 || t(2) != 3) return 1;
  return peer_check();
}
