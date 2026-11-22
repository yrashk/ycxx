#include <cstdarg>

static void read_tail(std::va_list* cursor, double* floating, int* integers) {
  floating[0] = va_arg(*cursor, double);
  integers[0] = va_arg(*cursor, int);
  integers[1] = va_arg(*cursor, int);
  floating[1] = va_arg(*cursor, double);
  floating[2] = va_arg(*cursor, double);
  floating[3] = va_arg(*cursor, double);
  floating[4] = va_arg(*cursor, double);
  integers[2] = va_arg(*cursor, int);
  floating[5] = va_arg(*cursor, double);
}

extern "C" void read_named(double first, int tag, double second, double* floating,
                           int* integers, ...) {
  std::va_list cursor;
  va_start(cursor, integers);

  floating[0] = first;
  floating[1] = second;
  integers[0] = tag;
  floating[2] = va_arg(cursor, double);
  integers[1] = va_arg(cursor, int);
  floating[3] = va_arg(cursor, double);
  integers[2] = va_arg(cursor, int);

  // Both cursors start in the register save area and finish on the stack.
  std::va_list copy;
  va_copy(copy, cursor);
  read_tail(&cursor, floating + 4, integers + 3);
  read_tail(&copy, floating + 10, integers + 6);
  va_end(copy);
  va_end(cursor);
}

extern "C" void read_after_named_stack(double, double, double, double, double,
                                       double, double, double, double ninth,
                                       int* integer, double* floating, ...) {
  std::va_list cursor;
  va_start(cursor, floating);
  floating[0] = ninth;
  *integer = va_arg(cursor, int);
  floating[1] = va_arg(cursor, double);
  floating[2] = va_arg(cursor, double);
  va_end(cursor);
}

extern "C" void read_host_cursor(std::va_list* cursor, double* floating, int* integer) {
  floating[0] = va_arg(*cursor, double);
  *integer = va_arg(*cursor, int);
  floating[1] = va_arg(*cursor, double);
}

extern "C" int check_candidate_cursor(std::va_list* cursor);

extern "C" int forward_cursor(double, int tag, ...) {
  std::va_list cursor;
  va_start(cursor, tag);
  int result = check_candidate_cursor(&cursor);
  va_end(cursor);
  return result;
}
