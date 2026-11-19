// #229: warnings from system headers are not reported, as Clang and GCC do: the SDK's <stdio.h>
// compiles with no warning, and so do a header found through -isystem and the headers it
// includes, from its own directory or through -I. A user header's warning is, and so is that of a
// system header's macro used here: the markers below and in inputs/user-warning.h.
// FLAGS: -isystem inputs/sys -I inputs/user
#include <stdio.h>
#include <noisy.h>
#include "inputs/user-warning.h"

int both() { return noisy() + user(); }
int used() { return NOISY_MULTICHAR; } //! warning
