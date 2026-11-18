// δ s2 (#86), from the step's review: a parenthesized member function name ([expr.prim.paren]);
// an in-class constant read through an object and through `this`, no odr-use
// ([basic.def.odr]); a static member function called through an object, whose object expression
// is still evaluated ([expr.ref]); class-scope names in member declarations and in the
// initializer of a static member's definition ([basic.scope.class]); redeclarations of members
// initialized in the class ([class.static.data], [depr.static.constexpr]); the address of a
// static member function ([expr.prim.id.qual]).
// EXIT: 0
int calls = 0;
struct S {
  int v;
  static const int x = 5;
  static constexpr int n = 3;
  int a[n];
  static const int lim = 4;
  static int count;
  int f() { return v; }
  int g() { return x + S::x + this->x; }
  static int sf() { return 1; }
};
int S::count = lim + 1;
const int S::x;
constexpr int S::n;
S g{2, {1, 2, 3}};
S* get() { ++calls; return &g; }

int main() {
  S s{3, {1, 2, 3}};
  if ((s.f)() != 3) return 1;
  if (s.x != 5 || s.g() != 15) return 2;
  int r = get()->sf();
  if (r != 1 || calls != 1) return 3;
  if (S::count != 5) return 4;
  int (*fp)() = S::sf;
  if (fp != &S::sf || fp == nullptr) return 5;
  return 0;
}
