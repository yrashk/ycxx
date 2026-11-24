// #306 [class.mem.general]/8, [class.default.ctor], [class.copy.ctor], [class.copy.assign]: the
// enclosing class's implicit special members construct, copy and assign a field whose type is a
// nested class, which is complete there (review of #306: the field was left unconstructed).
struct B { virtual int f() const { return 1; } };

struct O {
  struct P { virtual int f() const { return 7; } };
  P p;
};

struct C {
  struct I { int v; I(const I& o) : v(o.v + 1) {} I() : v(1) {} };
  I a;
};

struct A {
  struct I { int v = 1; I& operator=(const I& o) { v = o.v + 10; return *this; } };
  I a;
};

struct D {
  struct I : B { int k; I(int x) : k(x) {} int f() const override { return k; } };
  I a{7};
};

int call(const B& b) { return b.f(); }

int main() {
  O o;
  C c;
  C c2 = c;
  A a, a2;
  a2 = a;
  D d;
  D d2 = d;
  return (o.p.f() == 7 && c2.a.v == 2 && a2.a.v == 11 && call(d2.a) == 7) ? 0 : 1;
}
