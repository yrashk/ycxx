// [cpp.cond]/3, /4, /7: #ifdef and `defined` treat __has_include as a defined macro; in a #if
// it is 1 when the named header can be found by the #include search, else 0, for the quoted
// and the angled form, and for an operand that comes from a macro.
// FLAGS: -Iinputs/angle
#ifdef __has_include
has_include_is_defined;
#endif
#if defined(__has_include)
defined_operator;
#endif
#if __has_include("inputs/basic.h")
quoted_found;
#endif
#if __has_include("inputs/no-such-header.h")
not_kept;
#else
quoted_missing;
#endif
#if __has_include(<angle.h>)
angled_found;
#endif
#if __has_include(<no-such-angle-header.h>)
not_kept;
#else
angled_missing;
#endif
#define HEADER "inputs/decls.h"
#if __has_include(HEADER)
macro_operand;
#endif
#if __has_include("inputs/basic.h") && !__has_include("inputs/nope.h")
combined;
#endif
#if __has_include("inputs/basic.h")
#include "inputs/basic.h"
#endif
end FROM_BASIC;
