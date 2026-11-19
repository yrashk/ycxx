// [expr.call]/12, [conv.fpprom]: a float argument matched by an ellipsis is promoted to double, so
// printf's %f, %e, %g and %a read it as a double; double and long double keep their types.
// STDOUT: 1.500000 2.500000e-01 0.1 0x1.99999ap-4
// STDOUT: 0.10000000149011612 0.10000000000000001
// STDOUT: 3.000000 -0.75
// EXIT: 0
#include <stdio.h>

int main() {
  float a = 1.5f;
  float b = 0.25f;
  float c = 0.1f;
  printf("%f %e %g %a\n", a, b, c, c);
  double d = 0.1;
  printf("%.17g %.17g\n", c, d);
  long double ld = 3.0L;
  float neg = -0.75f;
  printf("%Lf %g\n", ld, neg);
  return 0;
}
