// #117, from the review: local classes of the same shape in a function and a function template
// are distinct items; a block's using-directive and namespace alias reach the members; a class
// nested in a local class is complete in the block; `struct A;` declares a local class that a
// later definition in the block completes ([class.local], [namespace.udir], [namespace.alias],
// [dcl.type.elab]).
namespace N {
int h(int) { return 8; }
int w = 4;
}  // namespace N
int h(long) { return 1; }

int plain() {
  struct A {
    int g() { return 2; }
  };
  return A{}.g();
}
template <class T> int templ() {
  { struct A { int g() { return 1; } }; if (A{}.g() != 1) return 100; }
  { struct A { int g() { return 3; } }; return A{}.g(); }
}

int directive() {
  using namespace N;
  namespace M = N;
  struct L {
    int g() { return h(0); }  // N::h(int) is the better match
    int v() { return M::w; }
  };
  return L{}.g() * 10 + L{}.v();
}

int nested() {
  struct B {
    struct In {
      int a;
    };
  };
  B::In x{3};
  return x.a;
}

int declared_first() {
  struct A;
  A* p = nullptr;
  struct A {
    int v;
  };
  A a{5};
  p = &a;
  return p->v;
}

int main() {
  if (plain() != 2 || templ<int>() != 3) return 1;
  if (directive() != 84) return 2;
  if (nested() != 3) return 3;
  if (declared_first() != 5) return 4;
  return 0;
}
