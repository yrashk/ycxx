// #306 [class.nest], [basic.lookup.qual]: a nested class names its still incomplete enclosing
// class's earlier members with a qualified name.
struct O {
  struct A { int v = 4; };
  static const int k = 3;
  enum E { e = 2 };
  struct I {
    O::A a;
    int arr[O::k];
    int f() const { return a.v + sizeof(arr) / sizeof(int) + O::e; }
  };
};

int main() {
  O::I i;
  return i.f() - 9;
}
