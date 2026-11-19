// The other side of abi-variadic.run.cpp, compiled by Clang: variadic functions reading doubles.
#include <stdarg.h>

double peer_vsum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  double s = 0;
  for (int i = 0; i < n; ++i)
    s += va_arg(ap, double);
  va_end(ap);
  return s;
}

double peer_vmixed(const char* kinds, ...) {
  va_list ap;
  va_start(ap, kinds);
  double s = 0;
  for (int i = 0; kinds[i]; ++i) {
    double v = 0;
    switch (kinds[i]) {
    case 'i': v = va_arg(ap, int); break;
    case 'd': v = va_arg(ap, double); break;
    case 'l': v = va_arg(ap, long long); break;
    case 'L': v = va_arg(ap, long double); break;
    }
    s += (i + 1) * v;
  }
  va_end(ap);
  return s;
}
