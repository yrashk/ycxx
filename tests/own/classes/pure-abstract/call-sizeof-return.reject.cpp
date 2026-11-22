// XFAIL: clang Clang 23 accepts the abstract prvalue operand of sizeof.
#include "abstract.hpp"

Abstract make();
// [expr.sizeof] applies temporary materialization to this prvalue.
static_assert(sizeof(make()) > 0);  //! error
