// #676: the macOS SDK's <_wchar.h> (reached from <wchar.h> and <cwchar>) includes <stdarg.h>,
// which the SDK leaves to the compiler. ycxx's resource header supplies it, and libycxx's
// <cstdarg> can follow it ([cstdarg.syn], [cpp.replace.general]).
// REQUIRES: darwin
// PARITY: by design: the macOS SDK's headers exist only on Darwin hosts
#include <_wchar.h>
#include <cstdarg>

int sum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; ++i) s += va_arg(ap, int);
  va_end(ap);
  return s;
}
