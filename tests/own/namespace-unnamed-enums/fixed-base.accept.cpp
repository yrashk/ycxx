enum : signed char {
  fixed_origin = -4,
  fixed_derived = fixed_origin + 6,
  fixed_next,
};

static_assert(fixed_origin == -4);
static_assert(fixed_derived == 2);
static_assert(fixed_next == 3);
static_assert(__is_same(__underlying_type(decltype(fixed_next)), signed char));
static_assert(__is_same(decltype(+fixed_next), int));
