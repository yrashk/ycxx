// With no written declaration, the builtins reference the C library's functions.
using Size = decltype(sizeof 0);
int builtin(const char* a, const char* b, Size n) {
  return __builtin_memcmp(a, b, n) + (__builtin_memchr(a, 'x', n) != nullptr);
}
int search(const char* a, const char* b) {
  return (__builtin_strchr(a, 'x') != nullptr) + (__builtin_strrchr(a, 'x') != nullptr) +
         (__builtin_strpbrk(a, b) != nullptr) + (__builtin_strstr(a, b) != nullptr);
}
void* copy(char* d, const char* s, Size n) {
  return __builtin_memset(__builtin_memmove(__builtin_memcpy(d, s, n), s, n), 0, n);
}
