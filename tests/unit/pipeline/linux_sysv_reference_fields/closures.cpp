#include "abi.hpp"
#include "try_or_null.hpp"

static_assert(!__ycxx::__detail::__cfg::exceptions);

extern "C" int ref_captures(bool heapless) {
  int first = 10;
  int second = 30;
  void* one = __ycxx::__detail::__try_or_null(heapless, [&]() -> void* {
    first = first + 3;
    return &first;
  });
  if (one != (heapless ? nullptr : &first) || first != (heapless ? 10 : 13)) return 1;
  void* two = __ycxx::__detail::__try_or_null(heapless, [&]() -> void* {
    first = first + 2;
    second = second + first;
    return &second;
  });
  if (two != (heapless ? nullptr : &second)) return 2;
  if (first != (heapless ? 10 : 15) || second != (heapless ? 30 : 45)) return 3;
  return 0;
}

template<class F>
int inspect_parameter(F function, F* source, int tail) {
  if (&function == source) return -1;
  return function() + tail;
}

extern "C" int closure_parameter_objects() {
  int first = 1;
  int second = 10;
  auto one = [&] {
    first = first + 1;
    return first;
  };
  auto two = [&] {
    first = first + 2;
    second = second + 1;
    return first + second;
  };
  if (inspect_parameter(one, &one, 7) != 9 || first != 2) return 1;
  if (inspect_parameter(two, &two, 13) != 28 || first != 4 || second != 11) return 2;
  return 0;
}

template<class F>
long invoke_value(F function, long tail) {
  return function() + tail;
}

extern "C" int value_captures() {
  long first = 11;
  long second = 31;
  auto one = [first] { return first; };
  auto two = [first, second] { return first + second; };
  first = 89;
  second = 97;
  if (invoke_value(one, 3L) != 14) return 1;
  if (invoke_value(two, 5L) != 47) return 2;
  return 0;
}
