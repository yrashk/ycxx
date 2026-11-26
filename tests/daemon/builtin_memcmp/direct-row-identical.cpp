constexpr char a[2][2] = {{1, 2}, {3, 4}};
static_assert(__builtin_memcmp(a[1], a[1], 3) == 0);
