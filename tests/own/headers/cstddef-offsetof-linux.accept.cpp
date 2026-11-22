// #248: use offsetof from the pinned libycxx <cstddef> on Linux ([support.types.layout]).
// REQUIRES: linux
// FLAGS: --target=x86_64-pc-linux-gnu
// XFAIL: gcc GCC selects its target without --target
#include "inputs/cstddef-offsetof.hpp"
