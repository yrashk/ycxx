// θ step 3 (#281), spec-only (split from p3-generic-mixed-params): an ordinary parameter of a
// generic lambda with a default argument ([expr.prim.lambda.closure], [dcl.fct.default]).
// XFAIL: ycxx default arguments, #300
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto middle = [](char tag, auto x, int k = 10) { return tag + x + k; };
  if (middle('\1', 2) != 13) return 4;
  if (middle('\1', 2, 0) != 3) return 5;
  if (middle(1L, 2L, 3L) != 6) return 6;
  if (!same<decltype(middle('\0', 1L)), long>::value) return 7;
  return 0;
}
