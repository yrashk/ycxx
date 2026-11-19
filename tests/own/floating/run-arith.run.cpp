// [expr.add], [expr.mul], [expr.unary.op]: run-time +, -, *, / and unary minus on doubles, with
// operands the compiler cannot fold (function parameters); %a shows the exact bits.
// STDOUT: 0x1.3333333333334p-2 0.30000000000000004
// STDOUT: 0x1.5555555555555p-2 0.33333333333333331
// STDOUT: -0x1.8p+0 0x1.2p+1 -2.5
// STDOUT: 0x1p-1074 0x1.fffffffffffffp+1023
// EXIT: 0
#include <stdio.h>

double add(double a, double b) { return a + b; }
double sub(double a, double b) { return a - b; }
double mul(double a, double b) { return a * b; }
double quo(double a, double b) { return a / b; }
double neg(double a) { return -a; }

int main() {
  double s = add(0.1, 0.2);
  printf("%a %.17g\n", s, s);
  double q = quo(1.0, 3.0);
  printf("%a %.17g\n", q, q);
  printf("%a %a %g\n", neg(1.5), mul(1.5, 1.5), sub(0.5, 3.0));
  printf("%a %a\n", quo(0x1p-1073, 2.0), mul(0x1.fffffffffffffp1022, 2.0));
  return 0;
}
