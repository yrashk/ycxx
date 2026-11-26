// Local classes (#117), spec-only: a constexpr function defining and using a local class with
// constexpr member functions is usable in a constant expression ([dcl.constexpr], [expr.const],
// [class.local]).
constexpr int sum_squares(int n) {
  struct Acc {
    int total = 0;
    constexpr void add(int v) { total += v * v; }
  };
  Acc a;
  for (int i = 1; i <= n; ++i) a.add(i);
  return a.total;
}

static_assert(sum_squares(3) == 14);
static_assert(sum_squares(0) == 0);

int main() {
  constexpr int k = sum_squares(4);
  static_assert(k == 30);
  int n = 5;
  if (sum_squares(n) != 55) return 1;
  return 0;
}
