// #132: the x86-64 Linux predefined macros against Clang for the same target.
// FLAGS: --target=x86_64-linux-gnu
// ORACLE: clang
// XFAIL: gcc GCC does not support --target and has different predefined macros
#include "predefined-target.h"
