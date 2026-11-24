// XFAIL: gcc GCC 16 reports the memcmp error inside the constexpr function rather than only at the static_assert, and accepts a memchr that reads an indeterminate byte before its match, which [expr.const] makes not constant.
// The bytes __builtin_memchr and __builtin_memcmp read in a constant expression must have values
// ([expr.const], [basic.indet]). Clang agrees.
constexpr int compare_indeterminate() {
  char a[3];
  a[0] = 'a';
  return __builtin_memcmp(a, "abc", 3);
}
static_assert(compare_indeterminate() == 0); //! error

constexpr const void* find_indeterminate() {
  char a[2];
  a[1] = 'b';
  return __builtin_memchr(a, 'b', 2);
}
static_assert(find_indeterminate() != nullptr); //! error
