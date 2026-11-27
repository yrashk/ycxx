// The wider argument converts to unsigned int before the count.
constexpr int count_wide(unsigned long long input) {
  return __builtin_clz(input);
}

static_assert(count_wide(0x8000000000000100ULL) == 23);
