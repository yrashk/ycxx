constexpr char a[2][2] = {{1, 2}, {3, 4}};
constexpr const char* q = a[1];
static_assert(__builtin_memcmp(a[1], q, 3) == 0);
