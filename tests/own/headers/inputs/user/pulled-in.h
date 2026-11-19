// Found through -I, but included by a system header (inputs/sys/noisy.h): a system header too,
// as with Clang and GCC.
#pragma once
inline int pulled_in() { return 'mn' == 0 ? 0 : 1; }
