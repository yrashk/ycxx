// [expr.cond]/7.2, [expr.arith.conv]: ?: with an int and a double operand has type double whichever
// branch is taken, so the int operand is converted (and printed as a double).
// STDOUT: 1 2.5 3 0.5
// STDOUT: 8 8
// EXIT: 0
#include <stdio.h>

double pick(bool b, int i, double d) { return b ? i : d; }

int main(int argc, char**) {
  bool yes = argc > 0;
  int one = 1;
  double r1 = yes ? one : 2.5;
  double r2 = !yes ? one : 2.5;
  float half = 0.5f;
  printf("%g %g %g %g\n", r1, r2, pick(true, 3, 9.0), pick(false, 3, half));
  printf("%d %d\n", (int)sizeof(yes ? one : 2.5), (int)sizeof(yes ? 1.0f : 2.0));
  return 0;
}
