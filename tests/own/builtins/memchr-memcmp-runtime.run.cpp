// __builtin_memchr and __builtin_memcmp behave as the C library's memchr and memcmp
// (docs/EXTENSIONS.md): bytes compare as unsigned char, the searched value is converted to
// unsigned char, and a zero length finds nothing and compares equal.
using Size = decltype(sizeof(0));

// As libycxx's run-time wrappers take them (`include/ycxx/core/mem_builtins.hpp`).
const void* find(const void* s, int c, Size n) noexcept { return __builtin_memchr(s, c, n); }
int compare(const void* a, const void* b, Size n) noexcept { return __builtin_memcmp(a, b, n); }

template <class T>
const T* find_in(const T* s, int c, Size n) {
  return static_cast<const T*>(__builtin_memchr(s, c, n * sizeof(T)));
}

int main(int argc, char**) {
  // `argc` keeps the lengths and values out of constant folding.
  const Size one = argc;
  char text[] = "abcabc\0x\xff";
  const char* const s = text;
  if (find(s, 'c', 3 * one) != s + 2) return 1;
  if (find(s + 3, 'c', 3 * one) != s + 5) return 2;
  if (find(s, 'z', 6 * one) != nullptr) return 3;
  if (find(s, 'a', 0 * one) != nullptr) return 4;
  if (find(s, '\0', 10 * one) != s + 6) return 5;  // a NUL does not end the search
  if (find(s, 'x', 10 * one) != s + 7) return 6;
  if (find(s, 0xff, 10 * one) != s + 8) return 7;   // converted to unsigned char
  if (find(s, -1, 10 * one) != s + 8) return 8;
  if (find(s, 'b' + 256, 10 * one) != s + 1) return 9;

  void* found = __builtin_memchr(text, 'b', 3 * one);  // the C signature: `void*`
  if (static_cast<char*>(found) != text + 1) return 10;
  *static_cast<char*>(found) = 'B';
  if (text[1] != 'B') return 11;
  text[1] = 'b';

  if (compare(s, "abc", 3 * one) != 0) return 20;
  if (compare(s, "abd", 3 * one) >= 0) return 21;
  if (compare("abd", s, 3 * one) <= 0) return 22;
  if (compare(s, "zzz", 0 * one) != 0) return 23;
  if (compare(s + 7, "x", 1 * one) != 0) return 24;
  if (compare(s + 8, "a", 1 * one) <= 0) return 25;  // 0xff > 'a' as unsigned char
  if (compare("a", s + 8, 1 * one) >= 0) return 26;
  if (compare(s, "abcabc\0y", 8 * one) >= 0) return 27;  // past a NUL

  // Each argument is evaluated once.
  Size calls = 0;
  const char* p = s;
  if (__builtin_memchr(p++, (++calls, 'a'), (calls += 10, 1 * one)) != s || p != s + 1 || calls != 11) return 30;
  if (__builtin_memcmp(p++, s + 1, (++calls, one)) != 0 || p != s + 2 || calls != 12) return 31;

  const int words[] = {1, 2, 3, 0x100};
  if (find_in(words, 3, 4 * one) != words + 2) return 40;

  // The libycxx reduction from #776.
  const char abc[] = "abc";
  return __builtin_memchr(abc, 'b', 3) == abc + 1 && __builtin_memcmp(abc, "abd", 3) < 0 ? 0 : 50;
}
