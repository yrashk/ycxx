// __builtin_strchr, __builtin_strpbrk, __builtin_strrchr, __builtin_strstr, __builtin_memset,
// __builtin_memcpy and __builtin_memmove behave as the C library's functions
// (docs/EXTENSIONS.md): searched characters convert to char, the terminating NUL can be found,
// memset stores its value converted to unsigned char, and memmove copies overlapping ranges.
using Size = decltype(sizeof(0));

// As libycxx's `<cstring>` wraps them (`include/cstring`).
inline const char* find(const char* s, int c) noexcept { return __builtin_strchr(s, c); }
inline char* find(char* s, int c) noexcept { return __builtin_strchr(s, c); }

template <class T>
T* fill(T* p, int value, Size n) {
  return static_cast<T*>(__builtin_memset(p, value, n * sizeof(T)));
}

int main(int argc, char**) {
  // `argc` keeps the lengths and values out of constant folding.
  const int one = argc;
  char s[] = "abcabc";
  const char* const cs = s;

  if (__builtin_strchr(s, 'c' * one) != s + 2) return 1;
  if (__builtin_strchr(cs, 'z' * one) != nullptr) return 2;
  if (__builtin_strchr(cs, 0 * one) != s + 6) return 3;          // the terminating NUL
  if (__builtin_strchr(cs, ('b' + 256) * one) != s + 1) return 4; // converted to char
  if (__builtin_strrchr(s, 'a' * one) != s + 3) return 5;
  if (__builtin_strrchr(cs, 'z' * one) != nullptr) return 6;
  if (__builtin_strrchr(cs, 0 * one) != s + 6) return 7;
  if (__builtin_strpbrk(s, "cb" + (one - 1)) != s + 1) return 8;
  if (__builtin_strpbrk(cs, "xyz" + (one - 1)) != nullptr) return 9;
  if (__builtin_strpbrk(cs, "" + (one - 1)) != nullptr) return 10;
  if (__builtin_strstr(s, "ca" + (one - 1)) != s + 2) return 11;
  if (__builtin_strstr(cs, "cba" + (one - 1)) != nullptr) return 12;
  if (__builtin_strstr(cs, "" + (one - 1)) != s) return 13;       // the empty string is at s

  // The `char*` result writes through, as C's does.
  *__builtin_strchr(cs, 'b' * one) = 'B';
  if (s[1] != 'B' || find(cs, 'B') != s + 1) return 14;
  *find(s, 'B') = 'b';

  char d[8];
  if (__builtin_memset(d, 'x' * one, sizeof d) != d) return 20;
  if (d[0] != 'x' || d[7] != 'x') return 21;
  __builtin_memset(d + 1, (0x100 + '-') * one, 2 * one);  // converted to unsigned char
  if (d[0] != 'x' || d[1] != '-' || d[2] != '-' || d[3] != 'x') return 22;
  if (__builtin_memcpy(d, s, 7 * one) != d) return 23;
  if (__builtin_strstr(d, "abcabc") != d || d[6] != '\0') return 24;
  if (__builtin_memcpy(d, s, 0 * one) != d) return 25;

  // memmove copies as if through a temporary, in either direction.
  if (__builtin_memmove(s + 1, s, 3 * one) != s + 1) return 30;  // "aabcbc"
  if (s[0] != 'a' || s[1] != 'a' || s[2] != 'b' || s[3] != 'c' || s[4] != 'b') return 31;
  if (__builtin_memmove(s, s + 2, 4 * one) != s) return 32;       // "bcbcbc"
  if (__builtin_strstr(s, "bcbcbc") != s) return 33;

  int words[4] = {1, 2, 3, 4};
  if (fill(words, 0, 4 * one) != words || words[0] != 0 || words[3] != 0) return 40;
  int more[4] = {5, 6, 7, 8};
  __builtin_memcpy(words, more, sizeof more);
  if (words[0] != 5 || words[3] != 8) return 41;

  // Each argument is evaluated once.
  int calls = 0;
  char* p = s;
  if (__builtin_strchr(p++, (++calls, 'c')) != s + 1 || p != s + 1 || calls != 1) return 50;
  if (__builtin_memset(p++, (calls += 10, 'q'), (++calls, 1)) != s + 1 || p != s + 2 || calls != 12) return 51;

  // The reduction from #1093.
  char t[] = "abcabc";
  char u[4];
  __builtin_memset(u, 0, sizeof u);
  __builtin_memcpy(u, t, 3);
  if (__builtin_strchr(t, 'c') != t + 2 || __builtin_strrchr(t, 'a') != t + 3 || u[2] != 'c') return 60;
  if (__builtin_strstr(t, "ca") != t + 2 || __builtin_strpbrk(t, "cb") != t + 1) return 61;
  __builtin_memmove(t + 1, t, 3);  // "aabcbc"
  return t[3] == 'c' ? 0 : 62;
}
