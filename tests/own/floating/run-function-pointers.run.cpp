// [conv.func], [expr.call], [dcl.fct]: calls through pointers to functions taking and returning
// double and float, from a table and as arguments.
// STDOUT: 3.5 -0.5 3 0.75
// STDOUT: 6.25 0.5
// EXIT: 0
#include <stdio.h>

double add(double a, double b) { return a + b; }
double sub(double a, double b) { return a - b; }
double mul(double a, double b) { return a * b; }
double quo(double a, double b) { return a / b; }
float fsquare(float f) { return f * f; }
float fhalf(float f) { return f / 2; }

double apply(double (*op)(double, double), double a, double b) { return op(a, b); }
float twice(float (*f)(float), float x) { return f(f(x)); }

int main() {
  double (*ops[4])(double, double) = {add, sub, mul, &quo};
  printf("%g %g %g %g\n", ops[0](1.5, 2), ops[1](1.5, 2), apply(ops[2], 1.5, 2), apply(quo, 1.5, 2));
  float (*sq)(float) = fsquare;
  printf("%g %g\n", sq(2.5f), twice(fhalf, 2.0f));
  return 0;
}
