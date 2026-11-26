constexpr char a[2][2] = {{1, 2}, {3, 4}};
constexpr const void* erased = a[0] + 2;
constexpr const char* rebound = static_cast<const char*>(erased);
constexpr const void* direct_erased = a[1];
constexpr const char* direct = static_cast<const char*>(direct_erased);
static_assert(__builtin_memcmp(rebound, direct, 3) == 0);
