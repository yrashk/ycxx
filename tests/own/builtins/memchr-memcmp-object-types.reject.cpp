// XFAIL: gcc GCC 16 reads the object representation of objects of any type. ycxx's constant evaluation has no bytes for them and follows Clang 23, which reads only narrow character types; the builtins are extensions (docs/EXTENSIONS.md).
// In constant expressions __builtin_memchr and __builtin_memcmp read only objects of narrow
// character type (char, signed char, unsigned char, char8_t) or arrays of them, through a pointer
// to an element (docs/EXTENSIONS.md). Other objects have no byte values the evaluation can read.
constexpr int words[] = {1, 2, 3};
constexpr int same[] = {1, 2, 3};
static_assert(__builtin_memchr(words, 2, sizeof words) != nullptr); //! error
static_assert(__builtin_memcmp(words, same, sizeof words) == 0); //! error
static_assert(__builtin_memcmp("a", words, 1) != 0); //! error

constexpr bool flags[] = {true, false};
static_assert(__builtin_memchr(flags, 0, 2) == flags + 1); //! error

constexpr char16_t wide[] = {u'a', u'b'};
static_assert(__builtin_memchr(wide, 'a', 1) == wide); //! error

struct Pair {
  char a, b;
};
constexpr Pair pair{'a', 'b'};
static_assert(__builtin_memchr(&pair, 'a', 1) != nullptr); //! error

// A pointer to a whole array, or to a row of a two-dimensional one, does not point to a byte.
constexpr char text[] = "abc";
static_assert(__builtin_memchr(&text, 'a', 1) != nullptr); //! error
constexpr char grid[2][2] = {{'a', 'b'}, {'c', 'd'}};
static_assert(__builtin_memchr(grid, 'a', 1) != nullptr); //! error
