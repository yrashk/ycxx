// [basic.start.static], [basic.start.dynamic]: a global double initialized by a function call is
// dynamically initialized before main; one initialized by a constant expression is constant
// initialized, even when the dynamic one reads it.
// STDOUT: 0.75 2.25 1.5
// EXIT: 0
#include <stdio.h>

double base = 0.75;
double triple(double x) { return x * 3; }
double computed = triple(base);
float converted = computed - base;

int main() {
  printf("%g %g %g\n", base, computed, converted);
  return 0;
}
