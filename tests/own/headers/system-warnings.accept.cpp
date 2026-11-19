// #229: warnings from system headers are not reported, as Clang and GCC do: the SDK's <stdio.h>
// compiles with no warning, and so do a header found through -isystem and the one it includes
// from its own directory. A user header's warning is: the one warning reported is at the marker
// in inputs/user-warning.h.
// FLAGS: -isystem inputs/sys
#include <stdio.h>
#include <noisy.h>
#include "inputs/user-warning.h"

int both() { return noisy() + user(); }
