// GNU bit operation built-ins: int __builtin_ctz(unsigned int).
enum Bits : unsigned int { eighth_bit = 0x100u };

constexpr int count_after_narrowing(unsigned long long value) {
    return __builtin_ctz(value);
}

static_assert(__is_same(decltype(__builtin_ctz(1u)), int));
static_assert(__builtin_ctz(1u) == 0);
static_assert(__builtin_ctz(0xa0u) == 5);
static_assert(__builtin_ctz(0x80000000u) == 31);
static_assert(__builtin_ctz(-8) == 3);
static_assert(__builtin_ctz(true) == 0);
static_assert(__builtin_ctz(eighth_bit) == 8);
static_assert(count_after_narrowing(0x100000800ULL) == 11);
