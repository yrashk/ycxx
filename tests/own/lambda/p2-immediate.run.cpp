// θ step 2 (#280), spec-only: a lambda-expression is a prvalue closure object that can be called right
// where it appears ([expr.prim.lambda.general], [expr.prim.lambda.closure], [expr.call]).
int main() {
  if ([] { return 7; }() != 7) return 1;
  if ([](int a) { return a * 3; }(4) != 12) return 2;
  if ([](int a, int b) { return a - b; }(10, 3) != 7) return 3;
  int r = [] () { return 40; }() + 2;
  if (r != 42) return 4;
  return 0;
}
