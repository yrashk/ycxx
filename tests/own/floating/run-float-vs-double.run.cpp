// [basic.fundamental], [expr.arith.conv]: float arithmetic rounds to float at each operation and
// double to double; the same sums differ in the last places, and mixing promotes to double.
// STDOUT: 0x1.000002p+0 1.0000001192092896
// STDOUT: 0x1.0000000000001p+0
// STDOUT: f=1.00000011920928955 d=0.99999999999999989
// STDOUT: mix=0x1.99999a0000000p-4
// EXIT: 0
#include <stdio.h>

float fadd(float a, float b) { return a + b; }
double dadd(double a, double b) { return a + b; }

int main() {
  float f = fadd(1.0f, 0x1p-23f);
  printf("%a %.17g\n", f, f);
  printf("%a\n", dadd(1.0, 0x1p-52));
  float fs = 0;
  double ds = 0;
  for (int i = 0; i < 10; ++i) {
    fs = fadd(fs, 0.1f);
    ds = dadd(ds, 0.1);
  }
  printf("f=%.18g d=%.17g\n", fs, ds);
  float tenth = 0.1f;
  double zero = 0.0;
  printf("mix=%.13a\n", tenth + zero);
  return 0;
}
