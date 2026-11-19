// θ step 3 (#281), spec-only: an immediately invoked lambda initializes a constexpr local variable, and a
// constexpr closure object is called in later constant expressions such as an array bound and a
// template argument ([expr.prim.lambda.closure], [dcl.constexpr], [expr.const]).
// XFAIL: ycxx class objects in constant evaluation, #262
template<int N> struct tag { static constexpr int value = N; };

int main() {
  constexpr int v = [](int x) { return x * 2; }(3);
  static_assert(v == 6);
  constexpr auto sq = [](int x) { return x * x; };
  static_assert(sq(5) == 25);
  int arr[sq(3)] = {};
  if (sizeof(arr) != 9 * sizeof(int)) return 1;
  if (tag<sq(4)>::value != 16) return 2;
  constexpr auto twice = [](auto x) { return x + x; };
  constexpr long big = twice(100000L);
  static_assert(big == 200000L);
  if (v != 6) return 3;
  if (sq(v) != 36) return 4;
  return 0;
}
