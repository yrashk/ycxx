// [expr.call], [dcl.fct]: integer and floating parameters interleaved, more of each than the
// eight registers of its kind, so both kinds spill to the stack in their own order.
// STDOUT: i=123456789 d=0.987654321
// STDOUT: 7 2.5 -3 0.125 40000000000
// EXIT: 0
#include <stdio.h>

void interleaved(int i1, double d1, int i2, double d2, int i3, double d3, int i4, double d4,
                 int i5, double d5, int i6, double d6, int i7, double d7, int i8, double d8,
                 int i9, double d9) {
  long is = ((((((((i1 * 10L + i2) * 10 + i3) * 10 + i4) * 10 + i5) * 10 + i6) * 10 + i7) * 10 +
              i8) * 10 + i9);
  double ds = d1 / 10 + d2 / 100 + d3 / 1e3 + d4 / 1e4 + d5 / 1e5 + d6 / 1e6 + d7 / 1e7 +
              d8 / 1e8 + d9 / 1e9;
  printf("i=%ld d=%.9f\n", is, ds);
}
void kinds(char c, float f, short s, double d, long long l) {
  printf("%d %g %d %g %lld\n", c, f, s, d, l);
}

int main() {
  interleaved(1, 9, 2, 8, 3, 7, 4, 6, 5, 5, 6, 4, 7, 3, 8, 2, 9, 1);
  kinds(7, 2.5f, -3, 0.125, 40000000000LL);
  return 0;
}
