#include <cstdarg>

extern "C" double read_adjusted_cursor(std::va_list cursor) {
  return va_arg(cursor, double);
}

extern "C" float return_named_float(float first, double second, double* observed, ...) {
  std::va_list cursor;
  va_start(cursor, observed);
  observed[0] = second;
  observed[1] = read_adjusted_cursor(cursor);
  va_end(cursor);
  return first;
}

extern "C" double return_unnamed_double(float, double second, ...) {
  std::va_list cursor;
  va_start(cursor, second);
  double result = read_adjusted_cursor(cursor);
  va_end(cursor);
  return result;
}
