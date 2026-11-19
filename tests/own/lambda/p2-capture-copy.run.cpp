// θ step 2 (#280), spec-only: a simple-capture without & captures by copy: the closure holds its own
// copy, initialized when the lambda-expression is evaluated, and later changes to the variable are not
// seen ([expr.prim.lambda.capture]).
int main() {
  int x = 5;
  auto get = [x] { return x; };
  x = 50;
  if (get() != 5) return 1;
  int a = 1, b = 2;
  auto sum = [a, b] { return a * 10 + b; };
  a = 7;
  b = 8;
  if (sum() != 12) return 2;
  auto now = [x] { return x; };
  if (now() != 50) return 3;
  return 0;
}
