// [basic.fundamental]: __builtin_fabs, __builtin_inf, __builtin_huge_val and __builtin_nan used
// at run time: their values, printed, and a NaN's behavior in comparisons.
// STDOUT: inf -inf inf
// STDOUT: 2.5 0 inf
// STDOUT: nan-eq=0 nan-ne=1 nan-lt=0
// EXIT: 0
#include <stdio.h>

double negate(double x) { return -x; }

int main() {
  double inf = __builtin_inf();
  printf("%g %g %g\n", inf, negate(__builtin_huge_val()), (double)__builtin_inff());
  double nz = negate(0.0);
  printf("%g %g %g\n", __builtin_fabs(negate(2.5)), __builtin_fabs(nz), __builtin_fabs(-inf));
  double nan = __builtin_nan("");
  double one = 1.0;
  printf("nan-eq=%d nan-ne=%d nan-lt=%d\n", nan == nan, nan != nan, nan < one);
  return 0;
}
