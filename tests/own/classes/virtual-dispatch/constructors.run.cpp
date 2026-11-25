// #309 [class.default.ctor], [class.base.init]: implicit default constructors with default member
// initializers and user-provided constructors with an explicit base initializer leave a
// polymorphic object that dispatches to its own class.
// EXIT: 0

int trace;

struct B {
  int x = 7;
  int y;
  B() : y(1) { trace = trace * 10 + 1; }
  B(int v) : y(v) { trace = trace * 10 + 2; }
  virtual int get() { return x + y; }
};

struct D : B {
  int z;
  D() : B(3), z(40) { trace = trace * 10 + 3; }
  D(int v) : z(v) { trace = trace * 10 + 4; }
  int get() override { return x + y + z; }
};

// Implicit default constructors throughout.
struct P {
  int a = 5;
  virtual int get() { return a; }
};

struct Q : P {
  int b = 6;
  int get() override { return a * 10 + b; }
};

int read(B& b) { return b.get(); }
int read(P& p) { return p.get(); }

int main() {
  D d;
  if (trace != 23) return 1;
  if (read(d) != 7 + 3 + 40) return 2;
  trace = 0;
  D e(100);
  if (trace != 14) return 3;
  if (read(e) != 7 + 1 + 100) return 4;
  B b;
  if (read(b) != 8) return 5;
  Q q;
  if (read(q) != 56) return 6;
  P p;
  if (read(p) != 5) return 7;
  return 0;
}
