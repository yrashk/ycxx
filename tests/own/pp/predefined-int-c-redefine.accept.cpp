// #657: a C library may define the predefined integer-constant macros again, as glibc's
// <stdint.h> does with GCC's spelling `c ## L`. A redefinition with the same replacement list,
// white-space separation included, is valid ([cpp.replace.general]/2).
// XFAIL: clang Clang predefines c##L, so the redefinition differs and -pedantic-errors rejects it
#if defined(__APPLE__)
#define __INT64_C(c) c ## LL
#define __UINT64_C(c) c ## ULL
#else
#define __INT64_C(c) c ## L
#define __UINT64_C(c) c ## UL
#endif
#define __INTMAX_C(c) c ## L
#define __UINTMAX_C(c) c ## UL

static_assert(__INT64_C(9223372036854775807) == 9223372036854775807);
static_assert(__UINT64_C(18446744073709551615) == 18446744073709551615u);
static_assert(__INTMAX_C(-1) < 0 && __UINTMAX_C(1) > 0);
