// θ step 2 (#280), spec-only: a lambda inside a lambda; the inner one captures through the outer one,
// which captures implicitly because of its capture-default ([expr.prim.lambda.capture]).
int main() {
  int x = 1, y = 2;
  auto outer = [&] {
    auto inner = [&] { x += 10; return y; };
    return inner();
  };
  if (outer() != 2 || x != 11) return 1;
  auto outer2 = [=] {
    auto inner = [y] { return y * 100; };
    return inner() + x;
  };
  if (outer2() != 211) return 2;
  auto curry = [](int a) { return [a](int b) { return a - b; }; };
  auto minus = curry(10);
  if (minus(3) != 7) return 3;
  auto deep = [&] { return [&] { return [&] { return x + y; }(); }(); };
  if (deep() != 13) return 4;
  return 0;
}
