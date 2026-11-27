template<class T> struct is_int { static constexpr bool value = false; };
template<> struct is_int<int> { static constexpr bool value = true; };

static_assert(~0ULL == 0xffffffffffffffffULL);
static_assert(is_int<decltype(__builtin_clzll(1ULL))>::value);
static_assert(__builtin_clzll(1ULL) == 63);
static_assert(__builtin_clzll((1ULL << 32) - 1) == 32);
static_assert(__builtin_clzll(1ULL << 32) == 31);
static_assert(__builtin_clzll(1ULL << 62) == 1);
static_assert(__builtin_clzll(1ULL << 63) == 0);
static_assert(__builtin_clzll(~0ULL) == 0);

constexpr unsigned char small = 2;
static_assert(__builtin_clzll(small) == 62);
static_assert(__builtin_clzll(-1) == 0);
