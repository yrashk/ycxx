// θ step 2 (#280), spec-only: a lambda with no return statement, or only `return;`, has return type
// void ([expr.prim.lambda.closure], [dcl.spec.auto.general], [stmt.return]).
int g;

int main() {
  auto set = [](int& r, int v) { r = v; };
  int x = 0;
  set(x, 9);
  if (x != 9) return 1;
  auto early = [](int& r) {
    if (r > 5) return;
    r = 100;
  };
  early(x);
  if (x != 9) return 2;
  int z = 1;
  early(z);
  if (z != 100) return 3;
  auto nothing = [] {};
  nothing();
  auto touch = [] { g = 5; return; };
  touch();
  return g == 5 ? 0 : 4;
}
