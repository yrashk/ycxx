constexpr char a[2][2] = {{1, 2}, {3, 4}};
constexpr const char* q = a[1];
constexpr bool f() { return __builtin_memcmp(a[1], q, 3) == 0; }
static_assert(f());
