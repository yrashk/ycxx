// [basic.start.static], [dcl.init]: floating globals and arrays of doubles with static storage
// duration, constant-initialized, zero-initialized or partly initialized, read and written at run
// time; a static local double keeps its value between calls.
// STDOUT: 2.5 0.1 0 -1e+300
// STDOUT: 1 2 3.5 0 0 sum=6.5
// STDOUT: 0.5 1 1.5
// STDOUT: 10
// EXIT: 0
#include <stdio.h>

double g = 2.5;
float gf = 0.1f;
double zeroed;
const double huge = -1e300;
double table[5] = {1, 2.0f, 3.5};
float ftable[2][2] = {{0.5f, 1.0f}, {1.5f, 2.0f}};

double counter() {
  static double total = 0;
  total += 0.5;
  return total;
}

int main() {
  printf("%g %g %g %g\n", g, gf, zeroed, huge);
  double sum = 0;
  for (int i = 0; i < 5; ++i)
    sum += table[i];
  printf("%g %g %g %g %g sum=%g\n", table[0], table[1], table[2], table[3], table[4], sum);
  double c1 = counter();
  double c2 = counter();
  double c3 = counter();
  printf("%g %g %g\n", c1, c2, c3);
  g *= 3;
  table[4] = ftable[1][0] + ftable[0][1];
  printf("%g\n", g + table[4] - zeroed);
  return 0;
}
