// #475: direct headers and their C++ counterparts expose the same limits.
// REQUIRES: linux
// FLAGS: --target=x86_64-pc-linux-gnu
// XFAIL: gcc GCC selects its target without --target
#include <float.h>
#include <limits.h>

constexpr int mantissa = LDBL_MANT_DIG;
constexpr float epsilon = FLT_EPSILON;
constexpr unsigned long maximum = ULONG_MAX;

#include <cfloat>
#include <climits>

static_assert(LDBL_MANT_DIG == mantissa && FLT_EPSILON == epsilon);
static_assert(ULONG_MAX == maximum && CHAR_BIT == 8);
