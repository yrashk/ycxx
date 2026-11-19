// [expr.rel], [expr.eq], [expr.arith.conv], [expr.const]: floating comparisons in constant
// expressions, after the usual arithmetic conversions; positive and negative zero compare equal.
static_assert(0.5 < 0.75 && 0.75 > 0.5);
static_assert(0.5 <= 0.5 && 0.5 >= 0.5);
static_assert(!(0.5 < 0.5) && !(0.5 > 0.5));
static_assert(-0.0 == 0.0 && !(-0.0 < 0.0));
static_assert(1.0f == 1.0 && 0.5f == 0.5);
static_assert(0.1f != 0.1 && 0.1f > 0.1);
static_assert(3 == 3.0 && 3 < 3.5 && -1 < -0.5);
static_assert(16777217 == 16777216.0f); // the int converts to float, which rounds it
static_assert(16777217 == 16777217.0);
static_assert(4294967295u > 4294967294.5);
static_assert(-1 < 0.5f);                // no unsigned trap: -1 converts to -1.0f
static_assert(0x1p-1074 > 0 && -0x1p-1074 < 0);
static_assert(1.0L > 0.5 && 0.25L == 0.25f);
constexpr bool between(double lo, double x, double hi) { return lo <= x && x < hi; }
static_assert(between(0.0, 0.0, 1.0) && !between(0.0, 1.0, 1.0));
