// #309 [class.virtual], [expr.call]: a virtual call through implicit `this` dispatches to the
// final overrider; a qualified name (`Base::f()` with implicit `this`) suppresses virtual dispatch.
// EXIT: 0

struct Shape {
  virtual int sides() { return 0; }
  virtual int weight() { return 1; }
  int describe() { return sides() * 10 + weight(); }
  int base_sides() { return Shape::sides(); }
};

struct Triangle : Shape {
  int sides() override { return 3; }
  int both() { return Shape::sides() * 100 + sides(); }
};

struct Square : Triangle {
  int sides() override { return 4; }
  int weight() override { return 2 + Triangle::weight(); }
  int parent() { return Triangle::sides(); }
};

int main() {
  Shape s;
  Triangle t;
  Square q;
  if (s.describe() != 1) return 1;
  if (t.describe() != 31) return 2;
  if (q.describe() != 43) return 3;
  Shape* p = &q;
  if (p->sides() != 4) return 4;
  if (p->base_sides() != 0) return 6;
  Triangle& r = q;
  if (r.both() != 4) return 8;
  if (t.both() != 3) return 9;
  if (q.parent() != 3) return 10;
  if (p->weight() != 3) return 11;
  return 0;
}
