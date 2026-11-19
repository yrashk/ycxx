// [expr.const] (usable in constant expressions), [dcl.constexpr]: unlike a const integer, a const
// double initialized with a constant is not usable in constant expressions; a constexpr one is.
const double c = 1.0;
constexpr double ce = 1.0;
double v = 1.0;
const int ci = 1;
static_assert(c == 1.0); //! error
constexpr double d = v; //! error
static_assert(ce == 1.0 && ci == 1);
constexpr double from_const_int = ci * 0.5;
static_assert(from_const_int == 0.5);
