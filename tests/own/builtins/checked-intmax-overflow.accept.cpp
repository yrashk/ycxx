// #1077 Checked signed arithmetic reports overflow and stores the converted result.
using Intmax = __INTMAX_TYPE__;
constexpr Intmax maximum = __INTMAX_MAX__;
constexpr Intmax minimum = -maximum - 1;

consteval bool add_matches(Intmax left, Intmax right, Intmax expected, bool overflowed) {
  Intmax result;
  auto overflow = __builtin_add_overflow(left, right, &result);
  static_assert(__is_same(decltype(overflow), bool));
  return result == expected && overflow == overflowed;
}

consteval bool multiply_matches(Intmax left, Intmax right, Intmax expected, bool overflowed) {
  Intmax result;
  auto overflow = __builtin_mul_overflow(left, right, &result);
  static_assert(__is_same(decltype(overflow), bool));
  return result == expected && overflow == overflowed;
}

static_assert(add_matches(maximum - 1, 1, maximum, false));
static_assert(add_matches(maximum, 2, minimum + 1, true));
static_assert(add_matches(minimum, minimum, 0, true));
static_assert(multiply_matches(-8, 3, -24, false));
static_assert(multiply_matches(minimum, 1, minimum, false));
static_assert(multiply_matches(maximum, 3, maximum - 2, true));
static_assert(multiply_matches(minimum, 2, 0, true));
static_assert(multiply_matches(minimum, -1, minimum, true));

consteval bool checked_product_plus_one(Intmax left, Intmax right, Intmax expected_product,
                                        Intmax expected_sum, bool overflowed) {
  Intmax product;
  Intmax sum = 71;
  bool overflow = __builtin_mul_overflow(left, right, &product) ||
                  __builtin_add_overflow(product, Intmax{1}, &sum);
  return overflow == overflowed && product == expected_product && sum == expected_sum;
}

static_assert(checked_product_plus_one(6, -5, -30, -29, false));
static_assert(checked_product_plus_one(maximum, 3, maximum - 2, 71, true));
static_assert(checked_product_plus_one(maximum, 1, maximum, minimum, true));
