// [expr.call]/12: doubles and promoted floats passed to printf through the ellipsis, printed with
// %f, %e, %g, %a and %.17g, many in one call so some go on the stack.
// STDOUT: 3.141593 3.141593e+00 3.14159 0x1.921fb54442d18p+1 3.1415926535897931
// STDOUT: 0.000010 1.000000e-05 1e-05 1e+20 100000
// STDOUT: 1.5 2.5 3.5 4.5 5.5 6.5 7.5 8.5 9.5 10.5
// STDOUT: -0.333333 -3.33e-01 -0.33333334
// EXIT: 0
#include <stdio.h>

int main() {
  double pi = 3.141592653589793;
  printf("%f %e %g %a %.17g\n", pi, pi, pi, pi, pi);
  double small = 1e-5;
  printf("%f %e %g %g %g\n", small, small, small, 1e20, 100000.0);
  float f = 1.5f;
  printf("%g %g %g %g %g %g %g %g %g %g\n", f, f + 1, f + 2, f + 3, f + 4, f + 5, f + 6, f + 7,
         f + 8, f + 9);
  float third = -1.0f / 3;
  printf("%f %.2e %.8g\n", third, third, third);
  return 0;
}
