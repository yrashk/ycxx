// δ s4 (#86): namespaces holding functions, variables and structs, qualified names, nested
// namespaces with `namespace a::b {}`, reopening a namespace, out-of-namespace definitions by a
// qualified name, `::x` for the global one, and a namespace member hiding a global
// ([basic.namespace], [namespace.def], [basic.lookup.qual], [namespace.memdef]).
// EXIT: 0
int value = 1;

namespace geo {
  int value = 2;
  struct Point { int x, y; };
  int manhattan(const Point& a, const Point& b);
  int global_value() { return ::value; }
  int own_value() { return value; }
}

namespace geo::shapes {
  struct Square { geo::Point corner; int side; };
  int area(const Square& s) { return s.side * s.side; }
}

namespace geo {  // reopened
  int abs(int v) { return v < 0 ? -v : v; }
  namespace shapes {
    int perimeter(const Square& s) { return 4 * s.side; }
  }
}

int geo::manhattan(const Point& a, const Point& b) { return abs(a.x - b.x) + abs(a.y - b.y); }

namespace counter {
  int n = 0;
  void bump() { ++n; }
}

int main() {
  geo::Point a = {1, 5}, b = {4, 1};
  if (geo::manhattan(a, b) != 7) return 1;
  geo::shapes::Square sq = {{0, 0}, 3};
  if (geo::shapes::area(sq) != 9) return 2;
  if (geo::shapes::perimeter(sq) != 12) return 3;
  if (geo::value != 2 || ::value != 1 || value != 1) return 4;
  if (geo::global_value() != 1 || geo::own_value() != 2) return 5;
  counter::bump();
  counter::bump();
  if (counter::n != 2) return 6;
  geo::value = 20;
  if (geo::own_value() != 20) return 7;
  int value = 3;  // hides ::value in this block
  if (value != 3 || ::value != 1) return 8;
  return 0;
}
