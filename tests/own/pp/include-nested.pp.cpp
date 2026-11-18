// [cpp.include]/6: a #include directive may appear in a file read because of another; outer.h
// includes "inner.h", found next to outer.h (in inputs/), not next to this file.
before;
#include "inputs/outer.h"
after;
