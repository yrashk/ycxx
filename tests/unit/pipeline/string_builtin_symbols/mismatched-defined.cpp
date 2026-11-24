// A program's own definition whose exception specification differs from the target's
// <string.h> (#1095): the builtin's implicit declaration is another entity, and must still name
// the definition, not add an undefined symbol beside it.
#ifdef __linux__
#define C_OTHER_THROW
#else
#define C_OTHER_THROW noexcept(true)
#endif
using Size = decltype(sizeof 0);
extern "C" int memcmp(const void* a, const void* b, Size n) C_OTHER_THROW {
  const auto* x = static_cast<const unsigned char*>(a);
  const auto* y = static_cast<const unsigned char*>(b);
  for (Size i = 0; i < n; ++i)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}

int builtin(const char* a, const char* b, Size n) { return __builtin_memcmp(a, b, n); }
