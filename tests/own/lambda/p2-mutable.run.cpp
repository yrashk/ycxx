// θ step 2 (#280), spec-only: `mutable` makes the function call operator non-const, so the copies held by
// the closure can change; the captured variable stays as it was, and each closure object keeps its own
// state ([expr.prim.lambda.closure], [expr.prim.lambda.capture]).
int main() {
  int n = 0;
  auto count = [n]() mutable { return ++n; };
  if (count() != 1) return 1;
  if (count() != 2) return 2;
  if (n != 0) return 3;
  auto other = count;
  if (other() != 3) return 4;
  if (count() != 3) return 5;
  int a = 5;
  auto all = [=]() mutable { a *= 2; return a; };
  if (all() != 10 || all() != 20) return 6;
  if (a != 5) return 7;
  return 0;
}
