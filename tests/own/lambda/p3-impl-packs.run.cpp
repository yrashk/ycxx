// θ step 3 (#281): lambda parameter packs (`auto... xs`, `[]<class... Ts>(Ts... xs)`, after
// other parameters) and pack captures `[xs...]`, `[&xs...]`, and a pack named under a
// capture-default ([dcl.fct]/23, [expr.prim.lambda.closure]/7, [expr.prim.lambda.capture]/17).
// STDOUT: 6 0 6 16 118
extern "C" int printf(const char*, ...);
template<class... Ts> int sum_all(Ts... xs) {
  auto f = [xs...] { return (xs + ... + 0); };
  auto g = [&] { return (xs * ... * 1); };
  auto h = [&xs...](int k) { return (k + ... + xs); };
  return f() + g() + h(100);
}
int main() {
  auto s = [](auto... xs) { return (xs + ... + 0); };
  auto c = []<class... Ts>(Ts... xs) { return sizeof...(Ts) + sizeof...(xs); };
  auto mixed = [](int a, auto... rest) { return a * (1 + ... + rest); };
  printf("%d %d %ld %d %d\n", s(1, 2, 3), s(), c(1, 'a', 2L), mixed(2, 3, 4), sum_all(1, 2, 3));
  return 0;
}
