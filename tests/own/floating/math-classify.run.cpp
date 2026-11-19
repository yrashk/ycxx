// [c.math.fpclass], [cmath.syn]: <math.h>'s classification macros isinf, isnan, isfinite,
// isnormal and signbit on zero, negative zero, a normal value, a subnormal, an infinity and a NaN,
// for double and float arguments. Each row prints inf nan finite normal sign.
// XFAIL: ycxx <math.h> is libycxx's wrapper of <cmath>, which does not compile yet (#349)
// STDOUT: 0.0: 0 0 1 0 0
// STDOUT: -0.0: 0 0 1 0 1
// STDOUT: 1.5: 0 0 1 1 0
// STDOUT: subnormal: 0 0 1 0 0
// STDOUT: -inf: 1 0 0 0 1
// STDOUT: nan: 0 1 0 0 0
// STDOUT: float -2.5f: 0 0 1 1 1
// STDOUT: float 1e-40f: 0 0 1 0 0
// EXIT: 0
#include <math.h>
#include <stdio.h>

void show(const char* name, double x) {
  printf("%s: %d %d %d %d %d\n", name, isinf(x) != 0, isnan(x) != 0, isfinite(x) != 0,
         isnormal(x) != 0, signbit(x) != 0);
}
void show_f(const char* name, float x) {
  printf("%s: %d %d %d %d %d\n", name, isinf(x) != 0, isnan(x) != 0, isfinite(x) != 0,
         isnormal(x) != 0, signbit(x) != 0);
}

int main() {
  volatile double zero = 0.0;
  show("0.0", zero);
  show("-0.0", -zero);
  show("1.5", 1.5);
  show("subnormal", 0x1p-1060);
  show("-inf", -1 / zero);
  show("nan", fabs(zero / zero));
  show_f("float -2.5f", -2.5f);
  show_f("float 1e-40f", 1e-40f);
  return 0;
}
