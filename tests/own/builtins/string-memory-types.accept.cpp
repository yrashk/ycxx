// __builtin_strchr, __builtin_strpbrk, __builtin_strrchr, __builtin_strstr, __builtin_memset,
// __builtin_memcpy and __builtin_memmove have the C library's signatures (docs/EXTENSIONS.md):
// the search functions give `char*` even from a `const char*`, as C's do, and the memory
// functions give `void*`. Clang and GCC agree.
#if !__has_builtin(__builtin_strchr) || !__has_builtin(__builtin_strpbrk) || !__has_builtin(__builtin_strrchr)
#error a string search builtin is unavailable
#endif
#if !__has_builtin(__builtin_strstr) || !__has_builtin(__builtin_memset) || !__has_builtin(__builtin_memcpy)
#error a string or memory builtin is unavailable
#endif
#if !__has_builtin(__builtin_memmove)
#error __builtin_memmove is unavailable
#endif

using Size = decltype(sizeof(0));

const char text[] = "abc";
char buffer[4];
static_assert(__is_same(decltype(__builtin_strchr(text, 'b')), char*));
static_assert(__is_same(decltype(__builtin_strchr("abc", 'b')), char*));
static_assert(__is_same(decltype(__builtin_strpbrk(text, "cb")), char*));
static_assert(__is_same(decltype(__builtin_strrchr(text, 'b')), char*));
static_assert(__is_same(decltype(__builtin_strstr(text, "bc")), char*));
static_assert(__is_same(decltype(__builtin_memset(buffer, 0, 4)), void*));
static_assert(__is_same(decltype(__builtin_memcpy(buffer, text, 4)), void*));
static_assert(__is_same(decltype(__builtin_memmove(buffer, text, 4)), void*));

// libycxx's `<cstring>` const-correct pairs: the `char*` result serves both.
inline const char* find(const char* s, int c) noexcept { return __builtin_strchr(s, c); }
inline char* find(char* s, int c) noexcept { return __builtin_strchr(s, c); }
inline const char* find_any(const char* s, const char* set) noexcept { return __builtin_strpbrk(s, set); }
inline char* find_any(char* s, const char* set) noexcept { return __builtin_strpbrk(s, set); }
inline char* find_last(char* s, int c) noexcept { return __builtin_strrchr(s, c); }
inline char* find_text(char* s, const char* t) noexcept { return __builtin_strstr(s, t); }

// Any object pointer converts to `void*` or `const void*`; the value and length convert as
// arguments do.
struct Pod { int a; char b; };
void conversions(Pod* p, const Pod* q, unsigned char c, long n, short m) {
  (void)__builtin_memset(p, c, n);
  (void)__builtin_memset(&p->b, 'x', m);
  (void)__builtin_memcpy(p, q, sizeof(Pod));
  (void)__builtin_memmove(&p->a, &q->a, sizeof(int));
  (void)__builtin_memcpy(nullptr, nullptr, 0);
  (void)__builtin_strchr(&p->b, c);
  (void)__builtin_strrchr(&p->b, n);
}

// In a template, a dependent argument waits for instantiation.
template <class T>
T* fill(T* p, int value, Size n) {
  return static_cast<T*>(__builtin_memset(p, value, n * sizeof(T)));
}
template <class T>
T* copy(T* d, const T* s, Size n) {
  return static_cast<T*>(__builtin_memcpy(__builtin_memmove(d, s, n * sizeof(T)), s, n * sizeof(T)));
}
template <class C>
C* search(C* s, C c) {
  return __builtin_strchr(s, c);
}
bool instantiate(char* s, int* w) { return fill(w, 0, 1) && copy(w, w, 1) && search(s, 'a'); }

// An unevaluated operand has the result type without a call.
static_assert(sizeof(__builtin_memcpy(buffer, text, 4)) == sizeof(void*));
static_assert(sizeof(__builtin_strstr(text, "c")) == sizeof(char*));
