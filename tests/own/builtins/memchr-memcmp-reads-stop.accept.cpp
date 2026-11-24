// XFAIL: gcc GCC 16 reads all n bytes before searching or comparing. C's memchr stops at the match ([cstring.syn]), and nothing reads past memcmp's first difference.
// __builtin_memchr reads the bytes in order and stops at the first match, as C specifies memchr,
// so n may exceed the object after it (docs/EXTENSIONS.md). Clang agrees.
using Size = decltype(sizeof(0));

constexpr char text[] = "abcabc";
static_assert(__builtin_memchr(text, 'a', 100) == text);
static_assert(__builtin_memchr(text, 'b', Size(-1)) == text + 1);

// memcmp stops reading at the first difference, so a later byte need not have a value (both
// objects must still hold all n bytes: memchr-memcmp-whole-range).
constexpr int partly_initialized() {
  char a[3];
  a[0] = 'a';
  return __builtin_memcmp(a, "xbc", 3);
}
static_assert(partly_initialized() < 0);
