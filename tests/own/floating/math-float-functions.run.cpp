// [c.math], [cmath.syn]: the float versions of <math.h> functions (sqrtf, floorf, ceilf, powf,
// fmodf) take and return float, so their results are float values.
// STDOUT: 0x1.6a09e6p+0 1.41421354
// STDOUT: -3 3 256 0.5
// EXIT: 0
#include <math.h>
#include <stdio.h>

int main() {
  float r = sqrtf(2.0f);
  printf("%a %.9g\n", r, r);
  printf("%g %g %g %g\n", floorf(-2.25f), ceilf(2.25f), powf(4.0f, 4.0f), fmodf(5.5f, 1.0f));
  return sizeof(sqrtf(2.0f)) == sizeof(float) ? 0 : 1;
}
