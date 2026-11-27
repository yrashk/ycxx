constexpr int leading_zeros_of_zero = __builtin_clzll(0ULL);

#ifdef EXPECT_CLZLL_ZERO
static_assert(leading_zeros_of_zero == EXPECT_CLZLL_ZERO);
#endif
