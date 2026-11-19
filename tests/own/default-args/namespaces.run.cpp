// ι (#300): a function declared with default arguments in a namespace, defined outside it with
// a qualified name, and called qualified, unqualified from inside the namespace, and unqualified
// through a using-directive. A default argument's names are looked up in the scope of the
// function's declaration, not the caller's ([dcl.fct.default]/1, /5, [namespace.udir],
// [basic.lookup]).
// EXIT: 0
namespace geo {
  int scale = 2;
  enum Unit { cm = 1, m = 100 };
  int area(int w, int h = scale + 1, Unit u = cm);
  int square(int s = 4) { return area(s, s); }
}

int geo::area(int w, int h, Unit u) { return w * h * scale * u; }

int scale = 1000;  // not the `scale` that geo::area's default argument names

namespace outer {
  namespace inner {
    int depth(int d = 2) { return d; }
  }
}

int main() {
  if (geo::area(5) != 30) return 1;
  if (geo::area(5, 1) != 10) return 2;
  if (geo::area(5, 1, geo::m) != 1000) return 3;
  if (geo::square() != 32) return 4;
  using namespace geo;
  if (area(2) != 12) return 5;
  if (square(1) != 2) return 6;
  if (outer::inner::depth() != 2) return 7;
  using namespace outer::inner;
  if (depth() != 2 || depth(9) != 9) return 8;
  return 0;
}
