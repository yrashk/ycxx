// #1077 Constant evaluation cannot store into an object whose lifetime began outside it.
using Intmax = __INTMAX_TYPE__;
Intmax outside = 0;

constexpr bool add_into(Intmax* result) {
  return __builtin_add_overflow(Intmax{3}, Intmax{4}, result);
}

static_assert(!add_into(&outside)); //! error
