#include "linux_bool_abi.hpp"
#include <cstdarg>

extern "C" int bool417_named(bool named, int anchor, ...) {
  if (anchor != 0x1234) return -1;

  std::va_list arguments;
  va_start(arguments, anchor);
  const int unnamed = va_arg(arguments, int);
  va_end(arguments);
  return 2 * named + unnamed;
}
