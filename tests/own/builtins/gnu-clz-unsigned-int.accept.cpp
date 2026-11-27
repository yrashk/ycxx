// GNU __builtin_clz has an unsigned int operand and an int result.
static_assert(__builtin_clz(1u) == 31);
static_assert(__builtin_clz(0x10000u) == 15);
static_assert(__builtin_clz(0x80000000u) == 0);

using ClzResult = decltype(__builtin_clz(1u));
using ClzResult = int;
