constexpr char a[2][2] = {{1, 2}, {3, 4}};
constexpr const void* erased = a[1];
constexpr const char* rebound = static_cast<const char*>(erased);
static_assert(*rebound == 3);
static_assert(__builtin_memcmp(rebound, a[1], 2) == 0);
