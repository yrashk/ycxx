// [c.math.abs], [cmath.syn]: <math.h>'s fabs and fabsf clear the sign: of a negative value, of
// negative zero and of negative infinity; a positive value is unchanged.
// STDOUT: 3.5 0 inf 0.25
// STDOUT: 2.5 0 0x1.99999ap-4
// STDOUT: signbit=0 0
// EXIT: 0
#include <math.h>
#include <stdio.h>

int main() {
  volatile double zero = 0.0;
  double nz = -zero;
  double ninf = -1 / zero;
  printf("%g %g %g %g\n", fabs(-3.5), fabs(nz), fabs(ninf), fabs(0.25));
  float nf = -2.5f;
  float nzf = -0.0f;
  printf("%g %g %a\n", fabsf(nf), fabsf(nzf), fabsf(-0.1f));
  printf("signbit=%d %d\n", signbit(fabs(nz)) != 0, signbit(fabsf(nzf)) != 0);
  return 0;
}
