#include "base.hpp"

extern "C" const volatile B* observe_keyless_operand(const volatile B*);

extern "C" const volatile D* keyless_qualified_downcast(const volatile B* source) {
  using Result = decltype(dynamic_cast<const volatile D*>(observe_keyless_operand(source)));
  static_assert(__is_same(Result, const volatile D*));
  return dynamic_cast<const volatile D*>(observe_keyless_operand(source));
}
