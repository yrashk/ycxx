// θ step 3 (#281), spec-only: a lambda's function call operator is constexpr when it satisfies the
// requirements of a constexpr function, so calling a lambda inside a static_assert in a function body
// is a constant expression ([expr.prim.lambda.closure], [dcl.constexpr], [expr.const]).
// XFAIL: ycxx class objects in constant evaluation, #262
int main() {
  static_assert([](int x) { return x * x; }(7) == 49);
  static_assert([] { return 3; }() + 1 == 4);
  static_assert([](int a, int b) {
    int r = 1;
    for (int i = 0; i < b; ++i) r *= a;
    return r;
  }(2, 10) == 1024);
  static_assert([](auto x) { return sizeof(x); }('a') == 1);
  static_assert([](auto x) { return x + 1; }(41L) == 42L);
  return [](int x) { return x - 5; }(5);
}
