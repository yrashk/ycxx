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

int written(const char* a, const char* b, Size n) { return memcmp(a, b, n) + (memchr(a, 'x', n) != nullptr); }
int builtin(const char* a, const char* b, Size n) {
  return __builtin_memcmp(a, b, n) + (__builtin_memchr(a, 'x', n) != nullptr);
}
