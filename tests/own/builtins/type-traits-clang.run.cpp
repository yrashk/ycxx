// #211: the type-trait built-ins only Clang has among the oracles (GCC spells `__remove_reference`
// for `__remove_reference_t`, and has none of the others), and a transformation of a dependent
// type in a function template's signature, mangled `u11__remove_cvIT_E` as Clang does
// (docs/EXTENSIONS.md).
// XFAIL: gcc GCC has no __is_void, __is_integral, __is_floating_point, __is_arithmetic, __is_lvalue_reference, __is_rvalue_reference, __remove_const, __remove_volatile, __remove_reference_t, and rejects built-in traits in signatures
// EXIT: 0
enum E { e0 };
static_assert(__is_void(void) && !__is_void(int) && __is_integral(bool) && __is_integral(char) && !__is_integral(E));
static_assert(__is_floating_point(double) && !__is_floating_point(int) && __is_arithmetic(int) && __is_arithmetic(float));
static_assert(__is_lvalue_reference(int&) && !__is_lvalue_reference(int&&) && __is_rvalue_reference(int&&));
static_assert(__is_same(__remove_const(const volatile int), volatile int) && __is_same(__remove_volatile(const volatile int), const int));
static_assert(__is_same(__remove_reference_t(int&&), int));
template<class T> int f(__remove_cv(T)* p) { return *p; }
int main() {
  int x = 4;
  return f<const int>(&x) - 4;
}
