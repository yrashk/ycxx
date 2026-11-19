// [expr.const], [basic.fundamental]: the GNU builtins that <math.h> builds on (__builtin_fabs,
// __builtin_inf, __builtin_huge_val, __builtin_nan) used directly, in constant expressions:
// infinities order above every finite value, a NaN compares unequal to everything, itself
// included, and fabs clears the sign.
constexpr double inf = __builtin_inf();
constexpr float inff = __builtin_inff();
constexpr double nan = __builtin_nan("");
static_assert(inf > 1.7976931348623157e308 && -inf < -1.7976931348623157e308);
static_assert(inf == __builtin_huge_val());
static_assert(inff == __builtin_huge_valf() && inff == inf);
static_assert(inf + 1 == inf && inf * 2 == inf);
static_assert(nan != nan && !(nan == nan));
static_assert(!(nan < 0) && !(nan > 0) && !(nan <= nan) && !(nan >= nan));
static_assert(__builtin_nan("") != 0.0);
static_assert(__builtin_fabs(-2.5) == 2.5 && __builtin_fabs(2.5) == 2.5);
static_assert(__builtin_fabsf(-0.5f) == 0.5f);
static_assert(__builtin_fabs(-inf) == inf);
static_assert(__builtin_fabs(-0.0) == 0.0);
static_assert(sizeof(__builtin_inff()) == sizeof(float) && sizeof(__builtin_inf()) == sizeof(double));
constexpr int classify(double x) { return x != x ? 0 : (x == inf || x == -inf) ? 1 : 2; }
static_assert(classify(nan) == 0 && classify(-inf) == 1 && classify(0.5) == 2);
