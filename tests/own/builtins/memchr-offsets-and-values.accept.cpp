// XFAIL: gcc GCC 16 folds memchr on an array at an offset to the wrong element, and rejects a negative value to search for, which converts to unsigned char.
// __builtin_memchr from a pointer into an array, and with a value converted to unsigned char as C
// specifies memchr ([cstring.syn]). Clang agrees.
constexpr char text[] = "abcabc";
static_assert(__builtin_memchr(text + 3, 'c', 3) == text + 5);

// A found pointer can be searched from again.
constexpr const char* found = static_cast<const char*>(__builtin_memchr(text, 'b', 6));
constexpr const char* again = static_cast<const char*>(__builtin_memchr(found + 1, 'b', 5));
static_assert(again == text + 4);

constexpr unsigned char high[] = {0xff, 0x01};
static_assert(__builtin_memchr(high, -1, 2) == high);
