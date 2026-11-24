// A written declaration whose exception specification differs from the target's <string.h>
// (#1095): `noexcept` on macOS, none on Linux. It and the builtin's implicit declaration are two
// entities with one C symbol.
#ifdef __linux__
#define C_OTHER_THROW
#else
#define C_OTHER_THROW noexcept(true)
#endif
using Size = decltype(sizeof 0);
extern "C" int memcmp(const void*, const void*, Size) C_OTHER_THROW;
extern "C" void* memchr(const void*, int, Size) C_OTHER_THROW;

int written(const char* a, const char* b, Size n) { return memcmp(a, b, n) + (memchr(a, 'x', n) != nullptr); }
int builtin(const char* a, const char* b, Size n) {
  return __builtin_memcmp(a, b, n) + (__builtin_memchr(a, 'x', n) != nullptr);
}
