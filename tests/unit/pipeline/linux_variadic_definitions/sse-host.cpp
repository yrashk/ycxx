#include <cstdarg>

extern "C" void read_named(double, int, double, double*, int*, ...);
extern "C" void read_after_named_stack(double, double, double, double, double,
                                        double, double, double, double, int*, double*, ...);
extern "C" void read_host_cursor(std::va_list*, double*, int*);
extern "C" int forward_cursor(double, int, ...);

static bool check_named() {
  double floating[16]{};
  int integers[9]{};
  // 44 is the first GP spill. The next three doubles still occupy XMM5–XMM7.
  // The shared overflow area then holds 9.75, 55, and -10.875 in that order.
  read_named(1.25, 113, -2.5, floating, integers, 3.125f, 11, -4.75, 22,
             5.5, 33, 44, -6.125, 7.25, -8.5, 9.75, 55, -10.875);
  const double expected_floating[16] = {1.25, -2.5, 3.125, -4.75, 5.5, -6.125, 7.25,
                                        -8.5, 9.75, -10.875, 5.5, -6.125, 7.25,
                                        -8.5, 9.75, -10.875};
  const int expected_integers[9] = {113, 11, 22, 33, 44, 55, 33, 44, 55};
  for (int i = 0; i < 16; ++i) {
    if (floating[i] != expected_floating[i]) return false;
  }
  for (int i = 0; i < 9; ++i) {
    if (integers[i] != expected_integers[i]) return false;
  }
  return true;
}

static bool check_named_stack() {
  double floating[3]{};
  int integer{};
  read_after_named_stack(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, -9.5,
                         &integer, floating, 307, 10.625, -11.75);
  return floating[0] == -9.5 && integer == 307 && floating[1] == 10.625 &&
         floating[2] == -11.75;
}

static bool check_host_cursor(int tag, ...) {
  std::va_list cursor;
  va_start(cursor, tag);
  bool prefix_matches = true;
  for (int i = 1; i <= 7; ++i) {
    if (va_arg(cursor, double) != i * 0.5) prefix_matches = false;
  }
  double floating[2]{};
  int integer{};
  read_host_cursor(&cursor, floating, &integer);
  double last = va_arg(cursor, double);
  va_end(cursor);
  return prefix_matches && floating[0] == -11.25 && integer == 417 &&
         floating[1] == 12.625 && last == -13.875;
}

extern "C" int check_candidate_cursor(std::va_list* cursor) {
  double first = va_arg(*cursor, double);
  int integer = va_arg(*cursor, int);
  double second = va_arg(*cursor, double);
  return first == 1.625 && integer == 523 && second == -2.875 ? 0 : 4;
}

int main() {
  if (!check_named()) return 1;
  if (!check_named_stack()) return 2;
  // An extra double lets the cursor-advance mutation fail on values without an overread.
  if (!check_host_cursor(0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, -11.25, 417, 12.625,
                         -13.875, 14.5)) return 3;
  return forward_cursor(-0.5, 0, 1.625f, 523, -2.875);
}
