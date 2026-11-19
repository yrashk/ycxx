// [basic.fundamental], [expr.mul], [expr.add], [expr.rel]: at run time, a nonzero value over zero
// and an overflowing product are infinities, which order above every finite value; infinity minus
// infinity is a NaN.
// STDOUT: inf -inf inf
// STDOUT: big=1 neg=1 sum=inf
// STDOUT: nan-ne-self=1
// EXIT: 0
#include <stdio.h>

double quo(double a, double b) { return a / b; }
double mul(double a, double b) { return a * b; }

int main() {
  double inf = quo(1.0, 0.0);
  double ninf = quo(-2.0, 0.0);
  double over = mul(1e308, 10.0);
  printf("%g %g %g\n", inf, ninf, over);
  printf("big=%d neg=%d sum=%g\n", inf > 1.7976931348623157e308, ninf < -1e308, inf + 1.0);
  double d = inf - over;
  printf("nan-ne-self=%d\n", d != d);
  return 0;
}
