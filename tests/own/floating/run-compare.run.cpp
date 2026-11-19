// [expr.rel], [expr.eq], [expr.arith.conv]: run-time comparisons of floats with doubles and of
// integers with floating values, after the usual arithmetic conversions.
// STDOUT: 0 1 1 1
// STDOUT: 1 0 1 1
// STDOUT: 1 1 0 1
// EXIT: 0
#include <stdio.h>

int main(int argc, char**) {
  float tenth_f = 0.1f;
  double tenth = 0.1;
  float half_f = 0.5f;
  double half = 0.5;
  printf("%d %d %d %d\n", tenth_f == tenth, tenth_f > tenth, half_f == half, half_f >= half);
  int n = 16777216 + argc; // 16777217
  float nf = 16777216.0f;
  double nd = 16777216.0;
  printf("%d %d %d %d\n", n == nf, n == nd, n > nd, n - 1 == nd);
  long long big = 9007199254740993LL;
  unsigned u = 4294967295u;
  printf("%d %d %d %d\n", big == 9007199254740992.0, u > 4294967294.5, -argc > 0.0f, -0.0 == 0.0);
  return 0;
}
