// The C library's declarations as each target's <string.h> writes them: glibc's `__THROW` is
// `noexcept(true)` in C++, the macOS SDK's have no exception specification.
#ifdef __linux__
#define C_THROW noexcept(true)
#else
#define C_THROW
#endif
using Size = decltype(sizeof 0);
extern "C" int memcmp(const void*, const void*, Size) C_THROW;
extern "C" void* memchr(const void*, int, Size) C_THROW;
extern "C" char* strchr(const char*, int) C_THROW;
extern "C" char* strpbrk(const char*, const char*) C_THROW;
extern "C" char* strrchr(const char*, int) C_THROW;
extern "C" char* strstr(const char*, const char*) C_THROW;
extern "C" void* memset(void*, int, Size) C_THROW;
extern "C" void* memcpy(void*, const void*, Size) C_THROW;
extern "C" void* memmove(void*, const void*, Size) C_THROW;

int written(const char* a, const char* b, Size n) { return memcmp(a, b, n) + (memchr(a, 'x', n) != nullptr); }
int builtin(const char* a, const char* b, Size n) {
  return __builtin_memcmp(a, b, n) + (__builtin_memchr(a, 'x', n) != nullptr);
}
int written_search(const char* a, const char* b) {
  return (strchr(a, 'x') != nullptr) + (strrchr(a, 'x') != nullptr) + (strpbrk(a, b) != nullptr) +
         (strstr(a, b) != nullptr);
}
int builtin_search(const char* a, const char* b) {
  return (__builtin_strchr(a, 'x') != nullptr) + (__builtin_strrchr(a, 'x') != nullptr) +
         (__builtin_strpbrk(a, b) != nullptr) + (__builtin_strstr(a, b) != nullptr);
}
void* written_copy(char* d, const char* s, Size n) { return memset(memmove(memcpy(d, s, n), s, n), 0, n); }
void* builtin_copy(char* d, const char* s, Size n) {
  return __builtin_memset(__builtin_memmove(__builtin_memcpy(d, s, n), s, n), 0, n);
}
