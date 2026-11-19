// The other side of abi-variadic-structs.run.cpp, compiled by Clang.
#include <stdarg.h>

struct D2 { double a, b; };
struct F3 { float a, b, c; };
struct D5 { double a, b, c, d, e; };
struct DI { double d; int i; };

// The arguments come in a fixed order: D2, F3, D5, DI, long double; each sum weighted by position.
double peer_vstructs(int n, ...) {
  va_list ap;
  va_start(ap, n);
  D2 d2 = va_arg(ap, D2);
  F3 f3 = va_arg(ap, F3);
  D5 d5 = va_arg(ap, D5);
  DI di = va_arg(ap, DI);
  long double ld = va_arg(ap, long double);
  va_end(ap);
  return (d2.a + d2.b) + 2 * (f3.a + f3.b + f3.c) + 3 * (d5.a + d5.b + d5.c + d5.d + d5.e) +
         4 * (di.d + di.i) + n * ld;
}
