// #475: <float.h> exposes the target floating-point properties ([cfloat.syn]).
// REQUIRES: linux
// FLAGS: --target=x86_64-pc-linux-gnu
// XFAIL: gcc GCC selects its target without --target
#include <float.h>

template<class T, class U> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;

static_assert(FLT_RADIX == 2);
static_assert(FLT_MANT_DIG == 24 && DBL_MANT_DIG == 53);
static_assert(FLT_MIN_EXP == -125 && DBL_MAX_EXP == 1024);
static_assert(FLT_EPSILON == 0.00000011920928955078125F);
static_assert(FLT_MIN > 0 && FLT_MIN < FLT_EPSILON && FLT_MAX > 1);
static_assert(same<decltype(FLT_MAX), float>);
static_assert(same<decltype(DBL_MAX), double>);

#if defined(__x86_64__)
static_assert(LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384);
#ifndef LDBL_MAX
#error <float.h> must define LDBL_MAX
#endif
#endif
