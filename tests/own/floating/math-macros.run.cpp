// [cmath.syn], [c.math]: <math.h>'s HUGE_VAL, HUGE_VALF, INFINITY and NAN: positive infinities
// of double and float, and a quiet NaN, which compares unequal to itself.
// STDOUT: inf inf inf
// STDOUT: 1 1 1 4
// STDOUT: 0 1 -inf
// EXIT: 0
#include <math.h>
#include <stdio.h>

int main() {
  double h = HUGE_VAL;
  float hf = HUGE_VALF;
  float inf = INFINITY;
  printf("%g %g %g\n", h, hf, inf);
  printf("%d %d %d %d\n", h > 1.7976931348623157e308, hf > 3.4e38f, h == inf, (int)sizeof(HUGE_VALF));
  float nan = NAN;
  printf("%d %d %g\n", nan == nan, nan != nan, -HUGE_VAL);
  return 0;
}
