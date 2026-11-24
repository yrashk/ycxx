// A program's own definition of the C function is the function the builtin calls.
#ifdef __linux__
#define C_THROW noexcept(true)
#else
#define C_THROW
#endif
using Size = decltype(sizeof 0);
extern "C" int memcmp(const void* a, const void* b, Size n) C_THROW {
  const auto* x = static_cast<const unsigned char*>(a);
  const auto* y = static_cast<const unsigned char*>(b);
  for (Size i = 0; i < n; ++i)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}
extern "C" void* memcpy(void* d, const void* s, Size n) C_THROW {
  auto* p = static_cast<unsigned char*>(d);
  const auto* q = static_cast<const unsigned char*>(s);
  for (Size i = 0; i < n; ++i) p[i] = q[i];
  return d;
}
extern "C" void* memset(void* d, int c, Size n) C_THROW {
  auto* p = static_cast<unsigned char*>(d);
  for (Size i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(c);
  return d;
}

int builtin(const char* a, const char* b, Size n) { return __builtin_memcmp(a, b, n); }
void* copy(char* d, const char* s, Size n) { return __builtin_memcpy(d, s, n); }
void* fill(char* d, Size n) { return __builtin_memset(d, 0, n); }
