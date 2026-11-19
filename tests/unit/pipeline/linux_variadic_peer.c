#include <stdarg.h>

int verify_variadic_values(int tag, ...) {
  va_list args;
  va_start(args, tag);
  int byte = va_arg(args, int);
  int small = va_arg(args, int);
  int truth = va_arg(args, int);
  long wide = va_arg(args, long);
  const char* pointer = va_arg(args, const char*);
  int stack_int = va_arg(args, int);
  long stack_long = va_arg(args, long);
  int final_int = va_arg(args, int);
  va_end(args);
  return tag != 13 || byte != 251 || small != -1223 || truth != 1 || wide != -(1L << 39) ||
         pointer[0] != 'x' || stack_int != -42 || stack_long != (1L << 41) || final_int != 3;
}
