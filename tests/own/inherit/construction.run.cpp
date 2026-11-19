// #308: bases are initialized first, in declaration order, from mem-initializers or by their
// default constructors, then the members; the destructor runs, then the members' and the bases'
// in reverse ([class.base.init]/13, [class.dtor]/13). An implicit copy constructor copies the
// bases, and a base class object is copied from a derived one's subobject.
// STDOUT: A7 M2 M5 D Mc12 | 21 7 12 |~M12 ~D ~M5 ~M2 ~A |
extern "C" int printf(const char*, ...);
struct M { int v; M(int x) : v(x) { printf("M%d ", v); } ~M() { printf("~M%d ", v); } M(const M& o) : v(o.v + 10) { printf("Mc%d ", v); } };
struct A { int a; A() : a(1) { printf("A "); } A(int x) : a(x) { printf("A%d ", a); } ~A() { printf("~A "); } };
struct B { M m; int b = 3; B() : m(2) {} };
struct D : A, B { M n; D() : A(7), n(5) { printf("D "); } ~D() { printf("~D "); } int sum() const { return a + b + m.v + n.v; } };
struct E : D { int e = 4; };
int f(const A& a) { return a.a; }
int g(B b) { return b.m.v; }
int main() {
  {
    E e;
    printf("| %d %d %d |", e.sum() + e.e, f(e), g(e));
  }
  printf("|\n");
  return 0;
}
