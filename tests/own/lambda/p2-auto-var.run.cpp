// θ step 2 (#280), spec-only: a closure object stored in an `auto` variable, called several times and
// copied; a copy calls the same function call operator ([expr.prim.lambda.closure], [dcl.spec.auto]).
int main() {
  auto twice = [](int v) { return v * 2; };
  if (twice(1) != 2) return 1;
  if (twice(21) != 42) return 2;
  auto again = twice;
  if (again(5) != 10) return 3;
  auto const& ref = twice;
  if (ref(-3) != -6) return 4;
  return 0;
}
