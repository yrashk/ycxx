// [expr.pre]/4, [expr.const]: an operation whose result is not mathematically defined (infinity
// minus infinity, zero times infinity, infinity over infinity) is not a core constant expression.
constexpr double inf = __builtin_huge_val();
static_assert(inf - inf != 0); //! error
static_assert(0.0 * inf != 0); //! error
static_assert(inf / inf != 0); //! error
static_assert(inf + inf > 0);
static_assert(inf * 2 == inf);
static_assert(-inf < 0);
