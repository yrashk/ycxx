constexpr int count(unsigned int value) {
    return __builtin_ctz(value);
}

static_assert(count(0u) == 32);
