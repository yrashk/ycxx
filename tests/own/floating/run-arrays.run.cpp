// [dcl.array], [expr.sub], [expr.add]: local arrays of doubles and floats filled, summed and
// searched in loops, indexed directly and through pointers.
// STDOUT: sum=5.5 mean=1.1 min=-2 max=4.5
// STDOUT: 0.25 0.5 1 2 4
// STDOUT: 7.5
// EXIT: 0
#include <stdio.h>

double sum(const double* p, int n) {
  double s = 0;
  for (int i = 0; i < n; ++i)
    s += p[i];
  return s;
}

int main() {
  double a[5] = {1.5, -2.0, 4.5, 0.25, 1.25};
  double lo = a[0];
  double hi = a[0];
  for (double* p = a; p != a + 5; ++p) {
    if (*p < lo) lo = *p;
    if (*p > hi) hi = *p;
  }
  printf("sum=%g mean=%g min=%g max=%g\n", sum(a, 5), sum(a, 5) / 5, lo, hi);
  float pow2[5];
  pow2[0] = 0.25f;
  for (int i = 1; i < 5; ++i)
    pow2[i] = pow2[i - 1] * 2;
  printf("%g %g %g %g %g\n", pow2[0], pow2[1], pow2[2], pow2[3], pow2[4]);
  double grid[2][3] = {{1, 2, 3}, {0.5, 0.25, 0.75}};
  printf("%g\n", sum(grid[0], 3) + sum(&grid[1][0], 3));
  return 0;
}
