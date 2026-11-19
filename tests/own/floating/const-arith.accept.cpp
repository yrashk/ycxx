// [expr.const], [expr.add], [expr.mul], [expr.unary.op]: floating +, -, *, / and unary minus in
// constant expressions give the correctly rounded IEEE results.
static_assert(0.5 + 0.25 == 0.75);
static_assert(0.1 + 0.2 != 0.3);
static_assert(0.1 + 0.2 == 0x1.3333333333334p-2);
static_assert(1.0 / 3 * 3 == 1.0);
static_assert(1.0 / 3 == 0x1.5555555555555p-2);
static_assert(2.0 / 3 == 0x1.5555555555555p-1);
static_assert(1.0f / 3 == 0x1.555556p-2f);
static_assert(1e16 + 1 == 1e16);
static_assert(-(-1.5) == 1.5 && -(0.25) == -0.25);
static_assert(+2.5 == 2.5);
static_assert(3.5 - 5 == -1.5);
static_assert(0.1 * 3 == 0x1.3333333333334p-2);
static_assert(1.5 * 1.5 == 2.25);
static_assert(0x1p-1022 / 4 == 0x1p-1024); // a subnormal result
static_assert(0x1p-1074 / 2 == 0.0);       // underflow to zero, a tie to even
constexpr double half = 0.5;
constexpr double quarter = half * half;
static_assert(quarter == 0.25);
