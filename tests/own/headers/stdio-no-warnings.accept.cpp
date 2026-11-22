// #229: the host C library's <stdio.h> compiles with no warning. On macOS ycxx defines __GNUC__,
// so the SDK's sys/cdefs.h no longer warns "Unsupported compiler detected"; on Linux glibc's needs
// the compiler's <stddef.h> (#133); and a system header's warnings are hidden. The one warning
// reported is this file's own (a test with a warning marker has all its warnings checked).
#include <stdio.h>

int main_file() { return 'op'; } //! warning
