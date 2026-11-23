// __builtin_memchr and __builtin_memcmp have the C library's signatures (docs/EXTENSIONS.md):
// `void* memchr(const void*, int, size_t)` and `int memcmp(const void*, const void*, size_t)`.
#if !__has_builtin(__builtin_memchr)
#error __builtin_memchr is unavailable
#endif
#if !__has_builtin(__builtin_memcmp)
#error __builtin_memcmp is unavailable
#endif

using Size = decltype(sizeof(0));

static_assert(__is_same(decltype(__builtin_memchr("abc", 'b', 3)), void*));
static_assert(__is_same(decltype(__builtin_memcmp("abc", "abd", 3)), int));

// Any object pointer converts to `const void*`; the value and length convert as arguments.
struct Pod { int a; char b; };
void conversions(Pod* p, unsigned char c, long n, short m) {
  (void)__builtin_memchr(p, c, n);
  (void)__builtin_memchr(&p->b, 'x', m);
  (void)__builtin_memcmp(p, &p->a, sizeof(Pod));
  (void)__builtin_memcmp(nullptr, nullptr, 0);
}

// In a template, a dependent argument waits for instantiation.
template <class T>
const T* find(const T* s, T value, Size n) {
  return static_cast<const T*>(__builtin_memchr(s, value, n));
}
template <class T>
bool equal(const T* a, const T* b, Size n) {
  return __builtin_memcmp(a, b, n * sizeof(T)) == 0;
}
bool instantiate(const char* s, const int* w) { return find(s, 'a', 1) && equal(w, w, 1); }

// An unevaluated operand has the result type without a call.
static_assert(sizeof(__builtin_memcmp("a", "b", 1)) == sizeof(int));
