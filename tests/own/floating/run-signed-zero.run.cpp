// [basic.fundamental], [expr.unary.op], [expr.mul]: negative zero compares equal to zero but keeps
// its sign: unary minus makes it, printf shows it, and 1 / -0.0 is negative infinity.
// STDOUT: -0 0 -0x0p+0
// STDOUT: eq=1 lt=0
// STDOUT: -inf inf
// STDOUT: -0 0
// EXIT: 0
#include <stdio.h>

double neg(double x) { return -x; }
double quo(double a, double b) { return a / b; }

int main() {
  double nz = neg(0.0);
  double pz = neg(nz);
  printf("%g %g %a\n", nz, pz, nz);
  printf("eq=%d lt=%d\n", nz == pz, nz < pz);
  printf("%g %g\n", quo(1.0, nz), quo(1.0, pz));
  printf("%g %g\n", nz * 5.0, nz + pz); // -0 * 5 is -0; -0 + +0 is +0
  return 0;
}
