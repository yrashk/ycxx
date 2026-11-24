// The bytes __builtin_memchr and __builtin_memcmp read in a constant expression follow
// [expr.const]'s rules for reads: initialized, and usable in constant expressions.
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

char mutable_text[] = "abc";
static_assert(__builtin_memchr(mutable_text, 'b', 3) != nullptr); //! error
static_assert(__builtin_memcmp(mutable_text, "abc", 3) == 0); //! error

// A user's own `memchr` is still not constexpr.
extern "C" const void* memchr(const void*, int, decltype(sizeof(0)));
static_assert(memchr("abc", 'b', 3) != nullptr); //! error
