// θ step 2 (#280), spec-only: a capture-default with explicit captures of the other kind: `[=, &y]` and
// `[&, x]` ([expr.prim.lambda.capture]).
int main() {
  int x = 1, y = 2;
  auto f = [=, &y] { y += x; return x; };
  x = 10;
  if (f() != 1 || y != 3) return 1;
  auto g = [&, x] { y = x; };
  x = 20;
  g();
  if (y != 10) return 2;
  int z = 0;
  auto h = [&, x, y] { z = x + y; };
  h();
  if (z != 30) return 3;
  return 0;
}
