// #229: a directory given with both -I and -isystem is a system directory, as with Clang and
// GCC, in either order: noisy.h's warnings are not reported. The main file's is.
// FLAGS: -I inputs/sys -I inputs/user -isystem inputs/sys
#include <noisy.h>

int main_file() { return noisy() + 'zz'; } //! warning
