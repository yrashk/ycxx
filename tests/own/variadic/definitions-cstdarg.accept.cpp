// [cstdarg.syn]: a variadic definition through <cstdarg>'s macros, on every target (#647).
// definitions.run.cpp runs the builtins they expand to; a run test including a libycxx
// header cannot link on Darwin yet.
#include <cstdarg>

static int count(int n, ...) {
  std::va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; ++i) s += va_arg(ap, int);
  va_end(ap);
  return s;
}

static double mixed(int n, ...) {
  std::va_list ap, copy;
  va_start(ap, n);
  va_copy(copy, ap);
  double s = 0;
  for (int i = 0; i < n; ++i) s += va_arg(ap, double);
  for (int i = 0; i < n; ++i) s += va_arg(copy, double);
  va_end(copy);
  va_end(ap);
  return s;
}

int main() {
  if (count(3, 4, 5, 6) != 15) return 1;
  if (mixed(2, 0.5, 1.25) != 3.5) return 2;
  return 0;
}
