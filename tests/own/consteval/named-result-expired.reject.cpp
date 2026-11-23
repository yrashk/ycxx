// #975 [expr.const], [basic.life]
#include "named-result-array.hpp"

constexpr bool expired_result() {
  const char* pointer = nullptr;
  {
    const Text value = make_text();
    pointer = value.middle();
  }
  // GCC locates the read and Clang locates the assertion.
  return *pointer == 'b'; } static_assert(expired_result()); //! error
