// #1400: pack expansions in a member's parenthesized mem-initializer expression-list
// ([class.base.init], [temp.variadic]): a forwarding constructor template keeping value
// categories, an expansion beside other arguments, a pattern that is a larger expression, and a
// scalar member initialized from a one-element pack.
// EXIT: 0
struct MoveOnly {
  int v;
  MoveOnly(int x) : v(x) {}
  MoveOnly(MoveOnly&& o) : v(o.v) { o.v = 0; }
  MoveOnly(const MoveOnly&) = delete;
};
struct Holder {
  int moved;
  int& ref;
  Holder(MoveOnly m, int& r) : moved(m.v), ref(r) {}
};
template<class T> struct Box {
  T value;
  template<class... A> Box(A&&... args) : value(static_cast<A&&>(args)...) {}
};

struct Three { int a, b, c, d; Three(int w, int x, int y, int z) : a(w), b(x), c(y), d(z) {} };
template<class... A> struct Around {
  Three m;
  Around(A... args) : m(1, args..., 2) {}
};
template<class... A> struct Doubled {
  Three m;
  Doubled(A... args) : m((args * 2)...) {}
};
template<class... A> struct Scalar {
  long value;
  Scalar(A... args) : value(args...) {}
};

int main() {
  int target = 5;
  MoveOnly source(40);
  Box<Holder> box(static_cast<MoveOnly&&>(source), target);
  if (box.value.moved != 40 || source.v != 0) return 1;
  box.value.ref = 9;
  if (target != 9) return 2;

  Around<int, int> around(7, 8);
  if (around.m.a != 1 || around.m.b != 7 || around.m.c != 8 || around.m.d != 2) return 3;

  Doubled<int, int, int, int> doubled(1, 2, 3, 4);
  if (doubled.m.a != 2 || doubled.m.b != 4 || doubled.m.c != 6 || doubled.m.d != 8) return 4;

  Scalar<int> scalar(42);
  if (scalar.value != 42) return 5;
  Box<int> boxed(11);
  if (boxed.value != 11) return 6;
  return 0;
}
