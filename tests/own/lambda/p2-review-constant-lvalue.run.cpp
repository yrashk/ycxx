// θ step 2 (#280), from the review: a constant named in a lambda that can capture it is the
// captured object where an lvalue is needed (its address, a reference to it), not a temporary
// ([basic.def.odr]/5, [expr.prim.lambda.capture]/8).
const int* addr(const int& r) { return &r; }
int main() {
  const int x = 5;
  auto g = [&] { return addr(x); };
  if (g() != &x) return 1;
  auto h = [&]() -> const int& { return x; };
  if (&h() != &x) return 2;
  auto p = [&] { return &x; };
  if (p() != &x) return 3;
  auto q = [&] { return [&x] { return x; }(); };
  if (q() != 5) return 4;
  auto v = [] { return x + 1; };  // no capture: its value
  return v() == 6 ? 0 : 5;
}
