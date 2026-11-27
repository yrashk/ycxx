struct Operand {
    constexpr operator unsigned long long() const { return 8ULL; }
};

static_assert(__builtin_clzll(Operand{}) == 60);

constexpr int count_from_double(double value) { return __builtin_clzll(value); }
static_assert(count_from_double(2.75) == 62);
