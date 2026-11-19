// [expr.call], [dcl.fct], [conv.fpprom]: with a prototype, a float argument is passed as float
// (not promoted) and a float result returned as float; float arguments convert to double and
// long double parameters, and an int argument to a float parameter.
// STDOUT: 0x1.99999ap-4 0x1.99999ap-4
// STDOUT: 0x1.8p+0 0x1p+1 7
// STDOUT: 3.5 0.75
// EXIT: 0
#include <stdio.h>

float identity(float f) { return f; }
double widen(float f) { return f; }
float halve(float f) { return f / 2; }
float from_int(int i) { return i; }
double as_double(double d) { return d; }
long double as_long_double(long double d) { return d; }
float sum3(float a, double b, float c) { return a + b + c; }

int main() {
  float t = identity(0.1f);
  printf("%a %a\n", t, widen(t));
  printf("%a %a %g\n", halve(3.0f), as_double(identity(2.0f)), from_int(7));
  printf("%g %Lg\n", sum3(1.0f, 2.0, 0.5f), as_long_double(0.75f));
  return 0;
}
