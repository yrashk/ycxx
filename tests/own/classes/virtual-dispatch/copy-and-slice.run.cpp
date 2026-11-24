// #309 [class.copy.ctor], [class.virtual]: the implicit copy constructor of a polymorphic class
// gives an object of the copied-to type; copying a D gives a D, and initializing a B from a D
// slices to a B whose virtual calls go to B.
// EXIT: 0

struct B {
  int v = 1;
  virtual int who() { return 10 + v; }
};

struct D : B {
  int w = 2;
  int who() override { return 20 + v + w; }
};

struct E : D {
  int who() override { return 30 + v + w; }
};

int call(B& b) { return b.who(); }

int main() {
  D d;
  d.v = 4;
  d.w = 5;
  D copy = d;
  if (call(copy) != 29) return 1;
  B sliced = d;
  if (call(sliced) != 14) return 2;
  if (sliced.who() != 14) return 3;
  B& ref = d;
  B fromref(ref);
  if (call(fromref) != 14) return 4;
  E e;
  D mid = e;
  if (call(mid) != 23) return 5;
  E ecopy(e);
  if (call(ecopy) != 33) return 6;
  return 0;
}
