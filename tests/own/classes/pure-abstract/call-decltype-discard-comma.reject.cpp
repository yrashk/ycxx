// XFAIL: clang Clang 23 accepts the discarded abstract left operand of comma in decltype.
#include "abstract.hpp"

Abstract make();
// [expr.context] materializes the abstract prvalue discarded by the comma operator.
using Result = decltype((make(), 0));  //! error
