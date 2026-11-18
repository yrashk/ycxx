// [expr.const], [expr.pre], [expr.mul], [expr.shift]: an operation with undefined behavior
// (signed overflow, division by zero, INT_MIN / -1, a shift count out of range) makes the
// expression not a core constant expression. Each undefined operation is on its marked line.
constexpr int zero() { return 0; }
constexpr int minus_one() { return -1; }
constexpr int big_shift() { return 200; }
constexpr int int_max() {
  unsigned u = 0;
  u = u - 1;
  int m = u / 2;
  return m;
}
constexpr int int_min() { return -int_max() - 1; }
static_assert(int_max() > 0 && int_min() < 0);
static_assert(int_min() / 1 == int_min() && 7 / minus_one() == -7);
static_assert(int_max() + 1 != 0); //! error
static_assert(int_min() - 1 != 0); //! error
static_assert(int_max() * 2 != 0); //! error
static_assert(-int_min() != 0); //! error
static_assert(1 / zero() == 0); //! error
static_assert(1 % zero() == 0); //! error
static_assert(int_min() / minus_one() != 0); //! error
static_assert(int_min() % minus_one() == 0); //! error
static_assert((1 << minus_one()) == 0); //! error
static_assert((1 << big_shift()) == 0); //! error
static_assert((1 >> minus_one()) == 0); //! error
static_assert((1 >> big_shift()) == 0); //! error
constexpr int doubling() { int x = 1; while (x > 0) x = x * 2; return x; } static_assert(doubling()); //! error
