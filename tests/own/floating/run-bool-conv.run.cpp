// [conv.bool], [stmt.if], [expr.log.not]: at run time, a floating condition is false only for zero
// of either sign; a NaN, a subnormal and an infinity are true.
// STDOUT: 0 0 1 1 1 1
// STDOUT: loop=3
// EXIT: 0
#include <stdio.h>

bool truth(double d) { return d; }

int main() {
  volatile double zero = 0.0;
  double nz = -zero;
  double nan = zero / zero;
  double sub = 0x1p-1074;
  double inf = 1 / zero;
  printf("%d %d %d %d %d %d\n", truth(zero), truth(nz), truth(nan), truth(sub), truth(inf),
         truth(0.5f));
  int n = 0;
  for (double d = 1.5; d; d -= 0.5)
    ++n;
  printf("loop=%d\n", n);
  if (zero || nz) return 1;
  if (!nan) return 2;
  return 0;
}
