// __builtin_memchr and __builtin_memcmp in constant expressions do not read past an object
// (docs/EXTENSIONS.md). Cases where an oracle disagrees are in memchr-memcmp-whole-range (Clang)
// and memchr-memcmp-past-member (GCC).
constexpr char text[] = "abcabc";

static_assert(__builtin_memchr(text, 'z', 8) == nullptr); //! error
static_assert(__builtin_memchr(text + 7, 'a', 1) == nullptr); //! error
static_assert(__builtin_memchr(nullptr, 'a', 1) == nullptr); //! error
static_assert(__builtin_memcmp(nullptr, text, 1) < 0); //! error

// A row's bytes end with the row.
constexpr char grid[2][3] = {{'a', 'b', 'c'}, {'d', 'e', 'f'}};
static_assert(__builtin_memchr(grid[0], 'e', 6) == &grid[1][1]); //! error
