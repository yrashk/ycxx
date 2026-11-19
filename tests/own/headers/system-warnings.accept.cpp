// #229: warnings from system headers are not reported, as Clang and GCC do: a header found through
// -isystem, the headers it includes (from its own directory or through -I) give none. A user
// header's warning is reported, and so is that of a system header's macro used here: the markers
// below and in inputs/user-warning.h. (The SDK's <stdio.h>: stdio-no-warnings.accept.cpp.)
// FLAGS: -isystem inputs/sys -I inputs/user
#include <noisy.h>
#include "inputs/user-warning.h"

int both() { return noisy() + user(); }
int used() { return NOISY_MULTICHAR; } //! warning
