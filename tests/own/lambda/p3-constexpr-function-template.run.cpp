// θ step 3 (#281), spec-only: a lambda passed to a constexpr function template and called there is
// evaluated in a constant expression, with captureless and capturing closures and a generic one
// ([expr.prim.lambda.closure], [dcl.constexpr], [expr.const], [temp.deduct.call]).
// XFAIL: ycxx class objects in constant evaluation, #346
template<class F> constexpr int apply(F f, int x) { return f(x); }
template<class F> constexpr int fold(F f, int init, int n) {
  for (int i = 1; i <= n; ++i) init = f(init, i);
  return init;
}

int main() {
  static_assert(apply([](int v) { return v + 1; }, 41) == 42);
  constexpr int r = fold([](int acc, int i) { return acc + i; }, 0, 10);
  static_assert(r == 55);
  static_assert(fold([](auto acc, auto i) { return acc * i; }, 1, 5) == 120);
  constexpr int k = 3;
  constexpr auto byK = [k](int v) { return v * k; };
  static_assert(apply(byK, 7) == 21);
  constexpr int m = apply([](int v) { return v - 1; }, 1);
  return m;
}
