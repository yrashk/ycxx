// θ step 2 (#280), spec-only: lambdas in an inline function and in member functions defined inside and
// outside the class ([expr.prim.lambda.closure], [dcl.inline], [class.mfct]).
inline int sumTo(int n) {
  int s = 0;
  auto add = [&s](int v) { s += v; };
  for (int i = 1; i <= n; ++i) add(i);
  return s;
}

struct Grid {
  int w;
  int h;
  int area() const { return [this] { return w * h; }(); }
  int border() const;
};

int Grid::border() const {
  auto side = [](int a) { return a > 0 ? a : 0; };
  return 2 * side(w) + 2 * side(h);
}

int main() {
  if (sumTo(4) != 10) return 1;
  Grid g{3, 4};
  if (g.area() != 12) return 2;
  if (g.border() != 14) return 3;
  return 0;
}
