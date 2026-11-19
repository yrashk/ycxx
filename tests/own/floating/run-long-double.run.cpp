// [basic.fundamental], [expr.call]/12: on Apple arm64 long double has the format of double:
// 8 bytes, the same arithmetic results, and printf reads it with %Lg / %La.
// REQUIRES: darwin
// STDOUT: 8 0x1.5555555555555p-2 0x1.5555555555555p-2
// STDOUT: 1.5 0.1
// EXIT: 0
#include <stdio.h>

long double third(long double x) { return x / 3; }

int main() {
  long double one = 1;
  printf("%d %La %a\n", (int)sizeof(long double), third(one), 1.0 / 3);
  long double sum = 1.0L;
  sum += 0.5;
  double d = 0.1L;
  printf("%Lg %g\n", sum, d);
  return third(one) == 1.0 / 3 ? 0 : 1;
}
