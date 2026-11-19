// θ step 1 (#279), spec-only: `auto&& [a, b]` binds to an lvalue by lvalue reference and extends a prvalue's
// lifetime; the names refer to the members ([dcl.struct.bind], [class.temporary]).
// XFAIL: ycxx structured bindings, #285
struct Q {
  int a;
  int b;
};
Q make() { return Q{5, 6}; }

int main() {
  Q q{1, 2};
  auto&& [a, b] = q;
  a = 10;
  if (q.a != 10) return 1;
  auto&& [c, d] = make();
  c += d;
  if (c != 11) return 2;
  return 0;
}
