// [expr.mul], [expr.arith.conv]: / with an integer and a floating operand divides in the floating
// type, unlike integer division; float division rounds to float.
// STDOUT: 3 3.5 -3.5 -3
// STDOUT: 0x1.555556p-2 0x1.5555555555555p-2
// STDOUT: 2.5 0.4
// EXIT: 0
#include <stdio.h>

int main(int argc, char**) {
  int seven = 6 + argc;
  int two = 1 + argc;
  printf("%d %g %g %d\n", seven / two, seven / 2.0, -seven / (double)two, -seven / two);
  float one = argc;
  float three = 3;
  printf("%a %a\n", one / three, (double)one / three);
  float five = 5;
  printf("%g %g\n", five / two, two / five);
  return 0;
}
