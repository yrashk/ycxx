// [expr.cond]/7.2, [expr.arith.conv]: when the second and third operands of ?: are arithmetic, the
// usual arithmetic conversions give the result type: int and double give double, float and double
// give double, float and char give float.
constexpr int kind(float) { return 1; }
constexpr int kind(double) { return 2; }
constexpr int kind(int) { return 4; }
constexpr bool yes = true;
constexpr bool no = false;
static_assert(kind(yes ? 1 : 2.0) == 2);
static_assert(kind(no ? 1.0f : 2.0) == 2);
static_assert(kind(yes ? 1.0f : 'c') == 1);
static_assert(kind(yes ? 1 : 2) == 4);
static_assert((yes ? 1 : 2.5) == 1.0);
static_assert((no ? 1 : 2.5) == 2.5);
static_assert((no ? 3 : 0.5f) == 0.5f);
