// θ step 3 (#281), spec-only: a generic lambda recurses by calling the closure object passed to it as an
// `auto` parameter; the explicit return type lets the recursive call be used before the return type
// would otherwise be deduced ([expr.prim.lambda.closure], [dcl.spec.auto.general], [temp.deduct.call]).
int main() {
  auto fact = [](auto self, int n) -> int { return n < 2 ? 1 : n * self(self, n - 1); };
  if (fact(fact, 5) != 120) return 1;
  if (fact(fact, 1) != 1) return 2;
  auto fib = [](auto& self, int n) -> long { return n < 2 ? n : self(self, n - 1) + self(self, n - 2); };
  if (fib(fib, 10) != 55) return 3;
  auto sum = [](const auto& self, int n) {
    if (n == 0) return 0;
    return n + self(self, n - 1);
  };
  if (sum(sum, 4) != 10) return 4;
  return 0;
}
