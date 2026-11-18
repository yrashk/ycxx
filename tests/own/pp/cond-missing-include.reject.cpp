// [cpp.include], [cpp.cond]/13: a #include of a missing file in a taken group is an error,
// while the same directive in a skipped group is not.
#if 0
#include "inputs/no-such-header.h"
#endif
#ifdef NOT_DEFINED
#include "inputs/no-such-header.h"
#endif
int before;
#if 1
#include "inputs/no-such-header.h" //! error
#endif
