// [conv.fpint]/1: run-time conversions from double and float to signed, unsigned and 64-bit
// integers truncate toward zero; every value here fits its destination after truncation.
// STDOUT: 2 -2 0 0
// STDOUT: 3000000000 4294967295 0
// STDOUT: -9000000000000000000 18000000000000000000
// STDOUT: 16777216 -7 65
// EXIT: 0
#include <stdio.h>

int to_int(double d) { return d; }
unsigned to_unsigned(double d) { return d; }
long long to_long(double d) { return d; }
unsigned long long to_ulong(double d) { return d; }
int f_to_int(float f) { return f; }
char to_char(double d) { return d; }

int main() {
  printf("%d %d %d %d\n", to_int(2.9), to_int(-2.9), to_int(-0.0), to_int(0.999));
  printf("%u %u %u\n", to_unsigned(3e9), to_unsigned(4294967295.75), to_unsigned(-0.75));
  printf("%lld %llu\n", to_long(-9e18), to_ulong(1.8e19));
  printf("%d %d %d\n", f_to_int(16777216.0f), f_to_int(-7.99f), to_char(65.5));
  return 0;
}
