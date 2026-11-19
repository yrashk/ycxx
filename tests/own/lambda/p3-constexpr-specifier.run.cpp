// θ step 3 (#281), spec-only: the constexpr specifier in a lambda-declarator makes the function call
// operator constexpr explicitly, for an ordinary and a generic lambda, with or without mutable
// ([expr.prim.lambda.general], [expr.prim.lambda.closure], [dcl.constexpr], [expr.const]).
// XFAIL: ycxx class objects in constant evaluation, #262
int main() {
  constexpr auto cube = [](int x) constexpr { return x * x * x; };
  static_assert(cube(3) == 27);
  static_assert([]() constexpr { return 11; }() == 11);
  constexpr auto neg = [](auto x) constexpr -> decltype(x) { return -x; };
  static_assert(neg(4) == -4);
  static_assert(neg(4L) == -4L);
  static_assert([n = 2]() mutable constexpr { n *= 5; return n; }() == 10);
  static_assert([n = 2]() constexpr mutable { return ++n; }() == 3);
  if (cube(2) != 8) return 1;
  int runtime = 5;
  if (neg(runtime) != -5) return 2;
  return 0;
}
