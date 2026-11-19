// θ step 2 (#280), spec-only (split from p2-params): a default argument in a lambda's
// parameter-declaration-clause ([expr.prim.lambda.general], [dcl.fct.default]).
// XFAIL: ycxx default arguments, #300
int main() {
  auto def = [](int a, int b = 100) { return a + b; };
  if (def(1) != 101 || def(1, 2) != 3) return 6;
  return 0;
}
