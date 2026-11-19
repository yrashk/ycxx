// θ step 2 (#280), spec-only: the parameter-declaration-clause of a lambda-declarator: values,
// references, pointers, class objects ([expr.prim.lambda.general], [dcl.fct]).
struct P { int x; int y; };

int main() {
  auto add = [](int a, int b) { return a + b; };
  if (add(2, 3) != 5) return 1;
  auto bump = [](int& r) { r += 10; };
  int v = 1;
  bump(v);
  if (v != 11) return 2;
  auto deref = [](const int* p) { return *p; };
  if (deref(&v) != 11) return 3;
  auto sum = [](P p) { return p.x + p.y; };
  if (sum(P{4, 5}) != 9) return 4;
  auto swapxy = [](P& p) { int t = p.x; p.x = p.y; p.y = t; };
  P q{1, 2};
  swapxy(q);
  if (q.x != 2 || q.y != 1) return 5;
  auto none = []() { return 3; };
  if (none() != 3) return 7;
  return 0;
}
