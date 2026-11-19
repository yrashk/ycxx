// [basic.fundamental]: run-time arithmetic produces and consumes subnormal values (no flush to
// zero): halving the smallest normal double and float, and stepping down to the smallest
// subnormal and below it, to zero.
// STDOUT: 1.1125369292536007e-308 4.9406564584124654e-324 0
// STDOUT: 5.87747175e-39 1.40129846e-45 0
// STDOUT: 1 1
// EXIT: 0
#include <stdio.h>

double half(double x) { return x / 2; }
float half_f(float x) { return x / 2; }

int main() {
  double min_normal = 0x1p-1022;
  double tiny = 0x1p-1073;
  printf("%.17g %.17g %g\n", half(min_normal), half(tiny), half(half(tiny)));
  float min_normal_f = 0x1p-126f;
  float tiny_f = 0x1p-148f;
  printf("%.9g %.9g %g\n", half_f(min_normal_f), half_f(tiny_f), half_f(half_f(tiny_f)));
  printf("%d %d\n", half(min_normal) * 2 == min_normal, half_f(tiny_f) > 0);
  return 0;
}
