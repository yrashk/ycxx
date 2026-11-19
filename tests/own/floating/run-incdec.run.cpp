// [expr.pre.incr], [expr.post.incr]: ++ and -- on double and float objects at run time; postfix
// yields the old value, prefix the new one, and float stops counting at 2^24.
// STDOUT: 0.5 1.5 2.5 2.5 -0.5
// STDOUT: 16777216 16777216 -0.75
// EXIT: 0
#include <stdio.h>

int main() {
  double d = 0.5;
  double a = d++;
  double b = d;
  double c = ++d;
  double e = d--;
  --d;
  --d;
  printf("%g %g %g %g %g\n", a, b, c, e, d);
  float f = 16777216.0f;
  float g = ++f;
  float h = 0.25f;
  h--;
  printf("%.0f %.0f %g\n", f, g, h);
  return 0;
}
