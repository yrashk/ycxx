// #657: glibc's <stdint.h> for the x86-64 Linux target. glibc redefines __INT64_C and
// __UINT64_C with GCC's spelling, which ycxx's predefined macros must match. (<inttypes.h> is
// blocked separately by libycxx's limit macros, #664.)
// REQUIRES: linux
// PARITY: by design: glibc's headers exist only on Linux hosts
// FLAGS: --target=x86_64-pc-linux-gnu
// XFAIL: gcc GCC selects its target without --target
#include <stdint.h>

static_assert(INT64_MAX == 9223372036854775807);
static_assert(UINT64_MAX == 18446744073709551615u);
static_assert(INTMAX_MIN < 0 && UINTMAX_MAX > 0);
static_assert(sizeof(int64_t) == 8 && sizeof(intptr_t) == sizeof(void*));
int64_t big = INT64_C(-5);
uint64_t ubig = UINT64_C(5);
