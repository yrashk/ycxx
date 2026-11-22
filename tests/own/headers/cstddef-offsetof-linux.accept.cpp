// #248: use offsetof from the pinned libycxx <cstddef> on Linux ([support.types.layout]).
// REQUIRES: linux
// PARITY: by design: Linux header lookup with an explicit target. See tests/own/headers/cstddef-offsetof.accept.cpp for Darwin.
// FLAGS: --target=x86_64-pc-linux-gnu
// XFAIL: gcc GCC selects its target without --target
#include "inputs/cstddef-offsetof.hpp"
