// #306 [class.nest]/3, [class.mfct]/1, [class.static.data]: members of a nested class are
// defined at namespace scope with a qualified name.
struct O {
  struct I {
    I(int v);
    int f() const;
    static int g;
    int v;
  };
};

O::I::I(int v) : v(v) {}
int O::I::f() const { return g + v; }
int O::I::g = 4;

int main() {
  O::I i(1);
  return i.f() - 5;
}
