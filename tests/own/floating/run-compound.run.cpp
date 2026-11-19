// [expr.assign]/7, [conv.fpint]: compound assignment with an integer left operand and a floating
// right one computes in the floating type and truncates back; with a float left operand and a
// double right one it computes in double and converts to float.
// STDOUT: 4 4 8 5
// STDOUT: 2 70 -3
// STDOUT: 0x1.19999ap+0 1.75
// EXIT: 0
#include <stdio.h>

int main() {
  int i = 3;
  i *= 1.5;
  int i1 = i;
  i += 0.75;
  int i2 = i;
  i /= 0.5;
  int i3 = i;
  i -= 2.5;
  printf("%d %d %d %d\n", i1, i2, i3, i);
  unsigned u = 10;
  u *= 0.25;
  char c = 'A';
  c += 5.9;
  long long l = -7;
  l /= 2.0;
  printf("%u %d %lld\n", u, c, l);
  float f = 1.0f;
  f += 0.1;
  double d = 1.0;
  d += 0.5f;
  d *= 3.5;
  d /= 3;
  printf("%a %g\n", f, d);
  return 0;
}
