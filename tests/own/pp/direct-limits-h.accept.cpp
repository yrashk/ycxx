// #475: the hosted glibc <limits.h> must expose public integer limits and
// preserve their required promoted types ([climits.syn]).
// REQUIRES: linux
// PARITY: by design: glibc's headers exist only on Linux hosts
// FLAGS: --target=x86_64-pc-linux-gnu
// XFAIL: gcc GCC selects its target without --target
#define _POSIX_C_SOURCE 200809L
#include <limits.h>

template<class T, class U> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;

static_assert(CHAR_BIT == 8 && MB_LEN_MAX == 16);
static_assert(_POSIX_PATH_MAX == 256 && PATH_MAX >= _POSIX_PATH_MAX);
static_assert(SCHAR_MIN == -128 && UCHAR_MAX == 255);
static_assert(SHRT_MIN == -32768 && USHRT_MAX == 65535);
static_assert(INT_MIN == -2147483647 - 1 && UINT_MAX == 4294967295U);
static_assert(LLONG_MAX == 9223372036854775807LL && ULLONG_MAX == 18446744073709551615ULL);

static_assert(same<decltype(UCHAR_MAX), int>);
static_assert(same<decltype(USHRT_MAX), int>);
static_assert(same<decltype(UINT_MAX), unsigned int>);
static_assert(same<decltype(ULONG_MAX), unsigned long>);
static_assert(same<decltype(ULLONG_MAX), unsigned long long>);

#if defined(__x86_64__)
static_assert(CHAR_WIDTH == 8 && INT_WIDTH == 32 && LONG_WIDTH == 64 && LLONG_WIDTH == 64);
static_assert(LONG_MIN == -9223372036854775807L - 1);
static_assert(ULONG_MAX == 18446744073709551615UL);
#endif
