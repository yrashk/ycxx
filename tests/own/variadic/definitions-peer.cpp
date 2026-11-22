// The Clang side of definitions.run.cpp: calls ycxx's variadic definitions and reads a
// va_list that ycxx started.
#include <stdarg.h>

extern "C" {
long ycxx_sum(int n, ...);
double ycxx_doubles(int n, ...);
long ycxx_stacked(long, long, long, long, long, long, long, long, char, int, ...);
long ycxx_copied(int n, ...);
double ycxx_fstack(double, double, double, double, double, double, double, double, float, double, ...);
struct Big { long a, b, c; };
Big ycxx_big(int n, ...);

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
  if (ycxx_fstack(1, 2, 3, 4, 5, 6, 7, 8, 0.5f, 0.25, -1, 2.0) != 36.75 - 100 + 2000) return 15;
  Big b = ycxx_big(3, 1L, 2L, 3L);
  if (b.a != 1 || b.b != 6 || b.c != 0) return 16;
  return 0;
}
}
