// #308: members found through bases ([class.member.lookup]), in member functions too, and the
// derived-to-base conversions of pointers ([conv.ptr]/3; a null pointer stays null), references
// ([dcl.init.ref]) and objects (a copy of the base class subobject), with overload resolution
// preferring the nearest base ([over.ics.rank]/4.4).
// STDOUT: 27 2 2 -1 1 20 7 2 1
extern "C" int printf(const char*, ...);
struct A { int a; int geta() const { return a; } };
struct B { int b; int getb() const { return b * 10; } };
struct D : A, B { int d; int all() const { return a + b + d + geta() + getb(); } };
struct E : D {};
int byptr(B* p) { return p ? p->b : -1; }
int pick(const A&) { return 1; }
int pick(const D&) { return 2; }
int main() {
  D d;
  d.a = 1;
  d.b = 2;
  d.d = 3;
  B* pb = &d;
  D* none = nullptr;
  B* pn = none;
  A copy = d;
  E e;
  e.a = 7;
  A& ra = e;
  printf("%d %d %d %d %d %d %d %d %d\n", d.all(), pb->b, byptr(&d), byptr(pn), copy.a, d.getb(), ra.a, pick(e), pick(copy));
  return 0;
}
