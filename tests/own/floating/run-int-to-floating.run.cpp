// [conv.fpint]/2: run-time conversions from signed, unsigned and 64-bit integers to double and
// float; values too wide for the significand round to nearest (the target's choice).
// STDOUT: -1 4294967295 -2147483648
// STDOUT: 18446744073709551616 -9223372036854775808 9007199254740992
// STDOUT: 16777216 4294967296 1.8446744e+19
// STDOUT: 65 1 0
// EXIT: 0
#include <stdio.h>

double from_int(int i) { return i; }
double from_unsigned(unsigned u) { return u; }
double from_long(long long l) { return l; }
double from_ulong(unsigned long long u) { return u; }
float f_from_int(int i) { return i; }
float f_from_unsigned(unsigned u) { return u; }
float f_from_ulong(unsigned long long u) { return u; }

int main() {
  printf("%.0f %.0f %.0f\n", from_int(-1), from_unsigned(4294967295u), from_int(-2147483647 - 1));
  printf("%.0f %.0f %.0f\n", from_ulong(18446744073709551615ULL),
         from_long(-9223372036854775807LL - 1), from_long(9007199254740993LL));
  printf("%.0f %.0f %.8g\n", f_from_int(16777217), f_from_unsigned(4294967295u),
         f_from_ulong(18446744073709551615ULL));
  char c = 'A';
  bool t = true;
  short z = 0;
  printf("%g %g %g\n", from_int(c), from_int(t), (double)z);
  return 0;
}
