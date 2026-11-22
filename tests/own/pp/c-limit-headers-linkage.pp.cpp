// #475: the direct limit headers expose macros without declaring anything
// inside the surrounding linkage specification.
// REQUIRES: linux
// PARITY: by design: glibc's headers exist only on Linux hosts
// FLAGS: --target=x86_64-pc-linux-gnu -U_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
// XFAIL: gcc GCC selects its target without --target
// FLAGS: -U_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
extern "C" {
#include <float.h>
#include <limits.h>
}

FLT_RADIX CHAR_BIT
