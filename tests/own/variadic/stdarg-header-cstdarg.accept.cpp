// [cstdarg.syn], [cpp.replace.general]: <stdarg.h> and libycxx's <cstdarg> define the same
// macros with the same replacement lists, so either order is a valid redefinition (#676).
#include <stdarg.h>
#include <cstdarg>
#include <stdarg.h>

static_assert(__is_same(std::va_list, va_list));

int sum(int n, ...) {
  std::va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; ++i) s += va_arg(ap, int);
  va_end(ap);
  return s;
}
