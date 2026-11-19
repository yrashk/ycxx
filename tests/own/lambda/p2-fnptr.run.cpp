// θ step 2 (#280), spec-only: the closure type of a lambda without a lambda-capture has a conversion
// function to pointer to function with the same parameter and return types; calling the pointer has the
// effect of calling the closure ([expr.prim.lambda.closure], [conv]).
int main() {
  int (*f)(int) = [](int x) { return x * x; };
  if (f(7) != 49) return 1;
  long (*g)(int, int) = [](int a, int b) -> long { return a + b; };
  if (g(2, 3) != 5) return 2;
  void (*h)(int&) = [](int& r) { r = 8; };
  int v = 0;
  h(v);
  if (v != 8) return 3;
  auto l = [] { return 11; };
  int (*p)() = l;
  if (p() != 11) return 4;
  p = [] { return 12; };
  if (p() != 12) return 5;
  return 0;
}
