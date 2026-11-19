// [c.math], [cmath.syn]: calls of <math.h> functions taking and returning double whose results are
// exact or correctly rounded: sqrt, pow with small integer powers, floor, ceil, trunc, round, fmod,
// fmax, fmin and ldexp.
// STDOUT: 1.4142135623730951 3 1024 0.125
// STDOUT: -3 -2 -2 -3 3
// STDOUT: 1.5 -1.5 4 -0.5
// STDOUT: 0x1.6a09e667f3bcdp+0 40
// EXIT: 0
#include <math.h>
#include <stdio.h>

int main() {
  printf("%.17g %g %g %g\n", sqrt(2.0), sqrt(9.0), pow(2.0, 10.0), pow(0.5, 3.0));
  printf("%g %g %g %g %g\n", floor(-2.5), ceil(-2.5), trunc(-2.5), round(-2.5), round(2.5));
  printf("%g %g %g %g\n", fmod(7.5, 2.0), fmod(-7.5, 2.0), fmax(-1.0, 4.0), fmin(-0.5, 3.0));
  printf("%a %g\n", sqrt(2.0), ldexp(5.0, 3));
  return 0;
}
