// ι (#300): a constructor whose every parameter has a default argument is a default constructor:
// it initializes `S s;`, `S s{};`, `S()` and the elements of an array; with one argument it is a
// converting constructor used in copy-initialization and for a function argument
// ([class.default.ctor]/1, [class.conv.ctor]/1, [dcl.init.general], [dcl.fct.default]/1).
// EXIT: 0
struct Point {
  int x, y;
  Point(int x = 1, int y = 2) : x(x), y(y) {}
};

struct Wrap {
  Point p;  // default-initialized by Point's default constructor
  int tag;
  Wrap(int tag = 9) : tag(tag) {}
};

int sum(Point p) { return p.x + p.y; }

int main() {
  Point a;
  if (a.x != 1 || a.y != 2) return 1;
  Point b{};
  if (b.x != 1 || b.y != 2) return 2;
  if (Point().y != 2) return 3;
  Point c(5);
  if (c.x != 5 || c.y != 2) return 4;
  Point d = 7;  // the converting constructor, the second argument from its default
  if (d.x != 7 || d.y != 2) return 5;
  if (sum(4) != 6) return 6;
  Point e(3, 4);
  if (e.x != 3 || e.y != 4) return 7;
  Point arr[3];
  if (arr[2].x != 1 || arr[2].y != 2) return 8;
  Wrap w;
  if (w.tag != 9 || w.p.x != 1 || w.p.y != 2) return 9;
  Wrap v(4);
  if (v.tag != 4) return 10;
  return 0;
}
