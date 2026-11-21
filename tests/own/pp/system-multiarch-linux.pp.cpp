// #252: glibc's features.h reaches target-specific headers, including bits/wordsize.h.
// REQUIRES: linux
// PARITY: by design: glibc's headers exist only on Linux hosts
// FLAGS: --target=x86_64-pc-linux-gnu
// ORACLE: clang
// XFAIL: gcc GCC does not support --target
#include <features.h>

__WORDSIZE
__TIMESIZE
