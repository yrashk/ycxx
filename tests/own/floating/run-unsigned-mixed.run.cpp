// [expr.arith.conv]: an unsigned or 64-bit operand with a floating one converts to the floating
// type (no unsigned wrap-around): -1 compared to 0.5 is -1.0 < 0.5, and 4294967295u + 1.0f is
// 2^32 in float.
// STDOUT: 4294967296 4294967296 1
// STDOUT: 1 1 0
// STDOUT: 3000000000.5 -2.5
// EXIT: 0
#include <stdio.h>

int main(int argc, char**) {
  unsigned u = 4294967295u;
  float uf = u + 1.0f;
  double ud = u + 1.0;
  int minus = -argc;
  printf("%.0f %.0f %d\n", uf, ud, minus < 0.5);
  unsigned three_billion = 3000000000u;
  unsigned long long big = 18446744073709551615ULL;
  printf("%d %d %d\n", three_billion > 2e9, big > 1.8e19, big < 1.8e19f);
  long long neg = -5;
  printf("%.1f %g\n", three_billion + 0.5, neg / 2.0);
  return 0;
}
