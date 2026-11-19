// #308, from the review:
// - a trivial copy into a base class subobject writes only the base's data, not the tail padding
//   or empty-base offset a derived class's members use (Itanium §2.4 nvsize);
// - a reference to a base binds to the base subobject of a materialized derived prvalue, whose
//   lifetime it extends ([dcl.init.ref]/5.3, [class.temporary]/6);
// - types and enumerators of a base are found from the derived class; a friend of a derived class
//   names a protected member of its base ([class.access.base]/5);
// - `?:` and `==` convert a derived class's glvalue or pointer to its base's ([expr.cond]/4,
//   [expr.type]/4.6).
// STDOUT: 42 7 153 2 7 | 7 ~C ~B | 9 | ~C ~B ~C ~B | 0
extern "C" int printf(const char*, ...);
struct E {};
struct Z : E { char x; };
struct P { int i; };
struct D : P, E { D(const E& e) : P{7}, E(e) {} };
struct D3 : P, E { int j; };
struct NP { int i; char c; NP() {} };
struct Y : NP { char d; };
struct A { int a = 1; };
struct B { int b; ~B() { printf("~B "); } };
struct C : A, B { C(int v) { b = v; } ~C() { printf("~C "); } };
int rd(const B& b) { return b.b; }
C make() { return C(9); }
struct S { using T = int; enum En { e1 = 5 }; };
struct W : S { T x = 3; En k; int h() { T t = 2; k = e1; return t + x; } };
W::T g = 4;
struct Q { protected: int prot = 6; };
struct R : Q { friend int fr(R&); };
int fr(R& r) { return r.prot; }
struct M1 { long m = 5; };
struct MM : M1, A { int x = 3; };
int mixed(int n) {
  W w;
  R r;
  A a;
  C c(1);
  MM mm;
  A& ra = n > 5 ? a : mm;
  A* pa = n > 5 ? &a : &mm;
  A* p2 = &mm;
  int same = p2 == &mm && &mm == p2;
  return w.h() + g + w.k + fr(r) + ra.a + pa->a + same - 23;
}
int main() {
  Z z;
  z.x = 42;
  E t;
  static_cast<E&>(z) = t;
  E e;
  D d(e);
  D3 d3{{0x99}, e, 2};
  Y y;
  y.d = 7;
  NP& r = y;
  NP o;
  o.i = 1;
  r = o;
  printf("%d %d %d %d %d | ", z.x, d.i, d3.i, d3.j, y.d);
  printf("%d ", rd(C(7)));
  printf("| ");
  {
    const B& tb = make();
    printf("%d | ", tb.b);
  }
  printf("| %d\n", mixed(1));
  return 0;
}
