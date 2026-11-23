// With no written declaration, the builtins reference the C library's functions.
using Size = decltype(sizeof 0);
int builtin(const char* a, const char* b, Size n) {
  return __builtin_memcmp(a, b, n) + (__builtin_memchr(a, 'x', n) != nullptr);
}
