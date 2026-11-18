// δ s1 stretch (#86): aggregates in constant expressions, a struct parameter by value: a constexpr struct, member access and
// aggregate initialization in static_assert and in a constexpr function
// ([expr.const], [dcl.constexpr], [dcl.init.aggr]).
// EXIT: 0
struct Span { int lo; int hi; };

constexpr Span whole = {0, 10};
static_assert(whole.hi - whole.lo == 10);

constexpr int length(Span s) { return s.hi - s.lo; }
static_assert(length({3, 8}) == 5);

constexpr int sum_lengths() {
  Span parts[3] = {{0, 2}, {5, 9}, {}};
  int n = 0;
  for (int i = 0; i < 3; i++) n += parts[i].hi - parts[i].lo;
  return n;
}
static_assert(sum_lengths() == 6);

int main() {
  constexpr Span half{whole.lo, whole.hi / 2};
  static_assert(half.hi == 5);
  if (length(half) != 5) return 1;
  if (sum_lengths() != 6) return 2;
  return 0;
}
