// #229: the macOS SDK's <stdio.h> compiles with no warning: ycxx defines __GNUC__, so sys/cdefs.h
// no longer warns "Unsupported compiler detected", and a system header's warnings are hidden. The
// one warning reported is this file's own (a test with a warning marker has all its warnings
// checked). On Linux, glibc's <stdio.h> still needs a compiler-provided <stddef.h> (#133).
// REQUIRES: darwin
#include <stdio.h>

int main_file() { return 'op'; } //! warning
