// θ step 2 (#280), from the review: a copy captured through an enclosing lambda has the
// entity's type, not the const one the enclosing operator sees ([expr.prim.lambda.capture]/10).
int main() {
  int x = 1;
  auto f = [=] { return [=]() mutable { return ++x; }(); };
  auto g = [x] { return [x]() mutable { x += 10; return x; }(); };
  auto h = [y = 1] { return [y]() mutable { ++y; return y; }(); };
  if (f() != 2 || g() != 11 || h() != 2 || x != 1) return 1;
  return 0;
}
