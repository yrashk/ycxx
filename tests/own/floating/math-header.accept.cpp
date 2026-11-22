// [cmath.syn], [c.math]: <math.h> compiles, and its macros HUGE_VAL, HUGE_VALF and INFINITY are
// constant expressions of type double, float and float; its functions have the C signatures.
// XFAIL: ycxx <math.h> is libycxx's wrapper of <cmath>, which does not compile yet (#349)
#include <math.h>

constexpr int kind(float) { return 1; }
constexpr int kind(double) { return 2; }
static_assert(kind(HUGE_VAL) == 2 && kind(HUGE_VALF) == 1 && kind(INFINITY) == 1);
static_assert(HUGE_VAL > 1.7976931348623157e308);
static_assert(HUGE_VALF == INFINITY);
static_assert(kind(NAN) == 1);
double (*sq)(double) = sqrt;
float (*sqf)(float) = sqrtf;
double (*pw)(double, double) = pow;
double (*fl)(double) = floor;
float (*fa)(float) = fabsf;
