// __builtin_memchr and __builtin_memcmp in constant expressions do not read past an object
// (docs/EXTENSIONS.md). memchr reads sequentially and stops at a match, as C's memchr is
// specified to; memcmp needs both objects to hold the n bytes, since C's memcmp may read them all.
constexpr char text[] = "abcabc";

static_assert(__builtin_memchr(text, 'z', 8) == nullptr); //! error
static_assert(__builtin_memchr(text + 7, 'a', 1) == nullptr); //! error
static_assert(__builtin_memchr(nullptr, 'a', 1) == nullptr); //! error
static_assert(__builtin_memcmp(text, text, 8) == 0); //! error
static_assert(__builtin_memcmp(text, "abd", 7) < 0); //! error
static_assert(__builtin_memcmp("abd", text, 7) > 0); //! error
static_assert(__builtin_memcmp(nullptr, text, 1) < 0); //! error

// A member's bytes end with the member, and a row's with the row.
struct Record {
  char letter;
  char pair[2];
};
constexpr Record record{'x', {'y', 'z'}};
static_assert(__builtin_memchr(&record.letter, 'y', 2) == record.pair); //! error
static_assert(__builtin_memchr(&record.letter, 'z', 2) == nullptr); //! error
static_assert(__builtin_memcmp(&record.letter, "xx", 2) != 0); //! error
constexpr char grid[2][3] = {{'a', 'b', 'c'}, {'d', 'e', 'f'}};
static_assert(__builtin_memchr(grid[0], 'e', 6) == &grid[1][1]); //! error
