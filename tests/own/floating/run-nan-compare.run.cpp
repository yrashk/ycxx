// [expr.rel], [expr.eq], [basic.fundamental]: a NaN, made at run time as 0.0 / 0.0, is unordered:
// every relational comparison and == with it is false, and != is true, even against itself.
// STDOUT: lt=0 le=0 gt=0 ge=0 eq=0 ne=1
// STDOUT: self eq=0 ne=1
// STDOUT: not-lt=1 if=nan-true
// EXIT: 0
#include <stdio.h>

double divide(double a, double b) { return a / b; }

int main() {
  volatile double zero = 0.0;
  double nan = zero / zero;
  double one = 1.0;
  printf("lt=%d le=%d gt=%d ge=%d eq=%d ne=%d\n", nan < one, nan <= one, nan > one, nan >= one,
         nan == one, nan != one);
  double n2 = divide(0.0, 0.0);
  printf("self eq=%d ne=%d\n", n2 == n2, n2 != n2);
  // [conv.bool]: NaN is not zero, so it converts to true.
  printf("not-lt=%d if=%s\n", !(nan < one), nan ? "nan-true" : "nan-false");
  return nan == nan;
}
