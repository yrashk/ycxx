// θ step 3 (#281), spec-only: a lambda that captures constexpr locals by copy, stored in a constexpr
// closure object, is called in constant expressions; its copied members are constant
// ([expr.prim.lambda.capture], [expr.prim.lambda.closure], [expr.const]).
// XFAIL: ycxx class objects in constant evaluation, #262
int main() {
  constexpr int k = 5;
  constexpr int scale = 3;
  constexpr auto f = [k](int x) { return x + k; };
  static_assert(f(1) == 6);
  constexpr auto g = [k, scale](auto x) { return x * scale + k; };
  static_assert(g(2) == 11);
  static_assert(g(2L) == 11L);
  constexpr auto h = [=](int x) { return x - k * scale; };
  static_assert(h(15) == 0);
  constexpr auto init = [m = k * 2](int x) { return m + x; };
  static_assert(init(0) == 10);
  return f(-6) + 1;
}
