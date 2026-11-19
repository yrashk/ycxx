// [expr.arith.conv], [conv.rank]: with a floating operand, the other operand converts to the
// floating type of greater rank: int op double is double, float op double is double, unsigned or
// long op float is float, and anything with long double is long double.
constexpr int kind(float) { return 1; }
constexpr int kind(double) { return 2; }
constexpr int kind(long double) { return 3; }
static_assert(kind(1 + 1.0) == 2);
static_assert(kind(1.0 - 1) == 2);
static_assert(kind(1.0f * 2.0) == 2);
static_assert(kind(1u + 1.0f) == 1);
static_assert(kind(1L / 2.0f) == 1);
static_assert(kind(1ULL - 1.0f) == 1);
static_assert(kind('a' + 1.0f) == 1);
static_assert(kind(true + 1.0f) == 1);
static_assert(kind(1.0f + 1.0f) == 1);
static_assert(kind(1.0f + 1.0L) == 3);
static_assert(kind(1.0 * 1.0L) == 3);
static_assert(kind(1 + 1.0L) == 3);
static_assert(1 + 0.5 == 1.5 && 7 / 2.0 == 3.5 && 1u + 0.5f == 1.5f);
static_assert(sizeof(1.0f + 1) == sizeof(float) && sizeof(1 + 1.0) == sizeof(double));
