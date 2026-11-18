// [cpp.cond]/13: the tokens of a skipped group are not translated, so it may hold text that is
// not C++, unknown directives, #error and a missing #include without making the program
// ill-formed; a declaration repeated in a skipped alternative is not a redefinition.
#if 0
this is not C++ at all
#bogus
#error never
#include "inputs/no-such-header.h"
#else
int kept = 1;
#endif
#ifdef NOT_DEFINED
int kept = 2;
#elif !defined(NOT_DEFINED)
int other() { return kept + 1; }
#endif
