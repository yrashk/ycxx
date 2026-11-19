// [expr.const], [dcl.constexpr]: constexpr functions with loops accumulating floating values, each
// step rounded: ten additions of 0.1 do not give 1.0, Newton's iteration converges to within an
// ulp of the square root of 2.
constexpr double sum_tenths(int n) {
  double s = 0;
  for (int i = 0; i < n; ++i)
    s += 0.1;
  return s;
}
static_assert(sum_tenths(10) != 1.0);
static_assert(sum_tenths(10) == 0x1.fffffffffffffp-1);
constexpr double newton_sqrt(double x) {
  double r = x;
  for (int i = 0; i < 20; ++i)
    r = (r + x / r) / 2;
  return r;
}
static_assert(newton_sqrt(4.0) == 2.0);
static_assert(newton_sqrt(2.25) == 1.5);
static_assert(newton_sqrt(2.0) >= 0x1.6a09e667f3bccp0 && newton_sqrt(2.0) <= 0x1.6a09e667f3bcep0);
constexpr double power(double b, int e) {
  double r = 1;
  while (e-- > 0)
    r *= b;
  return r;
}
static_assert(power(2.0, 10) == 1024.0 && power(0.5, 3) == 0.125);
constexpr float float_count() {
  float f = 16777214.0f;
  for (int i = 0; i < 4; ++i)
    f += 1.0f;
  return f;
}
static_assert(float_count() == 16777216.0f); // float stops counting at 2^24
