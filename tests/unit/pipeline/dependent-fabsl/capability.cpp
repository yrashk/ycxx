#ifndef EXPECT_FABSL
#error Pass EXPECT_FABSL as 0 or 1
#endif

#if __has_builtin(__builtin_fabsl) != EXPECT_FABSL
#error Unexpected fabsl capability
#endif
