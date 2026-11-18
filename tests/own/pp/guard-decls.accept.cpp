// [cpp.cond], [cpp.include], [basic.def.odr]: a guarded header with variable and function definitions and
// a macro, included twice, defines each once, so the program is well-formed.
#include "inputs/guarded.h"
#include "inputs/guarded.h"
int total() { return guarded_value() + guarded_variable + GUARDED_MACRO; }
int doubled = GUARDED_MACRO * 2;
