// #116, from the review: a call's class result assigned to an object is a temporary destroyed at
// the end of the full-expression ([expr.assign]/9, [class.temporary]/4); a class prvalue directly
// initializes an object, also in parentheses and in a constructor's argument
// ([dcl.init.general]/17.6.1, [expr.call]/7).
// STDOUT: T1 T5 ~T5 | T3 | T7 ~T7 | 8
// STDOUT: ~T3 ~T5 |
// STDOUT: ~U5 | ~U9 | 9
// STDOUT: ~U0 |
extern "C" int printf(const char*, ...);
struct T {
  int v;
  T(int x) : v(x) { printf("T%d ", v); }
  T(const T& o) : v(o.v + 100) { printf("Tc%d ", v); }
  ~T() { printf("~T%d ", v); }
  T& operator=(const T& o) { v = o.v; return *this; }
};
T mkT(int v) { return T(v); }
struct K { long v; K(T t, int z) : v(t.v + z) {} };
struct U { int v; ~U() { printf("~U%d ", v); } };
U mkU(int v) { return U{v}; }
struct G { U gu() const { return U{9}; } };
int run_t() {
  T t(1);
  t = mkT(5);
  printf("| ");
  T u(mkT(3));
  printf("| ");
  K k(mkT(7), 1);
  printf("| %ld\n", k.v);
  return 0;
}
int run_u() {
  U u{1};
  u = mkU(5);
  printf("| ");
  G g;
  u = g.gu();
  printf("| %d\n", u.v);
  u.v = 0;
  return 0;
}
int main() {
  run_t();
  printf("|\n");
  run_u();
  printf("|\n");
  return 0;
}
