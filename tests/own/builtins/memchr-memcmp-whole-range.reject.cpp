// XFAIL: clang Clang 23 checks memcmp's bounds only as it reads, and stops at the first difference. ycxx requires all n bytes, as C's memcmp may read them all ([cstring.syn]); [expr.const] leaves library undefined behaviour unspecified. GCC 16 agrees with ycxx.
// __builtin_memcmp in a constant expression needs both objects to hold all n bytes, even when the
// bytes differ before the end (docs/EXTENSIONS.md).
constexpr char text[] = "abcabc";

static_assert(__builtin_memcmp(text, "abd", 7) < 0); //! error
static_assert(__builtin_memcmp("abd", text, 7) > 0); //! error
