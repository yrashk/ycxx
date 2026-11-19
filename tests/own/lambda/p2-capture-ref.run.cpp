// θ step 2 (#280), spec-only: a simple-capture `& identifier` captures by reference: the lambda sees later
// changes to the variable and its changes reach the variable, without `mutable`
// ([expr.prim.lambda.capture]).
int main() {
  int x = 5;
  auto get = [&x] { return x; };
  x = 50;
  if (get() != 50) return 1;
  auto inc = [&x] { ++x; };
  inc();
  inc();
  if (x != 52) return 2;
  int a = 1, b = 2;
  auto swap = [&a, &b] { int t = a; a = b; b = t; };
  swap();
  if (a != 2 || b != 1) return 3;
  return 0;
}
