// [expr.mul]/4, [expr.const]: floating division by zero has undefined behavior, so it is not a
// core constant expression, whatever the dividend; division by a nonzero value is fine.
constexpr double zero() { return 0.0; }
constexpr float fzero() { return 0.0f; }
static_assert(1.0 / zero() > 0); //! error
static_assert(-1.0 / zero() < 0); //! error
static_assert(0.0 / zero() != 0); //! error
static_assert(1.0f / fzero() > 0); //! error
static_assert(1.0 / -zero() < 0); //! error
constexpr double divide(double a, double b) { return a / b; }
static_assert(divide(1.0, 0x1p-1000) == 0x1p1000);
static_assert(divide(0x1p-1074, 2.0) == 0.0);
static_assert(divide(0.0, 2.0) == 0.0);
