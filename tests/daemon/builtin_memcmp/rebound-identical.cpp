constexpr char a[2][2] = {{1, 2}, {3, 4}};
constexpr const void* erased = a[0] + 2;
constexpr const char* rebound = static_cast<const char*>(erased);
static_assert(__builtin_memcmp(rebound, rebound, 3) == 0);
