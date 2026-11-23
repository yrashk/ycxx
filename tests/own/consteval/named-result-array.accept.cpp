// #975 [expr.const], [class.temporary]
#include "named-result-array.hpp"

static_assert(make_text().first() == 'a');
static_assert(*make_text().middle() == 'b');

consteval Text relay_result() {
  Text value = make_text();
  return value;
}

static_assert(*relay_result().middle() == 'b');

consteval bool read_result() {
  const Text value = make_text();
  const char* pointer = value.middle();
  return *pointer == 'b' && pointer[1] == 'c' && pointer[2] == '\0';
}

static_assert(read_result());
