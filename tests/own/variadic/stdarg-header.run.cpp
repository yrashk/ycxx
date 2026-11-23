// [cstdarg.syn], [support.c.headers.general]: the C header <stdarg.h> supplies va_list and the
// va_start, va_arg, va_copy and va_end macros on every target (#676). The macOS SDK leaves the
// header to the compiler, and its <stdio.h> and <wchar.h> include it.
// EXIT: 0
#include <stdarg.h>
#include <stdarg.h>

static int count(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; ++i) s += va_arg(ap, int);
  va_end(ap);
  return s;
}

static double twice(int n, ...) {
  va_list ap, copy, legacy;
  va_start(ap, n);
  va_copy(copy, ap);
  __va_copy(legacy, ap);
  double s = 0;
  for (int i = 0; i < n; ++i) s += va_arg(ap, double) + va_arg(copy, double) + va_arg(legacy, double);
  va_end(legacy);
  va_end(copy);
  va_end(ap);
  return s;
}

static_assert(__is_same(va_list, __builtin_va_list));

int main() {
  if (count(3, 4, 5, 6) != 15) return 1;
  if (twice(2, 0.5, 1.25) != 5.25) return 2;
  return 0;
}
