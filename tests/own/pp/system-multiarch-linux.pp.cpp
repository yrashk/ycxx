// #252: glibc's features.h reaches target-specific headers, including bits/wordsize.h.
// REQUIRES: linux
// FLAGS: --target=x86_64-pc-linux-gnu
// ORACLE: clang
// XFAIL: gcc GCC does not support --target
#include <features.h>

__WORDSIZE
__TIMESIZE
