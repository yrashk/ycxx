// θ step 2 (#280), spec-only: each lambda-expression has its own unique closure type, even when two are
// written alike; several lambdas live side by side in one function ([expr.prim.lambda.closure]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto a = [](int x) { return x + 1; };
  auto b = [](int x) { return x + 1; };
  auto c = a;
  static_assert(!same<decltype(a), decltype(b)>::value);
  static_assert(same<decltype(a), decltype(c)>::value);
  int v = 2;
  auto d = [v] { return v; };
  auto e = [&v] { return v * 2; };
  v = 5;
  if (a(1) != 2 || b(2) != 3 || c(3) != 4) return 1;
  if (d() != 2 || e() != 10) return 2;
  return 0;
}
