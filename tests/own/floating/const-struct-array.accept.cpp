// [expr.const], [dcl.constexpr], [dcl.init.aggr]: constexpr aggregates and arrays with floating
// members, read and combined in constant expressions.
struct Point { double x, y; };
constexpr Point mid(Point a, Point b) { return {(a.x + b.x) / 2, (a.y + b.y) / 2}; }
constexpr double dot(Point a, Point b) { return a.x * b.x + a.y * b.y; }
constexpr Point m = mid({0.0, 1.0}, {3.0, -2.0});
static_assert(m.x == 1.5 && m.y == -0.5);
static_assert(dot({1.5, 2.0}, {2.0, 0.25}) == 3.5);
constexpr double weights[] = {0.5, 0.25, 0.125, 0.125};
constexpr double total() {
  double s = 0;
  for (double w : weights)
    s += w;
  return s;
}
static_assert(total() == 1.0);
constexpr double largest(const double* p, int n) {
  double r = p[0];
  for (int i = 1; i < n; ++i)
    if (p[i] > r) r = p[i];
  return r;
}
constexpr double values[5] = {-1.5, 2.25, 0.0, 2.0, -3.0};
static_assert(largest(values, 5) == 2.25);
struct Mixed { float f; int i; double d; };
constexpr Mixed mx{0.5f, 3, 0.25};
static_assert(mx.f * mx.i + mx.d == 1.75);
constexpr float floats[3] = {1, 2.5, 0.1};
static_assert(floats[0] == 1.0f && floats[2] == 0.1f);
