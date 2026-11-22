// The Clang side of definitions.run.cpp: calls ycxx's variadic definitions and reads a
// va_list that ycxx started.
#include <stdarg.h>

extern "C" {
long ycxx_sum(int n, ...);
double ycxx_doubles(int n, ...);
long ycxx_stacked(long, long, long, long, long, long, long, long, char, int, ...);
long ycxx_copied(int n, ...);

long peer_vsum(int n, va_list ap) {
  long s = 0;
  for (int i = 0; i < n; ++i) s += va_arg(ap, long);
  return s;
}

int peer_calls() {
  if (ycxx_sum(3, 5L, 6L, -1L) != 10) return 11;
  if (ycxx_doubles(2, 4.0, 0.5f) != 40.5) return 12;
  char text[] = "yes";
  if (ycxx_stacked(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 3, 1.5, text, 0xfedcba9876543210ul) != 55000 + 300 + 15 + 1)
    return 13;
  short s = -4;
  if (ycxx_copied(2, s, 'a') != 93 * 1000 + 186) return 14;
  return 0;
}
}
