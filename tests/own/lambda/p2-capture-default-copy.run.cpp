// θ step 2 (#280), spec-only: the capture-default `=` implicitly captures by copy each local entity the
// compound-statement odr-uses ([expr.prim.lambda.capture]).
int main() {
  int x = 3, y = 4, unused = 9;
  auto f = [=] { return x * 10 + y; };
  x = 0;
  y = 0;
  if (f() != 34) return 1;
  auto g = [=](int k) { return k + x + y + unused; };
  if (g(1) != 10) return 2;
  return 0;
}
