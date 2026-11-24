// XFAIL: gcc GCC 16 reads a class's bytes on past the member into the next one, and folds memcmp of an array with itself without reading it. Each read here is past the end of its object ([expr.add], [basic.compound]); Clang agrees with ycxx.
// __builtin_memchr and __builtin_memcmp in constant expressions read only within the object their
// pointer designates: a member's bytes end with the member (docs/EXTENSIONS.md).
constexpr char text[] = "abcabc";
static_assert(__builtin_memcmp(text, text, 8) == 0); //! error

struct Record {
  char letter;
  char pair[2];
};
constexpr Record record{'x', {'y', 'z'}};
static_assert(__builtin_memchr(&record.letter, 'y', 2) == record.pair); //! error
static_assert(__builtin_memchr(&record.letter, 'z', 2) == nullptr); //! error
static_assert(__builtin_memcmp(&record.letter, "xx", 2) != 0); //! error
