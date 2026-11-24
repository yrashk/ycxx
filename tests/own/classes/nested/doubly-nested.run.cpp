// #306 [class.nest]: classes nest to any depth, and each sees the members of all enclosing ones.
struct A {
  static constexpr int a = 1;
  struct B {
    static constexpr int b = 2;
    struct C {
      int c = 3;
      int sum() const { return a + b + c; }
    };
    C c;
  };
  B b;
};

int main() {
  A x;
  A::B::C c;
  return x.b.c.sum() + c.sum() - 12;
}
