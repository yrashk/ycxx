// [expr.call], [dcl.fct]: functions with more floating parameters than the eight floating argument
// registers, so the later ones go on the stack; each parameter is weighted by its position, so a
// lost or swapped argument changes the result.
// STDOUT: 1234567891.25
// STDOUT: 0x1.4p+3 12.5
// STDOUT: 66 66
// EXIT: 0
#include <stdio.h>

double digits(double a, double b, double c, double d, double e, double f, double g, double h,
              double i, double j, double k) {
  return a * 1e9 + b * 1e8 + c * 1e7 + d * 1e6 + e * 1e5 + f * 1e4 + g * 1e3 + h * 1e2 + i * 10 +
         j + k / 4;
}
float last_two(float a, float b, float c, float d, float e, float f, float g, float h, float i,
               float j) {
  return a * 0 + b * 0 + c * 0 + d * 0 + e * 0 + f * 0 + g * 0 + h * 0 + i * 2 + j;
}
double twelve(double a, double b, double c, double d, double e, double f, double g, double h,
              double i, double j, double k, double l) {
  return a + b + c + d + e + f + g + h + i + j + k + l;
}
float twelve_f(float a, float b, float c, float d, float e, float f, float g, float h, float i,
               float j, float k, float l) {
  return a + b + c + d + e + f + g + h + i + j + k + l;
}

int main() {
  printf("%.2f\n", digits(1, 2, 3, 4, 5, 6, 7, 8, 9, 1, 1));
  float r = last_two(9, 9, 9, 9, 9, 9, 9, 9, 4.5f, 1.0f);
  printf("%a %g\n", r, last_two(0, 0, 0, 0, 0, 0, 0, 0, 5, 2.5f));
  printf("%g %g\n", twelve(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0),
         twelve_f(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11));
  return 0;
}
