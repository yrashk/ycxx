// θ step 3 (#281), spec-only: each `auto` parameter of a generic lambda invents its own template
// parameter, so the two arguments may have different types and are deduced independently
// ([expr.prim.lambda.closure], [dcl.fct], [temp.deduct.call], [expr.arith.conv]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto add = [](auto a, auto b) { return a + b; };
  char c = 2;
  long l = 100;
  if (add(1, 2) != 3) return 1;
  if (add(1, c) != 3) return 2;
  if (add(l, 1) != 101) return 3;
  if (add(c, c) != 4) return 4;
  if (!same<decltype(add(c, c)), int>::value) return 5;
  if (!same<decltype(add(l, 1)), long>::value) return 6;
  if (!same<decltype(add(1, l)), long>::value) return 7;
  int a[4] = {5, 6, 7, 8};
  if (*add(a, 2) != 7) return 8;
  if (!same<decltype(add(a, 2)), int*>::value) return 9;
  auto pick = [](auto a, auto b, auto c3) { return c3 ? a : b; };
  if (pick(1, 2, true) != 1 || pick(1, 2, 0) != 2) return 10;
  return 0;
}
