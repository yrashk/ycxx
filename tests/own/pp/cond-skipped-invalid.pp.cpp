// [cpp.cond]/13: a skipped group is processed only through the directive names that track
// nesting, so it may hold unknown directives, #error, #warning, a missing #include, malformed
// #define, #line and #pragma lines, and #if/#elif with invalid expressions, none of which is
// diagnosed or passed through. (It is still tokenized: an unmatched ' or " there is ill-formed,
// see cond-skipped-quote.reject.cpp.)
#if 0
#bogus directive
#error not reported in a skipped group
#include "inputs/no-such-header.h"
#define 123
#undef "string"
#line 0
#line not_a_number
#if )(
#elif 1 / 0
#endif
#else
taken_else;
#endif
#ifdef NOT_DEFINED
#warning not reported either
#pragma not_passed_through
not_kept;
#endif
end;
