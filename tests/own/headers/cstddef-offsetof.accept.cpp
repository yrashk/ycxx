// #248: use offsetof from the pinned libycxx <cstddef> ([support.types.layout]).
// REQUIRES: darwin
// PARITY: by design: header lookup on the Darwin host. See tests/own/headers/cstddef-offsetof-linux.accept.cpp for Linux.
#include "inputs/cstddef-offsetof.hpp"
