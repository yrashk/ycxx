// θ step 3 (#281), from the review: a generic lambda's specialization sees what its definition
// saw: the constant a name denotes there (not a shadowed one gone out of scope), a block-scope
// alias, a local named in an unevaluated operand, and an enclosing generic lambda's template
// parameter and parameter ([basic.scope.block], [expr.prim.lambda.closure]/7, [temp.inst]).
int main() {
  const int k = 1;
  { const int k = 5; (void)k; }
  auto g = [](auto x) { return x + k; };
  if (g(0) != 1) return 1;
  using I = long;
  auto f = [](auto x) { I y = x; return sizeof(y); };
  if (f(4) != sizeof(long)) return 2;
  int v = 3;
  long w = 1;
  auto u = [](auto x) { return sizeof(v) + sizeof(decltype(w)) + x; };
  if (u(0) != sizeof(int) + sizeof(long)) return 3;
  auto outer = []<class T>(T x) { return [x](auto y) { T z = y; return z + x; }; };
  if (outer(1)(2L) != 3) return 4;
  auto o2 = [](auto x) { return [](auto y) { return sizeof(x) + y; }; };
  if (o2('a')(1) != 2) return 5;
  return v + int(w) == 4 ? 0 : 6;
}
