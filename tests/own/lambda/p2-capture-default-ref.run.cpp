// θ step 2 (#280), spec-only: the capture-default `&` implicitly captures by reference each local entity
// the compound-statement odr-uses ([expr.prim.lambda.capture]).
int main() {
  int x = 3, y = 4;
  auto f = [&] { return x * 10 + y; };
  x = 1;
  y = 2;
  if (f() != 12) return 1;
  auto set = [&](int v) { x = v; y = v + 1; };
  set(7);
  if (x != 7 || y != 8) return 2;
  return 0;
}
