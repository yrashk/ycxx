// #1417 [dcl.decl.general], [basic.scope.param], [expr.prim.req], [over.match.viable]: a
// trailing requires-clause of a member function is in the scope of the function's parameters
// and may name them in unevaluated operands (`sizeof`, a requires-expression).
struct Doubler { int twice() const { return 2; } };
struct Plain {};

template<class T>
struct Holder {
  int use(const T& p) requires requires { p.twice(); } { return p.twice() * 10; }
  int use(const T& p) requires (!requires { p.twice(); }) && (sizeof(p) > 0) { return 1; }

  static int width(T p) requires (sizeof(p) == 4) { return 4; }
  static int width(T p) requires (sizeof(p) != 4) { return 0; }
};

int main() {
  Holder<Doubler> d;
  Holder<Plain> p;
  if (d.use(Doubler{}) != 20) return 1;
  if (p.use(Plain{}) != 1) return 2;
  if (Holder<int>::width(3) != 4) return 3;
  if (Holder<char>::width('c') != 0) return 4;
  return 0;
}
