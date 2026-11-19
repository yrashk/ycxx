// θ step 3 (#281), spec-only: a generic lambda may mix `auto` parameters with ordinary ones; the
// ordinary parameters convert their arguments as for any function
// ([expr.prim.lambda.closure], [dcl.fct], [over.match.viable]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto scale = [](int n, auto x) { return n * x; };
  if (scale(3, 4) != 12) return 1;
  if (scale('\2', 5L) != 10L) return 2;
  if (!same<decltype(scale(1, 5L)), long>::value) return 3;
  return 0;
}
