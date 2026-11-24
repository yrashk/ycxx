// #309 [class.virtual]: a call through a pointer or reference to base calls the final overrider;
// overriders at several levels of a chain, and slots a derived class inherits without overriding.
// EXIT: 0

struct A {
  virtual int f() { return 1; }
  virtual int g() { return 10; }
  virtual int h() { return 100; }
};

struct B : A {
  int f() override { return 2; }
};

struct C : B {
  int g() override { return 30; }
};

struct D : C {
  virtual int f() { return 4; }
  int h() { return 400; }
};

int via_pointer(A* a) { return a->f() + a->g() + a->h(); }
int via_reference(A& a) { return a.f() + a.g() + a.h(); }
int via_middle(B& b) { return b.f() + b.g() + b.h(); }

int main() {
  A a;
  B b;
  C c;
  D d;
  if (via_pointer(&a) != 111) return 1;
  if (via_pointer(&b) != 112) return 2;
  if (via_pointer(&c) != 132) return 3;
  if (via_pointer(&d) != 434) return 4;
  if (via_reference(a) != 111) return 5;
  if (via_reference(b) != 112) return 6;
  if (via_reference(c) != 132) return 7;
  if (via_reference(d) != 434) return 8;
  if (via_middle(c) != 132) return 9;
  if (via_middle(d) != 434) return 10;
  C* pc = &d;
  if (pc->f() != 4 || pc->g() != 30 || pc->h() != 400) return 11;
  // A direct call on the object itself names the object's own class.
  if (d.f() != 4 || c.f() != 2 || b.g() != 10) return 12;
  return 0;
}
