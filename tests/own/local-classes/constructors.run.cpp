// Local classes (#117), spec-only: a local class with user-provided constructors (with a
// mem-initializer-list and a delegating constructor) and objects copied from it
// ([class.local], [class.ctor], [class.base.init], [class.copy.ctor]).
int main() {
  struct V {
    int a;
    int b;
    V(int x, int y) : a(x), b(x + y) {}
    V(int x) : V(x, 100) {}
    V() : a(-1), b(-2) {}
  };
  V v(2, 3);
  if (v.a != 2 || v.b != 5) return 1;
  V w = 7;
  if (w.a != 7 || w.b != 107) return 2;
  V d;
  if (d.a != -1 || d.b != -2) return 3;
  V copy = v;
  copy.a = 9;
  if (copy.a != 9 || copy.b != 5 || v.a != 2) return 4;
  V braced{4, 4};
  if (braced.b != 8) return 5;
  return 0;
}
