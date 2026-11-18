// [dcl.constexpr], [expr.const]: immediate invocations of consteval functions with constant
// arguments, consteval calling consteval, and calls from an immediate function context.
consteval int sq(int n) { return n * n; }
consteval int sq_sum(int a, int b) { return sq(a) + sq(b); }
consteval int count_up(int n) {
  int total = 0;
  for (int i = 1; i <= n; ++i)
    total += i;
  return total;
}
constexpr int nine = sq(3);
const int sixteen = sq(4);
int runtime_value = sq(5);
constexpr int via_constexpr() { return sq(6) + 1; }
static_assert(nine == 9);
static_assert(sixteen == 16);
static_assert(sq_sum(3, 4) == 25);
static_assert(count_up(10) == 55);
static_assert(via_constexpr() == 37);
int use() {
  int x = sq(7);
  constexpr int c = count_up(4);
  static_assert(c == 10);
  return x + sq(c);
}
