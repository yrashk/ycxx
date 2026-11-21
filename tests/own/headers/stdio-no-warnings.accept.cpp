// #229: the macOS SDK's <stdio.h> compiles with no warning: ycxx defines __GNUC__, so sys/cdefs.h
// no longer warns "Unsupported compiler detected", and a system header's warnings are hidden. The
// one warning reported is this file's own (a test with a warning marker has all its warnings
// checked). glibc's <stdio.h> compiles without warnings for the Linux target too, but on Linux
// tools/test checks ycxx's default target, Darwin, until #651.
// REQUIRES: darwin
// PARITY: #651
#include <stdio.h>

int main_file() { return 'op'; } //! warning
