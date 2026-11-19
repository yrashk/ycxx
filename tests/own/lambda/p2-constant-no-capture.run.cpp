// θ step 2 (#280), spec-only: a const integral variable with a constant initializer, or a constexpr
// variable, named in a lambda without odr-use need not be captured ([basic.def.odr],
// [expr.prim.lambda.capture], [expr.const]).
int main() {
  const int N = 5;
  constexpr int M = 7;
  constexpr long L = 9;
  auto f = [] { return N + M; };
  if (f() != 12) return 1;
  auto g = [](int x) { return x * N - M; };
  if (g(2) != 3) return 2;
  auto h = [] { long a[M] = {}; return sizeof(a) / sizeof(a[0]) + L; };
  if (h() != 16) return 3;
  return 0;
}
