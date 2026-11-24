// __builtin_memchr and __builtin_memcmp in constant expressions (docs/EXTENSIONS.md): they read
// arrays and objects of narrow character type, as unsigned char, under [expr.const]'s rules.
using Size = decltype(sizeof(0));

constexpr char text[] = "abcabc";

// memchr finds the first match among the first n bytes, and gives a pointer to it.
static_assert(__builtin_memchr(text, 'c', 6) == text + 2);
static_assert(__builtin_memchr(text + 3, 'c', 3) == text + 5);
static_assert(__builtin_memchr(text, 'z', 6) == nullptr);
static_assert(__builtin_memchr(text, 'c', 2) == nullptr);
static_assert(__builtin_memchr(text, 'a', 0) == nullptr);
static_assert(__builtin_memchr(text, '\0', 7) == text + 6);  // a NUL does not end the search
static_assert(__builtin_memchr("abc", 'b', 3) != nullptr);
static_assert(__builtin_memchr(nullptr, 'a', 0) == nullptr);
static_assert(__builtin_memchr(text + 7, 'a', 0) == nullptr);  // nothing is read

// The search stops at the match ([cstring.syn] and C's memchr): bytes after it are not read.
static_assert(__builtin_memchr(text, 'a', 100) == text);
static_assert(__builtin_memchr(text, 'b', Size(-1)) == text + 1);

// The value is converted to unsigned char, and so is each byte.
constexpr unsigned char high[] = {0xff, 0x01};
static_assert(__builtin_memchr(high, -1, 2) == high);
static_assert(__builtin_memchr(high, 0x101, 2) == high + 1);
constexpr signed char negative[] = {-1, 1};
static_assert(__builtin_memchr(negative, 0xff, 2) == negative);
constexpr char8_t utf8[] = u8"ab";
static_assert(__builtin_memchr(utf8, 'b', 2) == utf8 + 1);

// The result points to the byte: it converts back and can be read and compared.
constexpr const char* found = static_cast<const char*>(__builtin_memchr(text, 'b', 6));
static_assert(found == text + 1 && *found == 'b' && found - text == 1);

// memcmp compares the first n bytes as unsigned char.
static_assert(__builtin_memcmp("abc", "abd", 3) < 0);
static_assert(__builtin_memcmp("abd", "abc", 3) > 0);
static_assert(__builtin_memcmp("abc", "abd", 2) == 0);
static_assert(__builtin_memcmp(text, text + 3, 3) == 0);
static_assert(__builtin_memcmp("ab\0c", "ab\0d", 4) < 0);  // past a NUL
static_assert(__builtin_memcmp(nullptr, nullptr, 0) == 0);
static_assert(__builtin_memcmp(text + 7, text + 7, 0) == 0);
static_assert(__builtin_memcmp(high, "a", 1) > 0);  // 0xff > 'a'
static_assert(__builtin_memcmp(negative, "a", 1) > 0);
static_assert(__builtin_memcmp(utf8, "ab", 2) == 0);
static_assert(__builtin_memcmp(text, high, 1) < 0);  // the two need not have one type

// Objects of narrow character type that are not arrays, and arrays inside classes.
struct Record {
  int number;
  char letter;
  char pair[2];
};
constexpr Record record{1, 'x', {'y', 'z'}};
static_assert(__builtin_memchr(&record.letter, 'x', 1) == &record.letter);
static_assert(__builtin_memchr(record.pair, 'z', 2) == record.pair + 1);
static_assert(__builtin_memcmp(&record.letter, "x", 1) == 0);
static_assert(__builtin_memcmp(record.pair, "yz", 2) == 0);

// The examples of #1092, and local arrays of a constant evaluation.
constexpr int compare(const char* a, const char* b) { return __builtin_memcmp(a, b, 1); }
static_assert(compare("a", "b") < 0);
constexpr const void* hit = __builtin_memchr("abc", 'b', 3);
static_assert(hit != nullptr);

constexpr int locals() {
  char a[] = {'a', 'b', 'c'};
  const char b[] = {'a', 'b', 'd'};
  int r = __builtin_memcmp(a, b, 3);
  a[2] = 'd';
  r += 10 * (__builtin_memcmp(a, b, 3) == 0);
  r += 100 * (__builtin_memchr(a, 'd', 3) == a + 2);
  return r;
}
static_assert(locals() == 109);

// memcmp stops reading at the first difference, so a later byte need not have a value.
constexpr int partly_initialized() {
  char a[3];
  a[0] = 'a';
  return __builtin_memcmp(a, "xbc", 3);
}
static_assert(partly_initialized() < 0);

// In a template, evaluated once the arguments are known.
template <class T, Size N>
constexpr const T* find(const T (&s)[N], T value) {
  return static_cast<const T*>(__builtin_memchr(s, value, N));
}
static_assert(find(text, 'c') == text + 2);
