// #309 [dcl.init.general] (value-initialization), [class.default.ctor]: `T()` and `T{}` of a
// polymorphic class without a user-provided constructor zero-initialize members without an
// initializer, and the object still dispatches to its class.
// EXIT: 0

struct B {
  int a;
  int b = 7;
  virtual int sum() { return a + b; }
};

struct D : B {
  int c;
  int sum() override { return a + b + c + 100; }
};

int call(B& x) { return x.sum(); }

void scribble() {
  volatile int junk[32];
  for (int i = 0; i < 32; ++i) junk[i] = 0x5a5a + i;
}

int main() {
  scribble();
  B b1 = B();
  if (b1.a != 0 || b1.b != 7) return 1;
  scribble();
  B b2 = B{};
  if (b2.a != 0 || call(b2) != 7) return 2;
  scribble();
  D d1 = D();
  if (d1.a != 0 || d1.c != 0 || d1.b != 7) return 3;
  if (call(d1) != 107) return 4;
  scribble();
  D d2{};
  if (d2.a != 0 || d2.c != 0) return 5;
  if (call(d2) != 107) return 6;
  if (B().sum() != 7) return 7;
  if (D{}.sum() != 107) return 8;
  return 0;
}
