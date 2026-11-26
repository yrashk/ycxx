// [dcl.type.decltype]: an unevaluated prvalue retains its exact type.
static_assert(__is_same(decltype(__builtin_nansl(("" ""))), long double));

template <bool UseBuiltin>
int discard_in_template(int value) {
  if constexpr (UseBuiltin) {
    (void)__builtin_nansl("");
  }
  return value;
}

int call_discarded_template(int value) {
  return discard_in_template<false>(value);
}

int discard_ordinary(int value) {
  if constexpr (false) {
    (void)__builtin_nansl("");
  }
  return value;
}
